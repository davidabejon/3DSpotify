#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds/thread.h>

#include "ap3ds.h"
#include "artwork.h"
#include "cfg3ds.h"
#include "icons.h"
#include "lang.h"
#include "login5.h"
#include "logo.h"
#include "pair3ds.h"
#include "player_logic.h"
#include "spclient.h"
#include "tabbar.h"
#include "textscreen.h"
#include "thumbs.h"
#include "track.h"
#include "tls3ds.h"
#include "top.h"
#include "ui.h"
#include "view_home.h"
#include "view_player.h"
#include "view_results.h"
#include "view_settings.h"
#include "worker_priority.h"
#include "stb_vorbis.h"

#define SOC_BUFFER 0x100000
#define SEARCH_LIMIT 24
#define SEARCH_QUERY_SIZE 96
/* 8 x 2048 frames is ~370 ms of audio at 44.1 kHz: enough cushion for the
 * decoder to ride out a burst of TLS work in the download thread. */
#define PLAYER_WAVE_BUFFERS 8
#define PLAYER_FRAMES_PER_BUFFER 2048
#define PLAYER_INPUT_CAPACITY (128 * 1024)
/* Decoding steps per frame at most, so input and drawing stay responsive
 * even if the DSP keeps taking buffers. */
#define PLAYER_PUMP_MAX_STEPS 256

typedef struct {
	char host[128];
	char library_token[1024];
	char oauth_token[1280];
	char refresh_token[512];
	time_t oauth_expires_at;
	ap_candidate ap_candidates[AP_MAX_CANDIDATES];
	int ap_candidate_count;
} spotify_session;

typedef struct {
	track_stream stream;
	volatile bool stop;
	volatile bool done;
	int result;
	Thread thread;
} download_job;

typedef struct {
	char query[SEARCH_QUERY_SIZE];
	char host[128];
	char library_token[1024];
	sp_track tracks[SEARCH_LIMIT];
	int count;
	int result;
	char error[192];
	volatile bool cancel_requested;
	volatile bool done;
} search_job;

typedef struct {
	/* An id rather than text so the screen follows a language switch. */
	str_id category;
	char detail[256];
} startup_error;

static search_job search;
static Thread search_thread;
static sp_track playback_queue[SEARCH_LIMIT];
static int playback_queue_count;
static float playback_volume = 0.7f;
static artwork_state cover_art;
/* The last confirmed query, for the home view's repeat button. */
static char home_query[SEARCH_QUERY_SIZE];
/* Who owns the screens: the text screen (bottom) and the artwork (top)
 * for the session start-up, pairing and errors; the tabs once the session
 * is up. */
typedef enum { VIEW_TEXT, VIEW_TABS } screen_view;
static screen_view active_view = VIEW_TEXT;
static app_tab active_tab = TAB_HOME;
/* An applet (the keyboard) drew over both screens. */
static bool screens_lost;

static void frame(void);
static void switch_tab(app_tab tab);

static void secure_clear(void *data, size_t len)
{
	volatile unsigned char *p = data;
	while (len--)
		*p++ = 0;
}

/* Starts with the language saved in the config, else the console's system
 * language when it is Spanish, else English. */
static void language_init(void)
{
	char code[8];
	lang_id lang;
	if (cfg_get("language", code, sizeof(code)) &&
	    lang_from_code(code, &lang)) {
		lang_set(lang);
		return;
	}
	u8 system = CFG_LANGUAGE_EN;
	if (R_SUCCEEDED(cfguInit())) {
		if (R_FAILED(CFGU_GetSystemLanguage(&system)))
			system = CFG_LANGUAGE_EN;
		cfguExit();
	}
	lang_set(system == CFG_LANGUAGE_ES ? LANG_ES : LANG_EN);
}

/* SELECT, or the language button in Settings, switches the language from
 * any screen and remembers the choice. */
static void switch_language(void)
{
	lang_set(lang_next(lang_get()));
	cfg_set("language", lang_code(lang_get()));
}

/* The text screens: SELECT switches the language. Returns true when the
 * caller has to redraw its screen. */
static bool language_toggled(u32 down)
{
	if (!(down & KEY_SELECT))
		return false;
	switch_language();
	return true;
}

static void download_worker(void *arg)
{
	download_job *job = arg;
	job->result = track_stream_download(&job->stream, &job->stop);
	__sync_synchronize();
	job->done = true;
}

static bool ensure_oauth_token(spotify_session *session, char *error,
                               size_t error_size)
{
	if (session->oauth_token[0] &&
	    session->oauth_expires_at > time(NULL) + 60)
		return true;
	if (!session->refresh_token[0]) {
		snprintf(error, error_size, "%s", tr(STR_ERR_NO_REFRESH_CREDENTIAL));
		return false;
	}

	spotify_tokens tokens;
	memset(&tokens, 0, sizeof(tokens));
	if (token_refresh(session->refresh_token, &tokens, error, error_size) != 0) {
		secure_clear(&tokens, sizeof(tokens));
		return false;
	}
	if (!tokens.access_token[0]) {
		snprintf(error, error_size, "%s", tr(STR_ERR_NO_ACCESS_TOKEN));
		secure_clear(&tokens, sizeof(tokens));
		return false;
	}
	if (tokens.refresh_token[0] &&
	    strcmp(tokens.refresh_token, session->refresh_token) != 0) {
		if (!cfg_save_refresh_token(tokens.refresh_token, tokens.scope)) {
			snprintf(error, error_size, "%s",
			         tr(STR_ERR_SAVE_RENEWED_OAUTH));
			secure_clear(&tokens, sizeof(tokens));
			return false;
		}
		snprintf(session->refresh_token, sizeof(session->refresh_token),
		         "%s", tokens.refresh_token);
	}
	snprintf(session->oauth_token, sizeof(session->oauth_token), "%s",
	         tokens.access_token);
	session->oauth_expires_at = tokens.expires_at;
	secure_clear(&tokens, sizeof(tokens));
	return true;
}

static ap_session *open_playback_ap(spotify_session *session, char *error,
                                    size_t error_size)
{
	if (!ensure_oauth_token(session, error, error_size))
		return NULL;

	ap_candidate candidates[AP_MAX_CANDIDATES];
	int count = session->ap_candidate_count;
	if (count > 0)
		memcpy(candidates, session->ap_candidates,
		       (size_t)count * sizeof(candidates[0]));
	else
		count = ap_resolve_list(candidates, AP_MAX_CANDIDATES,
		                        error, error_size);
	if (count < 1)
		return NULL;

	char device_id[64];
	ap_device_id(device_id, sizeof(device_id));
	ap_info info;
	memset(&info, 0, sizeof(info));
	int tried = 0;
	ap_session *ap = ap_login_any(candidates, count, session->oauth_token,
	                              device_id, &info, &tried,
	                              error, error_size);
	if (!ap && info.login_error == 0 && session->ap_candidate_count > 0) {
		count = ap_resolve_list(candidates, AP_MAX_CANDIDATES,
		                        error, error_size);
		if (count > 0) {
			memset(&info, 0, sizeof(info));
			ap = ap_login_any(candidates, count, session->oauth_token,
			                  device_id, &info, &tried, error, error_size);
			if (ap) {
				memcpy(session->ap_candidates, candidates,
				       (size_t)count * sizeof(candidates[0]));
				session->ap_candidate_count = count;
			}
		}
	}
	secure_clear(device_id, sizeof(device_id));
	secure_clear(&info, sizeof(info));
	if (!ap && !error[0])
		snprintf(error, error_size, "%s", tr(STR_ERR_AP_SESSION));
	return ap;
}

/* --- Player ------------------------------------------------------------
 *
 * The audio engine. It is not a screen any more: player_play starts a
 * track, player_pump keeps the DSP fed once per frame from the main loop
 * whichever tab is showing, and the Playing tab (view_player.h) only reads
 * player_snapshot. Starting a track still blocks for its network steps
 * (access point, audio key), showing each stage, as it did before the
 * tabs; once it plays, nothing blocks. */

/* Everything one track holds; zeroed between tracks. */
typedef struct {
	download_job job;
	ndspWaveBuf waves[PLAYER_WAVE_BUFFERS];
	bool wave_submitted[PLAYER_WAVE_BUFFERS];
	uint8_t *input;
	int16_t *audio;
	bool ndsp_ready;
	stb_vorbis *decoder;
	bool decoder_open;
	bool eof;
	bool final_wave_submitted;
	size_t input_len;
	size_t source_pos;
	size_t pcm_frames;
	size_t total_audio_frames;
	size_t completed_audio_frames;
	size_t displayed_played_frames;
	int16_t pcm[PLAYER_FRAMES_PER_BUFFER * 2];
	float **decoded;
	int decoded_samples;
	int decoded_offset;
	stb_vorbis_info info;
} player_stream;

typedef struct {
	player_phase phase;
	int index;              /* into playback_queue */
	unsigned serial;        /* bumped by every player_play */
	str_id stage;           /* while loading */
	char error[256];        /* when failed */
	bool paused;            /* by the user */
	bool held;              /* by the app, while the keyboard is open */
	spotify_session *session;
	player_stream s;
} player_engine;

static player_engine player;
/* The queue came from the results on screen, so the list follows it. */
static bool queue_from_results;

static void set_playback_volume(float volume)
{
	if (volume < 0.0f)
		volume = 0.0f;
	if (volume > 1.0f)
		volume = 1.0f;
	playback_volume = volume;
	if (!player.s.ndsp_ready)
		return;
	float mix[12] = {0};
	mix[0] = mix[1] = playback_volume;
	ndspChnSetMix(0, mix);
}

static void player_stage(str_id stage)
{
	player.stage = stage;
	frame();
}

static void download_stop_join(download_job *job)
{
	if (!job->thread)
		return;
	job->stop = true;
	/* Waiting on the request in flight: the tab says so. */
	if (!job->done)
		player.phase = PLAYER_PHASE_LOADING;
	while (!job->done && aptMainLoop())
		player_stage(STR_PLAYER_DOWNLOAD_CANCELLING);
	threadJoin(job->thread, U64_MAX);
	threadFree(job->thread);
	job->thread = NULL;
}

/* Frees what the current track holds, in this order, and forgets it. */
static void player_release(void)
{
	player_stream *s = &player.s;
	download_stop_join(&s->job);
	if (s->decoder)
		stb_vorbis_close(s->decoder);
	if (s->ndsp_ready) {
		ndspChnWaveBufClear(0);
		ndspExit();
	}
	if (s->job.stream.data)
		secure_clear(s->job.stream.data, s->job.stream.len);
	track_stream_free(&s->job.stream);
	if (s->input) {
		secure_clear(s->input, PLAYER_INPUT_CAPACITY);
		free(s->input);
	}
	if (s->audio)
		linearFree(s->audio);
	secure_clear(s, sizeof(*s));
}

static void player_fail(const char *error)
{
	player_release();
	snprintf(player.error, sizeof(player.error), "%s", error);
	player.phase = PLAYER_PHASE_FAILED;
}

static const sp_track *player_track(void)
{
	return player.phase == PLAYER_PHASE_IDLE
	       ? NULL : &playback_queue[player.index];
}

/* The DSP is paused by the user or by the app. */
static void player_apply_pause(void)
{
	if (player.s.ndsp_ready)
		ndspChnSetPaused(0, player.paused || player.held);
}

/* Starts track `index` of playback_queue: the cover first, then
 * player_pump goes on with the audio once it has arrived. */
static void player_play(spotify_session *session, int index)
{
	if (index < 0 || index >= playback_queue_count)
		return;
	player_release();
	player.session = session;
	player.serial++;
	player.index = index;
	player.paused = false;
	player.error[0] = '\0';
	player.phase = PLAYER_PHASE_LOADING;
	player.stage = STR_PLAYER_ART_LOADING;
	artwork_request(&cover_art, playback_queue[index].art_large);
	/* The list comes back focused on the track playing. */
	if (queue_from_results)
		view_results_focus(index);
}

/* The network steps of a track, once its cover is in. */
static void player_begin_audio(void)
{
	player_stream *s = &player.s;
	const sp_track *selected = &playback_queue[player.index];
	char error[256] = {0};

	player_stage(STR_PLAYER_AUDIO_PREPARING);
	if (R_FAILED(ndspInit())) {
		player_fail(tr(STR_ERR_NDSP));
		return;
	}
	s->ndsp_ready = true;
	ndspChnSetPaused(0, false);

	s->input = malloc(PLAYER_INPUT_CAPACITY);
	s->audio = linearAlloc(PLAYER_FRAMES_PER_BUFFER * 4 * PLAYER_WAVE_BUFFERS);
	if (!s->input || !s->audio) {
		player_fail(tr(STR_ERR_AUDIO_MEMORY));
		return;
	}
	for (int i = 0; i < PLAYER_WAVE_BUFFERS; i++) {
		s->waves[i].data_vaddr = (uint8_t *)s->audio +
		                         i * PLAYER_FRAMES_PER_BUFFER * 4;
		s->waves[i].nsamples = PLAYER_FRAMES_PER_BUFFER;
		s->waves[i].status = NDSP_WBUF_DONE;
	}

	player_stage(STR_AP_AUTHENTICATING);
	ap_session *ap = open_playback_ap(player.session, error, sizeof(error));
	if (!ap) {
		player_fail(error);
		return;
	}

	player_stage(STR_PLAYER_AUDIO_KEY);
	int rc = track_stream_begin_cached(player.session->host,
	                                   player.session->library_token,
	                                   ap, selected, NULL, &s->job.stream,
	                                   error, sizeof(error));
	ap_close(ap);
	if (rc != 0) {
		if (rc == AP_KEY_REFUSED)
			snprintf(error, sizeof(error), "%s", tr(STR_ERR_KEY_REFUSED));
		player_fail(error);
		return;
	}
	s->source_pos = SPOTIFY_OGG_HEADER;

	s->job.thread = threadCreate(download_worker, &s->job, 32 * 1024,
	                             background_priority(), -2, false);
	if (!s->job.thread) {
		player_fail(tr(STR_ERR_DOWNLOAD_THREAD));
		return;
	}

	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
	ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
	set_playback_volume(playback_volume);
	player_apply_pause();
	player.phase = PLAYER_PHASE_PLAYING;
}

static int free_wave_index(ndspWaveBuf waves[PLAYER_WAVE_BUFFERS])
{
	for (int i = 0; i < PLAYER_WAVE_BUFFERS; i++)
		if (waves[i].status == NDSP_WBUF_DONE ||
		    waves[i].status == NDSP_WBUF_FREE)
			return i;
	return -1;
}

static void submit_wave(ndspWaveBuf *wave, size_t frames)
{
	wave->nsamples = (u32)frames;
	DSP_FlushDataCache(wave->data_vaddr,
	                   PLAYER_FRAMES_PER_BUFFER * 4);
	ndspChnWaveBufAdd(0, wave);
}

/* Queues the PCM gathered so far in wave buffer `index`. */
static void submit_pcm(int index)
{
	player_stream *s = &player.s;
	s->waves[index].data_pcm16 =
		(int16_t *)((uint8_t *)s->audio +
		index * PLAYER_FRAMES_PER_BUFFER * 4);
	memcpy(s->waves[index].data_pcm16, s->pcm, s->pcm_frames * 4);
	submit_wave(&s->waves[index], s->pcm_frames);
	s->wave_submitted[index] = true;
	s->total_audio_frames += s->pcm_frames;
	s->pcm_frames = 0;
}

typedef enum {
	PUMP_AGAIN,   /* made progress: go on in this frame */
	PUMP_YIELD,   /* the DSP is full or the download is behind */
	PUMP_END      /* finished or failed: player.phase says which */
} pump_step;

static pump_step pump_end(bool success, const char *error)
{
	if (success) {
		player_release();
		player.phase = PLAYER_PHASE_FINISHED;
	} else {
		player_fail(error);
	}
	return PUMP_END;
}

/* One step of download -> Vorbis -> PCM -> wave buffers. */
static pump_step pump_once(void)
{
	player_stream *s = &player.s;
	char error[256];

	for (int i = 0; i < PLAYER_WAVE_BUFFERS; i++) {
		if (s->wave_submitted[i] && s->waves[i].status == NDSP_WBUF_DONE) {
			s->completed_audio_frames += s->waves[i].nsamples;
			s->wave_submitted[i] = false;
		}
	}

	__sync_synchronize();
	size_t available = s->job.stream.downloaded;
	if (s->job.stream.data && available > s->source_pos &&
	    s->input_len < PLAYER_INPUT_CAPACITY) {
		size_t copy = available - s->source_pos;
		if (copy > PLAYER_INPUT_CAPACITY - s->input_len)
			copy = PLAYER_INPUT_CAPACITY - s->input_len;
		memcpy(s->input + s->input_len, s->job.stream.data + s->source_pos,
		       copy);
		s->input_len += copy;
		s->source_pos += copy;
	}

	if (!s->decoder && s->input_len > 0) {
		int consumed = 0;
		int vorbis_error = 0;
		s->decoder = stb_vorbis_open_pushdata(s->input, (int)s->input_len,
		                                      &consumed, &vorbis_error,
		                                      NULL);
		if (s->decoder) {
			if (consumed < 0 || (size_t)consumed > s->input_len)
				return pump_end(false, tr(STR_ERR_DECODER_OFFSET));
			memmove(s->input, s->input + consumed,
			        s->input_len - (size_t)consumed);
			s->input_len -= (size_t)consumed;
			s->info = stb_vorbis_get_info(s->decoder);
			if ((s->info.channels != 1 && s->info.channels != 2) ||
			    s->info.sample_rate == 0) {
				snprintf(error, sizeof(error),
				         tr(STR_ERR_VORBIS_CHANNELS_FMT), s->info.channels);
				return pump_end(false, error);
			}
			ndspChnSetRate(0, (float)s->info.sample_rate);
			s->decoder_open = true;
		} else if (vorbis_error != VORBIS_need_more_data) {
			snprintf(error, sizeof(error), tr(STR_ERR_VORBIS_OPEN_FMT),
			         vorbis_error);
			return pump_end(false, error);
		}
	}

	int wave_index = free_wave_index(s->waves);
	if (s->pcm_frames == PLAYER_FRAMES_PER_BUFFER) {
		if (wave_index < 0)
			return PUMP_YIELD;
		submit_pcm(wave_index);
		return PUMP_AGAIN;
	}

	if (s->decoded && s->decoded_offset < s->decoded_samples) {
		size_t amount = (size_t)(s->decoded_samples - s->decoded_offset);
		size_t room = PLAYER_FRAMES_PER_BUFFER - s->pcm_frames;
		if (amount > room)
			amount = room;
		for (size_t i = 0; i < amount; i++) {
			for (int channel = 0; channel < 2; channel++) {
				float sample = s->decoded[s->info.channels == 1 ? 0 : channel]
				                         [s->decoded_offset + (int)i];
				if (sample > 1.0f) sample = 1.0f;
				if (sample < -1.0f) sample = -1.0f;
				int value = (int)(sample * 32767.0f);
				s->pcm[(s->pcm_frames + i) * 2 + channel] = (int16_t)value;
			}
		}
		s->decoded_offset += (int)amount;
		s->pcm_frames += amount;
		if (s->decoded_offset == s->decoded_samples) {
			s->decoded = NULL;
			s->decoded_samples = s->decoded_offset = 0;
		}
		return PUMP_AGAIN;
	}

	bool decoder_needs_more = s->decoder_open && s->input_len == 0;
	if (s->decoder && s->input_len > 0) {
		int channels = 0;
		int samples = 0;
		int used = stb_vorbis_decode_frame_pushdata(
		               s->decoder, s->input, (int)s->input_len, &channels,
		               &s->decoded, &samples);
		if (used < 0 || (size_t)used > s->input_len)
			return pump_end(false, tr(STR_ERR_DECODER_BLOCK));
		if (used > 0) {
			memmove(s->input, s->input + used, s->input_len - (size_t)used);
			s->input_len -= (size_t)used;
		}
		if (samples > 0) {
			if (channels != s->info.channels)
				return pump_end(false, tr(STR_ERR_CHANNELS_CHANGED));
			s->decoded_samples = samples;
			s->decoded_offset = 0;
			return PUMP_AGAIN;
		}
		if (used > 0)
			return PUMP_AGAIN;
		decoder_needs_more = true;
	}

	__sync_synchronize();
	available = s->job.stream.downloaded;
	bool no_more_data = s->job.done && s->source_pos >= available &&
	                    (!s->decoder_open || decoder_needs_more);
	if (no_more_data && !s->eof) {
		if (s->job.stream.dl_failed) {
			snprintf(error, sizeof(error), tr(STR_ERR_DOWNLOAD_FAILED_FMT),
			         s->job.stream.dl_err);
			return pump_end(false, error);
		}
		if (!s->decoder_open)
			return pump_end(false, tr(STR_ERR_NO_VORBIS));
		s->input_len = 0;
		s->eof = true;
	}

	if (s->input_len == PLAYER_INPUT_CAPACITY)
		return pump_end(false, tr(STR_ERR_VORBIS_TOO_LARGE));
	if (s->eof && s->pcm_frames == 0 && !s->final_wave_submitted)
		s->final_wave_submitted = true;
	if (s->eof && s->pcm_frames > 0 && !s->final_wave_submitted) {
		wave_index = free_wave_index(s->waves);
		if (wave_index >= 0) {
			submit_pcm(wave_index);
			s->final_wave_submitted = true;
			return PUMP_AGAIN;
		}
	}
	if (s->eof && s->pcm_frames == 0 && s->final_wave_submitted) {
		bool playing = false;
		for (int i = 0; i < PLAYER_WAVE_BUFFERS; i++)
			if (s->waves[i].status == NDSP_WBUF_QUEUED ||
			    s->waves[i].status == NDSP_WBUF_PLAYING)
				playing = true;
		if (!playing)
			return pump_end(s->total_audio_frames > 0, tr(STR_ERR_NO_AUDIO));
	}
	return PUMP_YIELD;
}

/* Once per frame from the main loop, on every tab: decodes until the
 * wave buffers are full or the download is behind, so the frame's wait for
 * VBlank only comes once the DSP has its cushion. */
static void player_pump(void)
{
	if (player.phase == PLAYER_PHASE_LOADING) {
		if (!artwork_pending(&cover_art))
			player_begin_audio();
		return;
	}
	if (player.phase != PLAYER_PHASE_PLAYING)
		return;
	for (int step = 0; step < PLAYER_PUMP_MAX_STEPS; step++)
		if (pump_once() != PUMP_AGAIN)
			break;
}

/* A or the middle button: pause and resume, or play the track again once
 * it has finished or failed. */
static void player_toggle(void)
{
	if (player.phase == PLAYER_PHASE_PLAYING) {
		player.paused = !player.paused;
		player_apply_pause();
	} else if (player.phase == PLAYER_PHASE_FINISHED ||
	           player.phase == PLAYER_PHASE_FAILED) {
		player_play(player.session, player.index);
	}
}

static void player_skip(int direction)
{
	int next = playback_queue_step(player.index, playback_queue_count,
	                               direction);
	if (next >= 0)
		player_play(player.session, next);
}

/* The keyboard takes the main thread over: the DSP would run dry, so the
 * track pauses while it is open. */
static void player_hold(bool hold)
{
	player.held = hold;
	player_apply_pause();
}

/* Stops the track and forgets it: the session is going away. */
static void player_stop(void)
{
	player_release();
	player.phase = PLAYER_PHASE_IDLE;
	player.error[0] = '\0';
}

/* The audio download, or the network steps before it, are running: the
 * thumbnails wait (STYLES.md 9). */
static bool player_downloading(void)
{
	if (player.phase == PLAYER_PHASE_LOADING)
		return true;
	return player.phase == PLAYER_PHASE_PLAYING && !player.s.job.done;
}

static unsigned player_elapsed(void)
{
	player_stream *s = &player.s;
	if (player.phase != PLAYER_PHASE_PLAYING || !s->decoder_open ||
	    !s->info.sample_rate)
		return 0;
	size_t played_frames = s->completed_audio_frames;
	if (!player.paused && !player.held) {
		u16 current_sequence = ndspChnGetWaveBufSeq(0);
		for (int i = 0; i < PLAYER_WAVE_BUFFERS; i++)
			if (s->wave_submitted[i] &&
			    s->waves[i].sequence_id == current_sequence) {
				u32 position = ndspChnGetSamplePos(0);
				if (position < s->waves[i].nsamples)
					played_frames += position;
				break;
			}
		s->displayed_played_frames = played_frames;
	} else {
		played_frames = s->displayed_played_frames;
	}
	return (unsigned)(played_frames / s->info.sample_rate);
}

static player_info player_snapshot(void)
{
	const sp_track *track = player_track();
	player_stream *s = &player.s;
	unsigned percent = s->job.stream.total
	                   ? (unsigned)(s->job.stream.downloaded * 100 /
	                                s->job.stream.total) : 0;
	if ((s->job.done && s->job.stream.dl_done) ||
	    player.phase == PLAYER_PHASE_FINISHED)
		percent = 100;
	unsigned duration = track && track->duration_ms > 0
	                    ? (unsigned)track->duration_ms / 1000 : 0;
	player_info info = {
		.phase = player.phase,
		.track = track,
		.serial = player.serial,
		.index = player.index,
		.count = playback_queue_count,
		.stage = player.stage,
		.paused = player.paused,
		.elapsed_s = player_elapsed(),
		.duration_s = duration,
		.download_pct = percent > 100 ? 100 : percent,
		.volume_pct = (unsigned)(playback_volume * 100.0f + 0.5f),
		.error = player.error,
	};
	return info;
}

/* --- Screens ------------------------------------------------------------ */

/* Renders a GPU frame only when something changed, else just waits for
 * VBlank: rendering shares the main thread with the Vorbis decoder. While a
 * track plays, the clock changes once a second: the Playing tab's bottom
 * screen and the top screen's "now playing" card on the other tabs show
 * it, so that is one GPU frame per second on any tab (the big cover on the
 * Playing tab has no clock). With nothing playing, the top screen's logo
 * costs no frame at all (STYLES.md 9).
 *
 * Both screens are drawn together. Until the F0 spike confirms on hardware
 * that a target left out of a frame keeps its last image, skipping one is
 * not worth the risk; the top screen costs a few rectangles and quads.
 *
 * With the tabs, top.h draws the top screen (logo, card or big cover, and
 * the search bubble), the active tab's view draws the bottom one and the
 * tab bar goes over it; the artwork's own changes only count while the
 * cover is on screen, and switching tabs always repaints. The text screens
 * keep the artwork on top, as before. */
static void frame(void)
{
	/* 0: the text screen; 1 + tab: that tab. */
	static int drawn_screen = -1;
	artwork_poll(&cover_art);
	bool restored = ui_take_restored() || screens_lost;
	screens_lost = false;
	bool tabs = active_view == VIEW_TABS;
	int screen = tabs ? 1 + (int)active_tab : 0;
	bool changed;
	if (!tabs) {
		changed = artwork_dirty(&cover_art) || textscreen_dirty();
	} else {
		player_info info = player_snapshot();
		view_player_set(&info);
		top_set(&info);
		changed = tabbar_dirty() || top_dirty(active_tab, &cover_art);
		switch (active_tab) {
		case TAB_HOME:
			changed |= view_home_dirty();
			break;
		case TAB_SEARCH:
			changed |= view_results_dirty();
			break;
		case TAB_PLAYER:
			changed |= view_player_dirty();
			break;
		default:
			changed |= view_settings_dirty();
			break;
		}
	}
	if (!restored && screen == drawn_screen && !changed) {
		gspWaitForVBlank();
		return;
	}
	ui_begin();
	ui_scene(GFX_TOP);
	if (!tabs)
		artwork_draw(&cover_art);
	else
		top_draw(active_tab, &cover_art);
	ui_scene(GFX_BOTTOM);
	if (!tabs) {
		textscreen_draw();
	} else {
		switch (active_tab) {
		case TAB_HOME:
			view_home_draw_bottom();
			break;
		case TAB_SEARCH:
			view_results_draw_bottom();
			break;
		case TAB_PLAYER:
			view_player_draw_bottom();
			break;
		default:
			view_settings_draw_bottom();
			break;
		}
		tabbar_draw();
	}
	ui_end();
	drawn_screen = screen;
}

static void show_progress(const char *message)
{
	textscreen_clear();
	textscreen_printf("%s\n\n%s\n", tr(STR_LOGIN_TITLE), message);
	frame();
}

/* --- Search ------------------------------------------------------------ */

static void search_worker(void *arg)
{
	search_job *job = arg;
	char uris[SEARCH_LIMIT][64];
	const char *uri_ptrs[SEARCH_LIMIT];
	char error[sizeof(job->error)] = {0};

	job->count = sp_search_tracks(job->host, job->library_token,
	                              job->query, uris, SEARCH_LIMIT,
	                              error, sizeof(error));
	if (!job->cancel_requested && job->count >= 0 && job->count > 0) {
		for (int i = 0; i < job->count; i++)
			uri_ptrs[i] = uris[i];
		job->count = sp_track_metadata_batch(job->host,
		                                     job->library_token,
		                                     uri_ptrs, job->count,
		                                     job->tracks, error,
		                                     sizeof(error));
	}
	if (job->cancel_requested) {
		job->result = -2;
	} else if (job->count < 0) {
		snprintf(job->error, sizeof(job->error), "%s",
		         error[0] ? error : tr(STR_ERR_SEARCH_LOAD));
		job->result = -1;
	} else {
		job->result = 0;
	}
	secure_clear(uris, sizeof(uris));
	secure_clear(uri_ptrs, sizeof(uri_ptrs));
	secure_clear(error, sizeof(error));
	__sync_synchronize();
	job->done = true;
}

/* What to suggest for a failed search, from the HTTP status in `error`. */
static str_id search_error_advice(const char *error)
{
	if (strstr(error, "HTTP 401"))
		return STR_SEARCH_HTTP_401;
	if (strstr(error, "HTTP 403"))
		return STR_SEARCH_HTTP_403;
	if (strstr(error, "HTTP 429"))
		return STR_SEARCH_HTTP_429;
	return STR_SEARCH_CHECK_WIFI;
}

static void search_join(void)
{
	threadJoin(search_thread, U64_MAX);
	threadFree(search_thread);
	search_thread = NULL;
	secure_clear(search.library_token, sizeof(search.library_token));
	secure_clear(search.host, sizeof(search.host));
}

/* Cancels a running search and waits for its request in flight. The
 * wait blocks the decoder, so the track pauses meanwhile instead of
 * running dry. */
static void search_stop(void)
{
	if (!search_thread)
		return;
	search.cancel_requested = true;
	view_results_cancelling();
	bool held = player.held;
	player_hold(true);
	search_join();
	player_hold(held);
	view_results_idle();
}

/* Starts searching `query` in the background: the Search tab shows it
 * running, and the main loop, the audio included, goes on meanwhile. */
static void search_start(const spotify_session *session, const char *query)
{
	search_stop();
	memset(&search, 0, sizeof(search));
	snprintf(search.query, sizeof(search.query), "%s", query);
	snprintf(search.host, sizeof(search.host), "%s", session->host);
	snprintf(search.library_token, sizeof(search.library_token), "%s",
	         session->library_token);
	/* The tracks in the list are about to change under the queue's
	 * copy. */
	queue_from_results = false;
	view_results_searching();
	/* Below the decoder, like every worker that can overlap playback. */
	search_thread = threadCreate(search_worker, &search, 16 * 1024,
	                             background_priority(), -2, false);
	if (!search_thread) {
		snprintf(search.error, sizeof(search.error), "%s",
		         tr(STR_ERR_SEARCH_THREAD));
		secure_clear(search.library_token, sizeof(search.library_token));
		view_results_error(search_error_advice(search.error));
	}
}

/* Once per frame: shows a finished search. */
static void search_poll(void)
{
	if (!search_thread || !search.done)
		return;
	__sync_synchronize();
	search_join();
	if (search.cancel_requested) {
		view_results_idle();
		/* Cancelled from its tab: back home, as before the tabs. */
		if (active_tab == TAB_SEARCH)
			switch_tab(TAB_HOME);
	} else if (search.result == 0) {
		view_results_show(search.tracks, search.count, 0);
	} else {
		view_results_error(search_error_advice(search.error));
	}
}

/* Only a confirmed query replaces `query`: cancelling the keyboard keeps the
 * last search for the home view's repeat chip. */
static bool ask_search_query(char query[SEARCH_QUERY_SIZE])
{
	char typed[SEARCH_QUERY_SIZE] = {0};
	SwkbdState keyboard;
	swkbdInit(&keyboard, SWKBD_TYPE_WESTERN, 1, SEARCH_QUERY_SIZE - 1);
	swkbdSetValidation(&keyboard, SWKBD_NOTEMPTY_NOTBLANK,
	                   SWKBD_FILTER_PROFANITY, -1);
	swkbdSetHintText(&keyboard, tr(STR_SEARCH_HINT));
	/* The applet blocks this thread, and with it the decoder. */
	player_hold(true);
	bool confirmed = swkbdInputText(&keyboard, typed, sizeof(typed)) ==
	                 SWKBD_BUTTON_CONFIRM;
	player_hold(false);
	if (confirmed)
		memcpy(query, typed, sizeof(typed));
	/* The keyboard applet takes over both screens. */
	artwork_invalidate(&cover_art);
	textscreen_invalidate();
	screens_lost = true;
	return confirmed;
}

/* The keyboard, then the search on its tab. */
static void keyboard_search(const spotify_session *session,
                            char query[SEARCH_QUERY_SIZE])
{
	if (!ask_search_query(query) || !query[0])
		return;
	search_start(session, query);
	switch_tab(TAB_SEARCH);
}

static void prepare_queue(void)
{
	playback_queue_count = search.count;
	memcpy(playback_queue, search.tracks,
	       (size_t)playback_queue_count * sizeof(playback_queue[0]));
}

/* Plays the results from `index`, straight from the list: there is no
 * selection screen in between. The rest of the results are the queue, and
 * the list follows it. */
static void play_from_results(spotify_session *session, int index)
{
	/* The thumbnails must not compete with the audio download
	 * (STYLES.md 9); they resume, kept, once it is done. */
	view_results_allow_thumbs(false);
	prepare_queue();
	queue_from_results = true;
	player_play(session, index);
	switch_tab(TAB_PLAYER);
}

/* --- Tabs -------------------------------------------------------------- */

/* Shows `tab`: its view starts with no press in progress. */
static void switch_tab(app_tab tab)
{
	active_tab = tab;
	tabbar_set_active(tab);
	switch (tab) {
	case TAB_HOME:
		view_home_enter(home_query);
		break;
	case TAB_SEARCH:
		view_results_enter();
		break;
	case TAB_PLAYER:
		view_player_enter();
		break;
	default:
		view_settings_enter();
		break;
	}
}

/* Where the tabs go when they end. */
typedef enum {
	TABS_EXIT,       /* close the app */
	TABS_RECONNECT,  /* start the session again */
	TABS_PAIR        /* pair another account */
} tabs_exit;

/* The app once the session is up: one loop for every tab, so the audio
 * keeps playing while the user moves between them. */
static tabs_exit run_tabs(spotify_session *session)
{
	active_view = VIEW_TABS;
	tabbar_reset(TAB_HOME);
	switch_tab(TAB_HOME);
	for (;;) {
		hidScanInput();
		u32 down = hidKeysDown();
		u32 held = hidKeysHeld();
		u32 up = hidKeysUp();
		if (down & KEY_START)
			return TABS_EXIT;

		/* The audio first: the DSP's cushion is ~370 ms. */
		player_pump();
		search_poll();
		/* Not stopped on leaving the tab: joining the worker would block
		 * this thread, and the audio with it, for a whole request. It
		 * only fetches the rows on screen and the next ones anyway. */
		view_results_allow_thumbs(!player_downloading());

		int next = tabbar_update(down, held, up);
		if (next >= 0) {
			/* The button that switched tabs is spent. */
			down &= ~(u32)(KEY_L | KEY_R | KEY_ZL | KEY_ZR);
			if (next != (int)active_tab)
				switch_tab((app_tab)next);
		}

		switch (active_tab) {
		case TAB_HOME:
			switch (view_home_update(down)) {
			case HOME_ACTION_SEARCH:
				keyboard_search(session, home_query);
				break;
			case HOME_ACTION_REPEAT:
				search_start(session, home_query);
				switch_tab(TAB_SEARCH);
				break;
			case HOME_ACTION_LANGUAGE:
				/* The view shows the new language by itself. */
				switch_language();
				break;
			default:
				break;
			}
			break;
		case TAB_SEARCH: {
			/* Feedback only while searching: cancelling is decided
			 * here. */
			results_action action = view_results_update(down);
			if (search_thread && !search.cancel_requested &&
			    (down & KEY_B)) {
				search.cancel_requested = true;
				view_results_cancelling();
			}
			switch (action) {
			case RESULTS_ACTION_OPEN:
				play_from_results(session, view_results_selected());
				break;
			case RESULTS_ACTION_BACK:
				switch_tab(TAB_HOME);
				break;
			case RESULTS_ACTION_NEW_SEARCH:
				keyboard_search(session, home_query);
				break;
			case RESULTS_ACTION_RETRY:
				search_start(session, home_query);
				break;
			case RESULTS_ACTION_LANGUAGE:
				switch_language();
				break;
			default:
				break;
			}
			break;
		}
		case TAB_PLAYER:
			switch (view_player_update(down)) {
			case PLAYER_ACTION_TOGGLE:
				player_toggle();
				break;
			case PLAYER_ACTION_PREV:
				player_skip(-1);
				break;
			case PLAYER_ACTION_NEXT:
				player_skip(1);
				break;
			case PLAYER_ACTION_VOLUME_UP:
				set_playback_volume(playback_volume + 0.1f);
				break;
			case PLAYER_ACTION_VOLUME_DOWN:
				set_playback_volume(playback_volume - 0.1f);
				break;
			case PLAYER_ACTION_BACK:
				switch_tab(TAB_SEARCH);
				break;
			case PLAYER_ACTION_LANGUAGE:
				switch_language();
				break;
			default:
				break;
			}
			break;
		default:
			switch (view_settings_update(down)) {
			case SETTINGS_ACTION_LANGUAGE:
				/* The language button shows the new one by itself. */
				switch_language();
				break;
			case SETTINGS_ACTION_RECONNECT:
				return TABS_RECONNECT;
			case SETTINGS_ACTION_PAIR:
				return TABS_PAIR;
			case SETTINGS_ACTION_BACK:
				switch_tab(TAB_HOME);
				break;
			default:
				break;
			}
			break;
		}

		frame();
		if (!aptMainLoop())
			return TABS_EXIT;
	}
}

/* Leaving the tabs: no audio, search or thumbnails survive the session. */
static void stop_tabs(void)
{
	player_stop();
	search_stop();
	thumbs_stop();
	active_view = VIEW_TEXT;
}

static void show_pair_screen(const pair_request *req, int interval,
                             const char *status)
{
	textscreen_clear();
	textscreen_printf("%s\n\n", tr(STR_PAIR_TITLE));
	textscreen_printf("%s\n%s\n\n", tr(STR_PAIR_OPEN_URL), req->verification_uri);
	textscreen_printf("%s: %s\n", tr(STR_LABEL_CODE), req->user_code);
	if (req->verification_uri_complete[0])
		textscreen_printf("\n%s\n%s\n", tr(STR_PAIR_DIRECT_LINK),
		                  req->verification_uri_complete);
	textscreen_printf("\n%s\n", status);
	textscreen_printf(tr(STR_PAIR_POLL_FMT), interval);
	textscreen_printf("\n%s\n", tr(STR_KEYS_LANGUAGE));
}

static bool pair_account(spotify_tokens *tokens, startup_error *error)
{
	pair_request req;
	char detail[192] = {0};
	show_progress(tr(STR_PAIR_REQUESTING));
	if (pair_begin(&req, detail, sizeof(detail)) != 0) {
		error->category = STR_CAT_PAIRING;
		snprintf(error->detail, sizeof(error->detail), "%s", detail);
		secure_clear(&req, sizeof(req));
		return false;
	}

	int interval = req.interval_s > 0 ? req.interval_s : 5;
	time_t deadline = time(NULL) + req.expires_in_s;
	char status[192];
	snprintf(status, sizeof(status), "%s", tr(STR_PAIR_WAITING));
	while (aptMainLoop()) {
		show_pair_screen(&req, interval, status);
		for (int i = 0; i < interval * 60; i++) {
			hidScanInput();
			u32 down = hidKeysDown();
			if (down & KEY_B) {
				error->category = STR_CAT_PAIR_CANCELLED;
				snprintf(error->detail, sizeof(error->detail), "%s",
				         tr(STR_PAIR_CREDENTIALS_UNCHANGED));
				secure_clear(&req, sizeof(req));
				return false;
			}
			if (language_toggled(down))
				show_pair_screen(&req, interval, status);
			frame();
			if (!aptMainLoop()) {
				secure_clear(&req, sizeof(req));
				return false;
			}
		}
		if (time(NULL) >= deadline) {
			error->category = STR_CAT_PAIR_EXPIRED;
			snprintf(error->detail, sizeof(error->detail), "%s",
			         tr(STR_PAIR_RETRY_NEW_CODE));
			secure_clear(&req, sizeof(req));
			return false;
		}

		detail[0] = '\0';
		pair_status result = pair_poll(&req, tokens, detail, sizeof(detail));
		if (result == PAIR_PENDING) {
			snprintf(status, sizeof(status), "%s%s%s",
			         tr(STR_PAIR_STILL_WAITING), detail[0] ? " " : "",
			         detail);
			continue;
		}
		if (result == PAIR_SLOW_DOWN) {
			interval += 5;
			snprintf(status, sizeof(status), tr(STR_PAIR_SLOW_DOWN_FMT),
			         interval);
			continue;
		}
		if (result == PAIR_OK) {
			if (!tokens->refresh_token[0] ||
			    !cfg_save_refresh_token(tokens->refresh_token, tokens->scope)) {
				error->category = STR_CAT_SAVE_CREDENTIALS;
				snprintf(error->detail, sizeof(error->detail), "%s",
				         tr(STR_CHECK_SD_SPACE));
				secure_clear(tokens, sizeof(*tokens));
				secure_clear(&req, sizeof(req));
				return false;
			}
			secure_clear(&req, sizeof(req));
			return true;
		}

		error->category = result == PAIR_DENIED
		                  ? STR_CAT_PAIR_DENIED
		                  : result == PAIR_EXPIRED
		                    ? STR_CAT_PAIR_EXPIRED
		                    : STR_CAT_PAIR_FAILED;
		snprintf(error->detail, sizeof(error->detail), "%s",
		         detail[0] ? detail : tr(STR_NO_VALID_RESPONSE));
		secure_clear(tokens, sizeof(*tokens));
		secure_clear(&req, sizeof(req));
		return false;
	}
	secure_clear(&req, sizeof(req));
	error->category = STR_CAT_PAIR_INTERRUPTED;
	snprintf(error->detail, sizeof(error->detail), "%s",
	         tr(STR_APP_CLOSING));
	return false;
}

static bool resolve_and_login(spotify_tokens *tokens, startup_error *error,
                              spotify_session *session)
{
	char detail[192] = {0};
	char device_id[64];
	ap_device_id(device_id, sizeof(device_id));

	ap_candidate candidates[AP_MAX_CANDIDATES];
	show_progress(tr(STR_AP_SEARCHING));
	int count = ap_resolve_list(candidates, AP_MAX_CANDIDATES,
	                            detail, sizeof(detail));
	if (count < 1) {
		error->category = STR_CAT_AP_RESOLVE;
		snprintf(error->detail, sizeof(error->detail),
		         tr(STR_CHECK_NETWORK_FMT), detail);
		secure_clear(tokens, sizeof(*tokens));
		return false;
	}

	ap_info info;
	memset(&info, 0, sizeof(info));
	int tried = 0;
	show_progress(tr(STR_AP_AUTHENTICATING));
	ap_session *ap = ap_login_any(candidates, count, tokens->access_token,
	                              device_id, &info, &tried,
	                              detail, sizeof(detail));
	if (ap) {
		snprintf(session->oauth_token, sizeof(session->oauth_token), "%s",
		         tokens->access_token);
		snprintf(session->refresh_token, sizeof(session->refresh_token), "%s",
		         tokens->refresh_token);
		session->oauth_expires_at = tokens->expires_at;
		memcpy(session->ap_candidates, candidates,
		       (size_t)count * sizeof(candidates[0]));
		session->ap_candidate_count = count;
	}
	secure_clear(tokens, sizeof(*tokens));
	if (!ap) {
		error->category = info.login_error
		                  ? STR_CAT_AP_REJECTED
		                  : STR_CAT_AP_CONNECT;
		if (info.login_error)
			snprintf(error->detail, sizeof(error->detail),
			         tr(STR_ERROR_CODE_FMT),
			         ap_login_error_text(info.login_error), info.login_error);
		else
			snprintf(error->detail, sizeof(error->detail), "%s",
			         detail[0] ? detail : tr(STR_NO_AP_RESPONDED));
		secure_clear(&info, sizeof(info));
		return false;
	}

	login5_token library;
	show_progress(tr(STR_LIBRARY_OPENING));
	int rc = login5_from_credentials(info.username, info.reusable,
	                                 info.reusable_len, device_id,
	                                 &library, detail, sizeof(detail));
	ap_close(ap);
	secure_clear(&info, sizeof(info));
	secure_clear(device_id, sizeof(device_id));
	if (rc != 0) {
		error->category = strstr(detail, "login5 refused:")
		                  ? STR_CAT_LOGIN_REFUSED
		                  : STR_CAT_LOGIN_FAILED;
		snprintf(error->detail, sizeof(error->detail), "%s", detail);
		secure_clear(&library, sizeof(library));
		return false;
	}

	detail[0] = '\0';
	show_progress(tr(STR_SERVER_RESOLVING));
	rc = sp_resolve(session->host, sizeof(session->host),
	                 detail, sizeof(detail));
	if (rc != 0 || !session->host[0]) {
		error->category = STR_CAT_SERVER_RESOLVE;
		snprintf(error->detail, sizeof(error->detail), "%s",
		         detail[0] ? detail : tr(STR_INVALID_SERVER_RESPONSE));
		secure_clear(&library, sizeof(library));
		return false;
	}
	snprintf(session->library_token, sizeof(session->library_token), "%s",
	         library.access_token);
	secure_clear(&library, sizeof(library));
	return true;
}

static bool start_session(bool force_pair, startup_error *error,
                          spotify_session *session)
{
	char stored[512] = {0};
	spotify_tokens tokens;
	memset(&tokens, 0, sizeof(tokens));

	if (force_pair || !cfg_load_refresh_token(stored, sizeof(stored))) {
		secure_clear(stored, sizeof(stored));
		if (!pair_account(&tokens, error))
			return false;
		return resolve_and_login(&tokens, error, session);
	}

	char detail[192] = {0};
	show_progress(tr(STR_CREDENTIAL_RENEWING));
	if (token_refresh(stored, &tokens, detail, sizeof(detail)) != 0) {
		secure_clear(stored, sizeof(stored));
		secure_clear(&tokens, sizeof(tokens));
		if (strstr(detail, "invalid_grant")) {
			textscreen_clear();
			textscreen_printf("%s\n", tr(STR_CREDENTIAL_EXPIRED));
			textscreen_printf("%s\n", tr(STR_PAIR_AGAIN_NEEDED));
			for (int i = 0; i < 90 && aptMainLoop(); i++)
				frame();
			if (!pair_account(&tokens, error))
				return false;
			return resolve_and_login(&tokens, error, session);
		}
		error->category = STR_CAT_OAUTH_RENEW;
		snprintf(error->detail, sizeof(error->detail), "%s",
		         detail[0] ? detail : tr(STR_NO_VALID_RESPONSE));
		return false;
	}

	if (tokens.refresh_token[0] &&
	    strcmp(tokens.refresh_token, stored) != 0 &&
	    !cfg_save_refresh_token(tokens.refresh_token, tokens.scope)) {
		error->category = STR_CAT_SAVE_RENEWED;
		snprintf(error->detail, sizeof(error->detail), "%s",
		         tr(STR_CHECK_SD_SPACE));
		secure_clear(stored, sizeof(stored));
		secure_clear(&tokens, sizeof(tokens));
		return false;
	}
	if (!tokens.refresh_token[0])
		snprintf(tokens.refresh_token, sizeof(tokens.refresh_token), "%s",
		         stored);
	secure_clear(stored, sizeof(stored));
	return resolve_and_login(&tokens, error, session);
}

static void show_error(const startup_error *error)
{
	textscreen_clear();
	textscreen_printf("%s\n\n", tr(STR_SESSION_FAILED_TITLE));
	textscreen_printf("%s\n\n", tr(error->category));
	textscreen_printf("%s\n\n", error->detail);
	textscreen_printf("%s\n", tr(STR_KEYS_STARTUP_RETRY));
	textscreen_printf("%s\n", tr(STR_KEYS_EXIT));
	textscreen_printf("%s\n", tr(STR_KEYS_LANGUAGE));
}

int main(int argc, char **argv)
{
	(void)argc;
	gfxInitDefault();
	/* Without romfs the logo falls back to the app name as text. */
	bool romfs_ready = R_SUCCEEDED(romfsInit());
	/* Without the renderer there is no way to show anything at all. */
	if (!ui_init()) {
		if (romfs_ready)
			romfsExit();
		gfxExit();
		return 1;
	}
	/* Without icons, thumbnails or the logo the views still work, just
	 * without them. */
	icons_init();
	thumbs_init();
	if (romfs_ready)
		logo_load();
	if (!textscreen_init() || !view_home_init() || !view_results_init() ||
	    !view_player_init() || !view_settings_init() || !top_init()) {
		top_exit();
		view_settings_exit();
		view_player_exit();
		view_results_exit();
		view_home_exit();
		logo_free();
		thumbs_exit();
		icons_exit();
		textscreen_exit();
		ui_exit();
		if (romfs_ready)
			romfsExit();
		gfxExit();
		return 1;
	}
	cfg_set_path((argv && argv[0]) ? argv[0] : NULL);
	language_init();

	u32 *soc_buffer = memalign(0x1000, SOC_BUFFER);
	bool soc_ready = false;
	bool ps_ready = false;
	bool tls_ready = false;
	startup_error error = {0};
	spotify_session session = {0};

	if (!soc_buffer || R_FAILED(socInit(soc_buffer, SOC_BUFFER))) {
		error.category = STR_CAT_NO_NETWORK;
		snprintf(error.detail, sizeof(error.detail), "%s",
		         tr(STR_CHECK_WIFI_CONNECTED));
		goto failed;
	}
	soc_ready = true;
	if (R_FAILED(psInit())) {
		error.category = STR_CAT_PS_INIT;
		snprintf(error.detail, sizeof(error.detail), "%s",
		         tr(STR_PS_ENTROPY));
		goto failed;
	}
	ps_ready = true;

	{
		int rc = tls_global_init();
		if (rc != 0) {
			char reason[128];
			tls_strerror(rc, reason, sizeof(reason));
			error.category = STR_CAT_TLS_INIT;
			snprintf(error.detail, sizeof(error.detail), "%s", reason);
			goto failed;
		}
	}
	tls_ready = true;

	bool force_pair = false;
	for (;;) {
		active_view = VIEW_TEXT;
		textscreen_clear();
		textscreen_printf("%s\n\n", tr(STR_LOGIN_TITLE));
		textscreen_printf("%s\n", tr(STR_LOGIN_CONNECTING));
		frame();
		if (start_session(force_pair, &error, &session)) {
			tabs_exit result = run_tabs(&session);
			/* Reconnecting or pairing starts a new session from
			 * scratch: nothing of this one survives. */
			stop_tabs();
			secure_clear(&session, sizeof(session));
			if (result == TABS_EXIT)
				goto done;
			force_pair = result == TABS_PAIR;
		} else {
			secure_clear(&session, sizeof(session));
			show_error(&error);
			for (;;) {
				hidScanInput();
				u32 down = hidKeysDown();
				if (down & KEY_START)
					goto done;
				if (down & KEY_A) {
					force_pair = false;
					break;
				}
				if (down & KEY_Y) {
					force_pair = true;
					break;
				}
				if (language_toggled(down))
					show_error(&error);
				frame();
				if (!aptMainLoop())
					goto done;
			}
		}
	}

failed:
	show_error(&error);
	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown();
		if (down & KEY_START)
			break;
		if (language_toggled(down))
			show_error(&error);
		frame();
	}

done:
	stop_tabs();
	artwork_close(&cover_art);
	secure_clear(&session, sizeof(session));
	secure_clear(&search, sizeof(search));
	secure_clear(playback_queue, sizeof(playback_queue));
	if (tls_ready)
		tls_global_exit();
	if (ps_ready)
		psExit();
	if (soc_ready)
		socExit();
	free(soc_buffer);
	top_exit();
	view_settings_exit();
	view_player_exit();
	view_results_exit();
	view_home_exit();
	logo_free();
	thumbs_exit();
	icons_exit();
	textscreen_exit();
	ui_exit();
	if (romfs_ready)
		romfsExit();
	gfxExit();
	return 0;
}
