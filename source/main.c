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
#include "login_logic.h"
#include "login5.h"
#include "logo.h"
#include "pair3ds.h"
#include "player_logic.h"
#include "power_logic.h"
#include "spclient.h"
#include "tabbar.h"
#include "thumbs.h"
#include "track.h"
#include "tls3ds.h"
#include "top.h"
#include "ui.h"
#include "view_home.h"
#include "view_login.h"
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
/* Who owns the screens: the login view for the session start-up, pairing
 * and errors; the tabs once the session is up. */
typedef enum { VIEW_LOGIN, VIEW_TABS } screen_view;
static screen_view active_view = VIEW_LOGIN;
static app_tab active_tab = TAB_HOME;
/* An applet (the keyboard) drew over both screens. */
static bool screens_lost;

/* Closing the lid over music darkens the screens instead of sleeping. */
static bool ptmu_ready;
static bool lcd_ready;
static bool screens_dark;
/* NDM drops the Wi-Fi when the lid shuts, sleep or not, unless the app
 * holds the connection in the exclusive infrastructure state. */
static bool ndm_ready;
static bool wifi_held;

static void wifi_hold(bool hold)
{
	if (!ndm_ready || hold == wifi_held)
		return;
	if (hold) {
		if (R_FAILED(NDMU_EnterExclusiveState(
		        NDM_EXCLUSIVE_STATE_INFRASTRUCTURE)))
			return;
		NDMU_LockState();
	} else {
		NDMU_UnlockState();
		NDMU_LeaveExclusiveState();
	}
	wifi_held = hold;
}

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
 * player_snapshot. A track's network steps (token, access point, audio
 * key) run in a preparation thread that player_pump polls, so the input
 * and the screens keep answering while they take their time. Only a
 * session going away waits for them. */

/* Everything one track holds; zeroed between tracks. */
typedef struct {
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
	/* This track's preparation thread has been started. */
	bool preparing;
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

/* The DSP is set up once and kept for the whole run: starting it again for
 * every track meant starting it with the lid already shut whenever the
 * user closed it during loading, and then the track never sounded. */
static bool ndsp_up;

static bool audio_open(void)
{
	if (!ndsp_up)
		ndsp_up = R_SUCCEEDED(ndspInit());
	return ndsp_up;
}

/* A track's network steps, off the main thread: everything that may wait
 * on the network before the first byte of audio. One at a time: a skipped
 * track's preparation is left to finish ("abandoned") and reaped, and the
 * next one only starts after it, so two never touch the session at once. */
typedef struct {
	spotify_session *session;
	sp_track track;          /* a copy: the queue may change meanwhile */
	track_stream stream;     /* out, taken by the player when it is used */
	char error[256];
	int rc;                  /* 0, AP_KEY_REFUSED or -1 */
	volatile str_id stage;   /* what it is doing, for the Playing tab */
	volatile bool done;
	bool abandoned;          /* main thread: nobody wants the result */
	Thread thread;
} prepare_job;
static prepare_job prep;
#define PREPARE_STACK (64 * 1024)

/* The current track's audio download. Not part of player_stream: its
 * worker holds this address, and a skipped track's download is left to
 * notice `stop` ("abandoned") and reaped later rather than waited for.
 * The next track only starts preparing once it is gone, so there is never
 * more than one, nor two audio buffers at once. */
static download_job dl;
static bool dl_abandoned;

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

/* Joins the download (it has returned, or never started) and frees its
 * buffer. */
static void download_reap(void)
{
	__sync_synchronize();
	if (dl.thread) {
		threadJoin(dl.thread, U64_MAX);
		threadFree(dl.thread);
	}
	if (dl.stream.data)
		secure_clear(dl.stream.data, dl.stream.len);
	track_stream_free(&dl.stream);
	secure_clear(&dl, sizeof(dl));
	dl_abandoned = false;
}

/* The session is going away: the download must be over. */
static void download_wait(void)
{
	dl.stop = true;
	while (dl.thread && !dl.done && aptMainLoop())
		player_stage(STR_PLAYER_DOWNLOAD_CANCELLING);
	download_reap();
}

static void prepare_worker(void *arg)
{
	prepare_job *job = arg;
	job->stage = STR_AP_AUTHENTICATING;
	ap_session *ap = open_playback_ap(job->session, job->error,
	                                  sizeof(job->error));
	if (!ap) {
		job->rc = -1;
	} else {
		job->stage = STR_PLAYER_AUDIO_KEY;
		job->rc = track_stream_begin_cached(job->session->host,
		                                    job->session->library_token,
		                                    ap, &job->track, NULL,
		                                    &job->stream, job->error,
		                                    sizeof(job->error));
		ap_close(ap);
	}
	__sync_synchronize();
	job->done = true;
}

/* Joins a finished preparation (it costs nothing then) and frees whatever
 * of its result nobody took. */
static void prepare_reap(void)
{
	__sync_synchronize();
	threadJoin(prep.thread, U64_MAX);
	threadFree(prep.thread);
	track_stream_free(&prep.stream);
	secure_clear(&prep, sizeof(prep));
}

/* The session is going away: the preparation must be over before it is
 * cleared. The only place that waits for it. */
static void prepare_wait(void)
{
	while (prep.thread && !prep.done && aptMainLoop())
		player_stage(STR_PLAYER_DOWNLOAD_CANCELLING);
	if (prep.thread)
		prepare_reap();
}

/* Frees what the current track holds, in this order, and forgets it. */
static void player_release(void)
{
	player_stream *s = &player.s;
	/* A preparation still on the network is not waited for: it is left
	 * to finish and reaped by player_pump. */
	if (prep.thread) {
		if (prep.done)
			prepare_reap();
		else
			prep.abandoned = true;
	}
	/* Same for the download: asked to stop, it is reaped by player_pump
	 * once its request in flight lets it notice. An abandoned one is not
	 * this track's. */
	if (!dl_abandoned) {
		if (dl.thread && !dl.done) {
			dl.stop = true;
			dl_abandoned = true;
		} else {
			download_reap();
		}
	}
	if (s->decoder)
		stb_vorbis_close(s->decoder);
	if (s->ndsp_ready) {
		/* The DSP stays up for the next track; only the channel goes. */
		ndspChnWaveBufClear(0);
		ndspChnReset(0);
	}
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

/* Starts the track's preparation thread. Not while a skipped track's is
 * still running, nor a skipped track's download, nor while the thumbnails
 * finish their last request (they must not compete with the preparation,
 * STYLES.md 9): it waits, a frame at a time, without blocking. */
static void player_start_prepare(void)
{
	player.stage = STR_PLAYER_AUDIO_PREPARING;
	if (prep.thread || dl.thread || thumbs_stopping())
		return;
	memset(&prep, 0, sizeof(prep));
	prep.session = player.session;
	prep.track = playback_queue[player.index];
	prep.stage = STR_PLAYER_AUDIO_PREPARING;
	prep.thread = threadCreate(prepare_worker, &prep, PREPARE_STACK,
	                           background_priority(), -2, false);
	if (!prep.thread) {
		player_fail(tr(STR_ERR_DOWNLOAD_THREAD));
		return;
	}
	player.s.preparing = true;
}

/* Once the preparation has finished, takes its stream and sets the audio up.
 * Nothing here waits on the network. */
static void player_begin_audio(void)
{
	player_stream *s = &player.s;
	char error[256];
	/* `done` was seen: the worker's writes before it are visible. */
	__sync_synchronize();
	int rc = prep.rc;
	snprintf(error, sizeof(error), "%s", prep.error);
	/* The stream is the player's from here: player_release frees it. */
	dl.stream = prep.stream;
	memset(&prep.stream, 0, sizeof(prep.stream));
	prepare_reap();
	if (rc != 0) {
		if (!error[0])
			snprintf(error, sizeof(error), "%s",
			         tr(rc == AP_KEY_REFUSED ? STR_ERR_KEY_REFUSED
			                                 : STR_ERR_AP_SESSION));
		player_fail(error);
		return;
	}

	if (!audio_open()) {
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

	s->source_pos = SPOTIFY_OGG_HEADER;

	dl.thread = threadCreate(download_worker, &dl, 32 * 1024,
	                         background_priority(), -2, false);
	if (!dl.thread) {
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
	size_t available = dl.stream.downloaded;
	if (dl.stream.data && available > s->source_pos &&
	    s->input_len < PLAYER_INPUT_CAPACITY) {
		size_t copy = available - s->source_pos;
		if (copy > PLAYER_INPUT_CAPACITY - s->input_len)
			copy = PLAYER_INPUT_CAPACITY - s->input_len;
		memcpy(s->input + s->input_len, dl.stream.data + s->source_pos,
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
	available = dl.stream.downloaded;
	bool no_more_data = dl.done && s->source_pos >= available &&
	                    (!s->decoder_open || decoder_needs_more);
	if (no_more_data && !s->eof) {
		if (dl.stream.dl_failed) {
			snprintf(error, sizeof(error), tr(STR_ERR_DOWNLOAD_FAILED_FMT),
			         dl.stream.dl_err);
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
	/* A skipped track's preparation or download that has finished: nobody
	 * wants it. */
	if (prep.thread && prep.abandoned && prep.done)
		prepare_reap();
	if (dl_abandoned && dl.done)
		download_reap();
	if (player.phase == PLAYER_PHASE_LOADING) {
		if (artwork_pending(&cover_art))
			return;
		if (!player.s.preparing) {
			player_start_prepare();
		} else if (!prep.abandoned) {
			player.stage = prep.stage;
			if (prep.done)
				player_begin_audio();
		}
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
	prepare_wait();
	download_wait();
	player.phase = PLAYER_PHASE_IDLE;
	player.error[0] = '\0';
}

/* The track's network steps are running (token, access point, audio key):
 * the thumbnails wait (STYLES.md 9). During the audio download itself they
 * go on: a 64 px cover is a few KB next to megabytes of audio, and the
 * list would otherwise stay empty for the whole download. */
static bool player_preparing(void)
{
	return player.phase == PLAYER_PHASE_LOADING;
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
	/* An abandoned download is the skipped track's, not this one's. */
	const download_job *job = dl_abandoned ? NULL : &dl;
	unsigned percent = job && job->stream.total
	                   ? (unsigned)(job->stream.downloaded * 100 /
	                                job->stream.total) : 0;
	if ((job && job->done && job->stream.dl_done) ||
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
 * cover is on screen, and switching tabs always repaints. Before the
 * session, view_login.h draws both screens. */
/* Called every frame: keeps the console awake while music is due, and
 * darkens the screens while the lid is shut over it. */
static void power_poll(void)
{
	if (!ptmu_ready || !lcd_ready)
		return;   /* without both, closing the lid just sleeps as before */
	bool awake = power_keep_awake(player.phase, player.paused || player.held,
	                              osIsHeadsetConnected());
	u8 shell = 1;
	if (R_FAILED(PTMU_GetShellState(&shell)))
		shell = 1;
	bool lit = power_screens_lit(shell != 0, awake);
	/* Before sleep is allowed again: that sleeps at once if the lid is
	 * shut, and NDM should take the connection back first. */
	wifi_hold(awake);
	if (lit && screens_dark) {
		GSPLCD_PowerOnAllBacklights();
		screens_dark = false;
		screens_lost = true;
	} else if (!lit && !screens_dark) {
		GSPLCD_PowerOffAllBacklights();
		screens_dark = true;
	}
	/* Allowing sleep again with the lid shut sleeps at once, which is why
	 * the screens were lit first. */
	if (aptIsSleepAllowed() == awake)
		aptSetSleepAllowed(!awake);
}

static void frame(void)
{
	/* 0: the login view; 1 + tab: that tab. */
	static int drawn_screen = -1;
	artwork_poll(&cover_art);
	power_poll();
	if (screens_dark) {
		/* Nobody can see them: drawing would only cost battery. The
		 * repaint comes when the lid opens. */
		gspWaitForVBlank();
		return;
	}
	bool restored = ui_take_restored() || screens_lost;
	screens_lost = false;
	bool tabs = active_view == VIEW_TABS;
	int screen = tabs ? 1 + (int)active_tab : 0;
	bool changed;
	if (!tabs) {
		changed = view_login_dirty();
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
		view_login_draw_top();
	else
		top_draw(active_tab, &cover_art);
	ui_scene(GFX_BOTTOM);
	if (!tabs) {
		view_login_draw_bottom();
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
	/* The thumbnails must not compete with the track's preparation
	 * (STYLES.md 9); they resume, kept, once its download starts. */
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
		/* Not stopped on leaving the tab: it only fetches the rows on
		 * screen and the next ones anyway. */
		view_results_allow_thumbs(!player_preparing());

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
	/* Every worker's network wait fails within TLS_ABORT_SLICE_MS, so the
	 * joins below take a moment, not a whole connection's timeout. */
	tls_set_abort(true);
	player_stop();
	search_stop();
	/* Waits: after this the session, TLS and sockets may go away, and a
	 * thumbnail request still in flight would use them freed. */
	thumbs_wait();
	tls_set_abort(false);
	active_view = VIEW_LOGIN;
}

/* --- Session start-up ---------------------------------------------------- */

/* The start-up runs in a worker so the main thread keeps drawing and reading
 * the buttons while it blocks in TLS handshakes for seconds at a time. The
 * worker never touches the screens: it publishes how far it got, and the
 * main thread draws that. One at a time; global, never on a stack, because
 * the worker holds its address. */
typedef struct {
	bool force_pair;                /* in: skip the stored credential */
	spotify_session *session;       /* out, filled on success */
	volatile str_id stage;          /* what it is doing now */
	volatile bool pairing;          /* a code is on screen, being polled */
	volatile bool paired;           /* pairing ran: one more step */
	volatile bool reason_expired;   /* pairing because the credential died */
	volatile bool cancel;           /* main thread: give up */
	volatile bool done;
	bool ok;
	startup_error error;
	/* What the pairing screen shows, copied under `lock`. Never the
	 * device code, which is secret. */
	LightLock lock;
	char user_code[16];
	char verification_uri[128];
	char qr_url[192];
	bool offline;                   /* the last poll did not reach Spotify */
	Thread thread;
} session_job;

/* A consistent copy of the pairing data, for drawing. */
typedef struct {
	bool reason_expired;
	bool offline;
	char user_code[16];
	char verification_uri[128];
	char qr_url[192];
} pair_view;

static session_job login;
/* The worker's large locals, off its stack and cleared after each use. */
static spotify_tokens login_tokens;
static pair_request login_request;
static ap_info login_ap;
static login5_token login_library;
static ap_candidate login_candidates[AP_MAX_CANDIDATES];
static char login_stored[512];
/* As PREPARE_STACK, which already logs in to an access point over TLS. */
#define SESSION_STACK (64 * 1024)
#define SESSION_SLICE_MS 100

static void session_stage(str_id stage)
{
	__sync_synchronize();
	login.stage = stage;
}

static void pair_publish(const pair_request *req, bool offline)
{
	LightLock_Lock(&login.lock);
	snprintf(login.user_code, sizeof(login.user_code), "%s", req->user_code);
	snprintf(login.verification_uri, sizeof(login.verification_uri), "%s",
	         req->verification_uri);
	login_qr_url(req->verification_uri, req->verification_uri_complete,
	             req->user_code, login.qr_url, sizeof(login.qr_url));
	login.offline = offline;
	LightLock_Unlock(&login.lock);
}

static void pair_snapshot(pair_view *out)
{
	LightLock_Lock(&login.lock);
	out->reason_expired = login.reason_expired;
	out->offline = login.offline;
	memcpy(out->user_code, login.user_code, sizeof(out->user_code));
	memcpy(out->verification_uri, login.verification_uri,
	       sizeof(out->verification_uri));
	memcpy(out->qr_url, login.qr_url, sizeof(out->qr_url));
	LightLock_Unlock(&login.lock);
}

/* Waits `seconds` in slices, so a cancel is seen within one. True when it
 * was cancelled. */
static bool session_wait(int seconds)
{
	for (int ms = 0; ms < seconds * 1000 && !login.cancel;
	     ms += SESSION_SLICE_MS)
		svcSleepThread((s64)SESSION_SLICE_MS * 1000000LL);
	return login.cancel;
}

static void pair_cancelled(startup_error *error)
{
	error->category = STR_CAT_PAIR_CANCELLED;
	snprintf(error->detail, sizeof(error->detail), "%s",
	         tr(STR_PAIR_CREDENTIALS_UNCHANGED));
}

/* Worker thread. */
static bool pair_account(spotify_tokens *tokens, startup_error *error)
{
	pair_request *req = &login_request;
	char detail[192] = {0};
	session_stage(STR_PAIR_REQUESTING);
	if (pair_begin(req, detail, sizeof(detail)) != 0) {
		error->category = STR_CAT_PAIRING;
		snprintf(error->detail, sizeof(error->detail), "%s", detail);
		secure_clear(req, sizeof(*req));
		return false;
	}

	int interval = req->interval_s > 0 ? req->interval_s : 5;
	time_t deadline = time(NULL) + req->expires_in_s;
	pair_publish(req, false);
	__sync_synchronize();
	login.pairing = true;
	bool ok = false;
	for (;;) {
		if (session_wait(interval)) {
			pair_cancelled(error);
			break;
		}
		if (time(NULL) >= deadline) {
			error->category = STR_CAT_PAIR_EXPIRED;
			snprintf(error->detail, sizeof(error->detail), "%s",
			         tr(STR_PAIR_RETRY_NEW_CODE));
			break;
		}

		detail[0] = '\0';
		pair_status result = pair_poll(req, tokens, detail, sizeof(detail));
		/* A cancel wins even over an approval that raced it: nothing is
		 * saved, and the stored credential stays as it was. */
		if (login.cancel) {
			secure_clear(tokens, sizeof(*tokens));
			pair_cancelled(error);
			break;
		}
		/* Still pending: with a detail, the poll never reached Spotify
		 * (pair_poll keeps the code good and says why). */
		if (result == PAIR_PENDING) {
			pair_publish(req, detail[0] != '\0');
			continue;
		}
		if (result == PAIR_SLOW_DOWN) {
			interval += 5;
			pair_publish(req, false);
			continue;
		}
		login.pairing = false;
		if (result == PAIR_OK) {
			login.paired = true;
			if (!tokens->refresh_token[0] ||
			    !cfg_save_refresh_token(tokens->refresh_token, tokens->scope)) {
				error->category = STR_CAT_SAVE_CREDENTIALS;
				snprintf(error->detail, sizeof(error->detail), "%s",
				         tr(STR_CHECK_SD_SPACE));
				secure_clear(tokens, sizeof(*tokens));
				break;
			}
			ok = true;
			break;
		}

		error->category = result == PAIR_DENIED
		                  ? STR_CAT_PAIR_DENIED
		                  : result == PAIR_EXPIRED
		                    ? STR_CAT_PAIR_EXPIRED
		                    : STR_CAT_PAIR_FAILED;
		snprintf(error->detail, sizeof(error->detail), "%s",
		         detail[0] ? detail : tr(STR_NO_VALID_RESPONSE));
		secure_clear(tokens, sizeof(*tokens));
		break;
	}
	login.pairing = false;
	secure_clear(req, sizeof(*req));
	return ok;
}

/* Worker thread. */
static bool resolve_and_login(spotify_tokens *tokens, startup_error *error,
                              spotify_session *session)
{
	char detail[192] = {0};
	char device_id[64];
	ap_device_id(device_id, sizeof(device_id));

	ap_candidate *candidates = login_candidates;
	session_stage(STR_AP_SEARCHING);
	int count = ap_resolve_list(candidates, AP_MAX_CANDIDATES,
	                            detail, sizeof(detail));
	if (count < 1) {
		error->category = STR_CAT_AP_RESOLVE;
		snprintf(error->detail, sizeof(error->detail),
		         tr(STR_CHECK_NETWORK_FMT), detail);
		secure_clear(tokens, sizeof(*tokens));
		return false;
	}

	ap_info *info = &login_ap;
	memset(info, 0, sizeof(*info));
	int tried = 0;
	session_stage(STR_AP_AUTHENTICATING);
	ap_session *ap = ap_login_any(candidates, count, tokens->access_token,
	                              device_id, info, &tried,
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
		error->category = info->login_error
		                  ? STR_CAT_AP_REJECTED
		                  : STR_CAT_AP_CONNECT;
		if (info->login_error)
			snprintf(error->detail, sizeof(error->detail),
			         tr(STR_ERROR_CODE_FMT),
			         ap_login_error_text(info->login_error),
			         info->login_error);
		else
			snprintf(error->detail, sizeof(error->detail), "%s",
			         detail[0] ? detail : tr(STR_NO_AP_RESPONDED));
		secure_clear(info, sizeof(*info));
		return false;
	}

	login5_token *library = &login_library;
	session_stage(STR_LIBRARY_OPENING);
	int rc = login5_from_credentials(info->username, info->reusable,
	                                 info->reusable_len, device_id,
	                                 library, detail, sizeof(detail));
	ap_close(ap);
	secure_clear(info, sizeof(*info));
	secure_clear(device_id, sizeof(device_id));
	if (rc != 0) {
		error->category = strstr(detail, "login5 refused:")
		                  ? STR_CAT_LOGIN_REFUSED
		                  : STR_CAT_LOGIN_FAILED;
		snprintf(error->detail, sizeof(error->detail), "%s", detail);
		secure_clear(library, sizeof(*library));
		return false;
	}

	detail[0] = '\0';
	session_stage(STR_SERVER_RESOLVING);
	rc = sp_resolve(session->host, sizeof(session->host),
	                 detail, sizeof(detail));
	if (rc != 0 || !session->host[0]) {
		error->category = STR_CAT_SERVER_RESOLVE;
		snprintf(error->detail, sizeof(error->detail), "%s",
		         detail[0] ? detail : tr(STR_INVALID_SERVER_RESPONSE));
		secure_clear(library, sizeof(*library));
		return false;
	}
	/* Both are 1024 bytes; the bound only tells GCC so, now that the token
	 * is a global it cannot see the end of. */
	snprintf(session->library_token, sizeof(session->library_token), "%.*s",
	         (int)sizeof(session->library_token) - 1, library->access_token);
	secure_clear(library, sizeof(*library));
	return true;
}

/* Worker thread. */
static bool start_session(bool force_pair, startup_error *error,
                          spotify_session *session)
{
	char *stored = login_stored;
	size_t stored_size = sizeof(login_stored);
	spotify_tokens *tokens = &login_tokens;
	memset(stored, 0, stored_size);
	memset(tokens, 0, sizeof(*tokens));

	if (force_pair || !cfg_load_refresh_token(stored, stored_size)) {
		secure_clear(stored, stored_size);
		if (!pair_account(tokens, error))
			return false;
		return resolve_and_login(tokens, error, session);
	}

	char detail[192] = {0};
	session_stage(STR_CREDENTIAL_RENEWING);
	if (token_refresh(stored, tokens, detail, sizeof(detail)) != 0) {
		secure_clear(stored, stored_size);
		secure_clear(tokens, sizeof(*tokens));
		if (strstr(detail, "invalid_grant")) {
			/* Straight to pairing; the pairing screen says why. */
			login.reason_expired = true;
			if (!pair_account(tokens, error))
				return false;
			return resolve_and_login(tokens, error, session);
		}
		error->category = STR_CAT_OAUTH_RENEW;
		snprintf(error->detail, sizeof(error->detail), "%s",
		         detail[0] ? detail : tr(STR_NO_VALID_RESPONSE));
		return false;
	}

	if (tokens->refresh_token[0] &&
	    strcmp(tokens->refresh_token, stored) != 0 &&
	    !cfg_save_refresh_token(tokens->refresh_token, tokens->scope)) {
		error->category = STR_CAT_SAVE_RENEWED;
		snprintf(error->detail, sizeof(error->detail), "%s",
		         tr(STR_CHECK_SD_SPACE));
		secure_clear(stored, stored_size);
		secure_clear(tokens, sizeof(*tokens));
		return false;
	}
	if (!tokens->refresh_token[0])
		snprintf(tokens->refresh_token, sizeof(tokens->refresh_token), "%s",
		         stored);
	secure_clear(stored, stored_size);
	return resolve_and_login(tokens, error, session);
}

static void session_worker(void *arg)
{
	(void)arg;
	login.ok = start_session(login.force_pair, &login.error, login.session);
	__sync_synchronize();
	login.done = true;
}

/* Cuts the worker's network waits short and joins it: the same pattern as
 * stop_tabs(). DNS is not cut (tls3ds.h), so this may take a moment. */
static void session_cancel(void)
{
	login.cancel = true;
	if (!login.thread)
		return;
	tls_set_abort(true);
	threadJoin(login.thread, U64_MAX);
	threadFree(login.thread);
	login.thread = NULL;
	tls_set_abort(false);
}

typedef enum {
	LOGIN_READY,    /* *session is up */
	LOGIN_FAILED,   /* *error says why */
	LOGIN_QUIT      /* the app is closing */
} login_result;

/* Hands the worker's progress to the login view. Cheap enough for every
 * frame: the view changes nothing when the content is the same. */
static void show_login(void)
{
	if (!login.pairing) {
		view_login_connecting(login.stage, login.paired);
		return;
	}
	pair_view pair;
	pair_snapshot(&pair);
	login_pair_info info = {
		.reason_expired = pair.reason_expired,
		.offline = pair.offline,
		.user_code = pair.user_code,
		.verification_uri = pair.verification_uri,
		.qr_url = pair.qr_url,
	};
	view_login_pairing(&info);
	secure_clear(&pair, sizeof(pair));
}

/* Runs the session start-up in the worker and keeps drawing meanwhile. B
 * cancels a pairing without touching the stored credential; SELECT
 * switches the language. */
static login_result run_login(bool force_pair, spotify_session *session,
                              startup_error *error)
{
	secure_clear(&login, sizeof(login));
	LightLock_Init(&login.lock);
	login.force_pair = force_pair;
	login.session = session;
	login.stage = STR_LOGIN_CONNECTING;
	active_view = VIEW_LOGIN;
	show_login();
	frame();

	__sync_synchronize();
	login.thread = threadCreate(session_worker, NULL, SESSION_STACK,
	                            background_priority(), -2, false);
	/* Without a thread, the start-up still works, only without a live
	 * screen: as it always did. */
	if (!login.thread)
		session_worker(NULL);

	while (!login.done) {
		if (!aptMainLoop()) {
			session_cancel();
			secure_clear(&login, sizeof(login));
			return LOGIN_QUIT;
		}
		hidScanInput();
		show_login();
		login_action action = view_login_update(hidKeysDown());
		if (action == LOGIN_ACTION_LANGUAGE)
			switch_language();
		else if (action == LOGIN_ACTION_CANCEL && login.pairing)
			session_cancel();
		frame();
	}

	__sync_synchronize();
	if (login.thread) {
		threadJoin(login.thread, U64_MAX);
		threadFree(login.thread);
		login.thread = NULL;
	}
	login_result result = login.ok ? LOGIN_READY : LOGIN_FAILED;
	if (!login.ok)
		*error = login.error;
	secure_clear(&login, sizeof(login));
	return result;
}

/* The start-up error until the user picks: Retry (A) with the stored
 * credential, Pair (Y) again, or Exit (START). A fatal error (no network
 * to retry with) only exits. Closing the app counts as Exit. */
static login_action run_error(const startup_error *error, bool fatal)
{
	active_view = VIEW_LOGIN;
	view_login_error(error->category, error->detail, fatal);
	while (aptMainLoop()) {
		hidScanInput();
		login_action action = view_login_update(hidKeysDown());
		if (action == LOGIN_ACTION_LANGUAGE)
			switch_language();
		else if (action == LOGIN_ACTION_RETRY ||
		         action == LOGIN_ACTION_PAIR ||
		         action == LOGIN_ACTION_EXIT)
			return action;
		frame();
	}
	return LOGIN_ACTION_EXIT;
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
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
	ptmu_ready = R_SUCCEEDED(ptmuInit());
	lcd_ready = R_SUCCEEDED(gspLcdInit());
	ndm_ready = R_SUCCEEDED(ndmuInit());
	/* Now, with the lid surely open; a failure is retried, and reported,
	 * on the first play. */
	audio_open();
	/* Without icons, thumbnails or the logo the views still work, just
	 * without them. */
	icons_init();
	thumbs_init();
	if (romfs_ready)
		logo_load();
	if (!view_home_init() || !view_results_init() ||
	    !view_player_init() || !view_settings_init() || !view_login_init() ||
	    !top_init()) {
		top_exit();
		view_login_exit();
		view_settings_exit();
		view_player_exit();
		view_results_exit();
		view_home_exit();
		logo_free();
		thumbs_exit();
		icons_exit();
		ui_exit();
		if (romfs_ready)
			romfsExit();
		gfxExit();
		return 1;
	}
	cfg_set_path(NULL);
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
		login_result login_state = run_login(force_pair, &session, &error);
		if (login_state == LOGIN_QUIT)
			goto done;
		if (login_state == LOGIN_READY) {
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
			login_action choice = run_error(&error, false);
			if (choice == LOGIN_ACTION_EXIT)
				goto done;
			force_pair = choice == LOGIN_ACTION_PAIR;
		}
	}

failed:
	run_error(&error, true);

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
	view_login_exit();
	view_settings_exit();
	view_player_exit();
	view_results_exit();
	view_home_exit();
	logo_free();
	thumbs_exit();
	icons_exit();
	if (screens_dark)
		GSPLCD_PowerOnAllBacklights();
	if (ndsp_up)
		ndspExit();
	wifi_hold(false);
	if (ndm_ready)
		ndmuExit();
	aptSetSleepAllowed(true);
	if (lcd_ready)
		gspLcdExit();
	if (ptmu_ready)
		ptmuExit();
	ui_exit();
	if (romfs_ready)
		romfsExit();
	gfxExit();
	return 0;
}
