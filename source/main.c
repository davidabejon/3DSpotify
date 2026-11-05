#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds/thread.h>

#include "ap3ds.h"
#include "artwork.h"
#include "cfg3ds.h"
#include "lang.h"
#include "login5.h"
#include "pair3ds.h"
#include "player_logic.h"
#include "spclient.h"
#include "textscreen.h"
#include "track.h"
#include "tls3ds.h"
#include "ui.h"
#include "worker_priority.h"
#include "stb_vorbis.h"

#define SOC_BUFFER 0x100000
#define SEARCH_LIMIT 24
#define SEARCH_QUERY_SIZE 96
#define RESULTS_PER_PAGE 7
/* 8 x 2048 frames is ~370 ms of audio at 44.1 kHz: enough cushion for the
 * decoder to ride out a burst of TLS work in the download thread. */
#define PLAYER_WAVE_BUFFERS 8
#define PLAYER_FRAMES_PER_BUFFER 2048
#define PLAYER_INPUT_CAPACITY (128 * 1024)
#define PLAYER_STATUS_ROW 8
#define PLAYER_STATUS_WIDTH 39

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
	volatile bool exit_requested;
	volatile bool done;
} search_job;

typedef struct {
	/* An id rather than text so the screen follows a language switch. */
	str_id category;
	char detail[256];
} startup_error;

typedef enum {
	SEARCH_REQUEST_OK,
	SEARCH_REQUEST_ERROR,
	SEARCH_REQUEST_CANCELLED,
	SEARCH_REQUEST_EXIT
} search_result;

static search_job search;
static sp_track playback_queue[SEARCH_LIMIT];
static int playback_queue_count;
static float playback_volume = 0.7f;
static artwork_state cover_art;

static void frame(void);

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

/* SELECT switches the language on any menu screen and remembers the choice.
 * Returns true when the caller has to redraw its screen. */
static bool language_toggled(u32 down)
{
	if (!(down & KEY_SELECT))
		return false;
	lang_set(lang_next(lang_get()));
	cfg_set("language", lang_code(lang_get()));
	return true;
}

static void show_player_stage(const char *message)
{
	textscreen_clear();
	textscreen_printf("%s\n\n%s\n", tr(STR_PLAYER_TITLE), message);
	textscreen_printf("\n%s\n", tr(STR_KEYS_CANCEL_EXIT));
	frame();
}

static void show_player_screen(const sp_track *track, int track_index,
                               int queue_count)
{
	textscreen_clear();
	textscreen_printf("%s\n\n", tr(STR_PLAYER_TITLE));
	textscreen_printf("%.42s\n", track->name);
	textscreen_printf("%s: %.42s\n", tr(STR_LABEL_ARTIST), track->artist);
	textscreen_printf(tr(STR_PLAYER_QUEUE_FMT), track_index + 1, queue_count);
	textscreen_printf("\n\n%s\n", tr(STR_KEYS_PLAYER));
	textscreen_printf("%s\n", tr(STR_KEYS_RESULTS_EXIT));
}

static void show_playback_failed(const char *error)
{
	textscreen_clear();
	textscreen_printf("%s\n\n", tr(STR_PLAYER_FAILED_TITLE));
	textscreen_printf("%s\n\n", error[0] ? error : tr(STR_PLAYER_INTERRUPTED));
	textscreen_printf("%s\n", tr(STR_KEYS_RESULTS_EXIT));
	textscreen_printf("%s\n", tr(STR_KEYS_LANGUAGE));
}

static void show_playback_finished(const sp_track *track)
{
	textscreen_clear();
	textscreen_printf("%s\n\n", tr(STR_PLAYER_FINISHED_TITLE));
	textscreen_printf("%.42s\n", track->name);
	textscreen_printf("%s: %.42s\n\n", tr(STR_LABEL_ARTIST), track->artist);
	textscreen_printf("%s\n", tr(STR_KEYS_RESULTS_EXIT));
	textscreen_printf("%s\n", tr(STR_KEYS_LANGUAGE));
}

static void player_update_line(const char *message)
{
	/* Move the text screen's cursor with ANSI (1-based) and stay one column
	 * short to avoid wrapping. Rewriting the same text is free: only a real
	 * change makes textscreen ask for a GPU frame. */
	textscreen_printf("\x1b[%d;1H%-*.*s", PLAYER_STATUS_ROW + 1,
	                  PLAYER_STATUS_WIDTH, PLAYER_STATUS_WIDTH, message);
}

static void download_worker(void *arg)
{
	download_job *job = arg;
	job->result = track_stream_download(&job->stream, &job->stop);
	__sync_synchronize();
	job->done = true;
}

static void download_stop_join(download_job *job)
{
	if (!job->thread)
		return;
	job->stop = true;
	while (!job->done && aptMainLoop()) {
		player_update_line(tr(STR_PLAYER_DOWNLOAD_CANCELLING));
		frame();
	}
	threadJoin(job->thread, U64_MAX);
	threadFree(job->thread);
	job->thread = NULL;
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

static void set_playback_volume(float volume)
{
	if (volume < 0.0f)
		volume = 0.0f;
	if (volume > 1.0f)
		volume = 1.0f;
	playback_volume = volume;
	float mix[12] = {0};
	mix[0] = mix[1] = playback_volume;
	ndspChnSetMix(0, mix);
}

static playback_result playback(spotify_session *session,
                                const sp_track *queue, int queue_count,
                                int *track_index)
{
	const sp_track *selected = &queue[*track_index];
	char error[256] = {0};
	download_job job;
	memset(&job, 0, sizeof(job));
	ndspWaveBuf waves[PLAYER_WAVE_BUFFERS];
	memset(waves, 0, sizeof(waves));
	bool wave_submitted[PLAYER_WAVE_BUFFERS] = {0};
	uint8_t *input = NULL;
	int16_t *audio = NULL;
	bool ndsp_ready = false;
	bool decoder_open = false;
	stb_vorbis *decoder = NULL;
	bool success = false;
	bool cancelled = false;
	bool exit_requested = false;
	bool return_to_results = false;
	bool track_changed = false;
	bool paused = false;
	bool eof = false;
	bool final_wave_submitted = false;
	size_t input_len = 0;
	size_t source_pos = SPOTIFY_OGG_HEADER;
	size_t pcm_frames = 0;
	size_t total_audio_frames = 0;
	size_t completed_audio_frames = 0;
	size_t displayed_played_frames = 0;
	int16_t pcm[PLAYER_FRAMES_PER_BUFFER * 2];
	float **decoded = NULL;
	int decoded_samples = 0;
	int decoded_offset = 0;
	stb_vorbis_info info = {0};

	artwork_request(&cover_art, selected->art_large);
	show_player_stage(tr(STR_PLAYER_ART_LOADING));
	while (artwork_pending(&cover_art) && aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown();
		if (down & KEY_START) {
			cancelled = true;
			exit_requested = true;
		} else if (down & KEY_B) {
			cancelled = true;
			return_to_results = true;
		}
		player_update_line(cancelled
		                   ? tr(STR_PLAYER_ART_WAITING)
		                   : tr(STR_PLAYER_ART_DOWNLOADING));
		frame();
	}
	if (!aptMainLoop() && !cancelled) {
		cancelled = true;
		exit_requested = true;
	}
	if (cancelled)
		goto cleanup;

	show_player_stage(tr(STR_PLAYER_AUDIO_PREPARING));
	if (R_FAILED(ndspInit())) {
		snprintf(error, sizeof(error), "%s", tr(STR_ERR_NDSP));
		goto cleanup;
	}
	ndsp_ready = true;
	ndspChnSetPaused(0, false);

	input = malloc(PLAYER_INPUT_CAPACITY);
	audio = linearAlloc(PLAYER_FRAMES_PER_BUFFER * 4 * PLAYER_WAVE_BUFFERS);
	if (!input || !audio) {
		snprintf(error, sizeof(error), "%s", tr(STR_ERR_AUDIO_MEMORY));
		goto cleanup;
	}
	for (int i = 0; i < PLAYER_WAVE_BUFFERS; i++) {
		waves[i].data_vaddr = (uint8_t *)audio +
		                      i * PLAYER_FRAMES_PER_BUFFER * 4;
		waves[i].nsamples = PLAYER_FRAMES_PER_BUFFER;
		waves[i].status = NDSP_WBUF_DONE;
	}

	show_player_stage(tr(STR_AP_AUTHENTICATING));
	ap_session *ap = open_playback_ap(session, error, sizeof(error));
	if (!ap)
		goto cleanup;

	show_player_stage(tr(STR_PLAYER_AUDIO_KEY));
	int rc = track_stream_begin_cached(session->host, session->library_token,
	                                   ap, selected, NULL, &job.stream,
	                                   error, sizeof(error));
	ap_close(ap);
	if (rc != 0) {
		if (rc == AP_KEY_REFUSED)
			snprintf(error, sizeof(error), "%s", tr(STR_ERR_KEY_REFUSED));
		goto cleanup;
	}
	show_player_screen(selected, *track_index, queue_count);
	char initial_status[64];
	snprintf(initial_status, sizeof(initial_status),
	         tr(STR_PLAYER_STATUS_FMT), 0u, 0u,
	         selected->duration_ms / 60000,
	         (selected->duration_ms / 1000) % 60, 0u,
	         (unsigned)(playback_volume * 100.0f + 0.5f));
	player_update_line(initial_status);
	/* What the status row shows now; the loop below only reprints it when
	 * the text changes, which keeps formatting off the decoder's thread. */
	char shown_status[64];
	snprintf(shown_status, sizeof(shown_status), "%s", initial_status);
	frame();

	job.thread = threadCreate(download_worker, &job, 32 * 1024,
	                          background_priority(), -2, false);
	if (!job.thread) {
		snprintf(error, sizeof(error), "%s", tr(STR_ERR_DOWNLOAD_THREAD));
		goto cleanup;
	}

	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
	ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
	set_playback_volume(playback_volume);

	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown();
		if (down & KEY_START) {
			cancelled = true;
			exit_requested = true;
			break;
		}
		if (down & KEY_B) {
			cancelled = true;
			return_to_results = true;
			break;
		}
		if (down & KEY_A) {
			paused = !paused;
			ndspChnSetPaused(0, paused);
		}
		/* The status line below is rewritten on this same iteration. */
		if (language_toggled(down)) {
			show_player_screen(selected, *track_index, queue_count);
			shown_status[0] = '\0';
		}
		if ((down & KEY_LEFT) || (down & KEY_RIGHT)) {
			int direction = (down & KEY_LEFT) ? -1 : 1;
			int next = playback_queue_step(*track_index, queue_count,
			                               direction);
			if (next >= 0) {
				*track_index = next;
				track_changed = true;
				cancelled = true;
				break;
			}
		}
		if ((down & KEY_UP) || (down & KEY_DOWN)) {
			float change = (down & KEY_UP) ? 0.1f : -0.1f;
			set_playback_volume(playback_volume + change);
		}

		for (int i = 0; i < PLAYER_WAVE_BUFFERS; i++) {
			if (wave_submitted[i] && waves[i].status == NDSP_WBUF_DONE) {
				completed_audio_frames += waves[i].nsamples;
				wave_submitted[i] = false;
			}
		}

		__sync_synchronize();
		size_t available = job.stream.downloaded;
		size_t played_frames = completed_audio_frames;
		if (!paused) {
			u16 current_sequence = ndspChnGetWaveBufSeq(0);
			for (int i = 0; i < PLAYER_WAVE_BUFFERS; i++)
				if (wave_submitted[i] &&
				    waves[i].sequence_id == current_sequence) {
					u32 position = ndspChnGetSamplePos(0);
					if (position < waves[i].nsamples)
						played_frames += position;
					break;
				}
			displayed_played_frames = played_frames;
		} else {
			played_frames = displayed_played_frames;
		}
		unsigned percent = job.stream.total
		                   ? (unsigned)(available * 100 /
		                                job.stream.total) : 0;
		if (job.done && job.stream.dl_done)
			percent = 100;
		unsigned sample_rate = decoder_open ? info.sample_rate : 0;
		unsigned elapsed = sample_rate
		                   ? (unsigned)(played_frames / sample_rate) : 0;
		unsigned volume = (unsigned)(playback_volume * 100.0f + 0.5f);
		char status[64];
		snprintf(status, sizeof(status), tr(STR_PLAYER_STATUS_FMT),
		         elapsed / 60, elapsed % 60,
		         selected->duration_ms / 60000,
		         (selected->duration_ms / 1000) % 60,
		         percent, volume);
		if (strcmp(status, shown_status) != 0) {
			player_update_line(status);
			snprintf(shown_status, sizeof(shown_status), "%s", status);
		}

		if (job.stream.data && available > source_pos &&
		    input_len < PLAYER_INPUT_CAPACITY) {
			size_t copy = available - source_pos;
			if (copy > PLAYER_INPUT_CAPACITY - input_len)
				copy = PLAYER_INPUT_CAPACITY - input_len;
			memcpy(input + input_len, job.stream.data + source_pos, copy);
			input_len += copy;
			source_pos += copy;
		}

		if (!decoder && input_len > 0) {
			int consumed = 0;
			int vorbis_error = 0;
			decoder = stb_vorbis_open_pushdata(input, (int)input_len,
			                                   &consumed, &vorbis_error, NULL);
			if (decoder) {
				if (consumed < 0 || (size_t)consumed > input_len) {
					snprintf(error, sizeof(error), "%s",
					         tr(STR_ERR_DECODER_OFFSET));
					break;
				}
				memmove(input, input + consumed, input_len - (size_t)consumed);
				input_len -= (size_t)consumed;
				info = stb_vorbis_get_info(decoder);
				if ((info.channels != 1 && info.channels != 2) ||
				    info.sample_rate == 0) {
					snprintf(error, sizeof(error),
					         tr(STR_ERR_VORBIS_CHANNELS_FMT),
					         info.channels);
					break;
				}
				ndspChnSetRate(0, (float)info.sample_rate);
				decoder_open = true;
			} else if (vorbis_error != VORBIS_need_more_data) {
				snprintf(error, sizeof(error),
				         tr(STR_ERR_VORBIS_OPEN_FMT),
				         vorbis_error);
				break;
			}
		}

		int wave_index = free_wave_index(waves);
		if (pcm_frames == PLAYER_FRAMES_PER_BUFFER) {
			if (wave_index >= 0) {
				waves[wave_index].data_pcm16 =
					(int16_t *)((uint8_t *)audio +
					wave_index * PLAYER_FRAMES_PER_BUFFER * 4);
				memcpy(waves[wave_index].data_pcm16,
				       pcm, pcm_frames * 4);
				submit_wave(&waves[wave_index], pcm_frames);
				wave_submitted[wave_index] = true;
				total_audio_frames += pcm_frames;
				pcm_frames = 0;
			} else {
				frame();
			}
			continue;
		}

		if (decoded && decoded_offset < decoded_samples) {
			size_t amount = (size_t)(decoded_samples - decoded_offset);
			size_t room = PLAYER_FRAMES_PER_BUFFER - pcm_frames;
			if (amount > room)
				amount = room;
			for (size_t i = 0; i < amount; i++) {
				for (int channel = 0; channel < 2; channel++) {
					float sample = decoded[info.channels == 1 ? 0 : channel]
					                        [decoded_offset + (int)i];
					if (sample > 1.0f) sample = 1.0f;
					if (sample < -1.0f) sample = -1.0f;
					int value = (int)(sample * 32767.0f);
					pcm[(pcm_frames + i) * 2 + channel] = (int16_t)value;
				}
			}
			decoded_offset += (int)amount;
			pcm_frames += amount;
			if (decoded_offset == decoded_samples) {
				decoded = NULL;
				decoded_samples = decoded_offset = 0;
			}
			continue;
		}

		bool decoder_needs_more = decoder_open && input_len == 0;
		if (decoder && input_len > 0) {
			int channels = 0;
			int samples = 0;
			int used = stb_vorbis_decode_frame_pushdata(
			               decoder, input, (int)input_len, &channels,
			               &decoded, &samples);
			if (used < 0 || (size_t)used > input_len) {
				snprintf(error, sizeof(error), "%s",
				         tr(STR_ERR_DECODER_BLOCK));
				break;
			}
			if (used > 0) {
				memmove(input, input + used, input_len - (size_t)used);
				input_len -= (size_t)used;
			}
			if (samples > 0) {
				if (channels != info.channels) {
					snprintf(error, sizeof(error), "%s",
					         tr(STR_ERR_CHANNELS_CHANGED));
					break;
				}
				decoded_samples = samples;
				decoded_offset = 0;
				continue;
			}
			if (used > 0)
				continue;
			decoder_needs_more = true;
		}

		__sync_synchronize();
		available = job.stream.downloaded;
		bool no_more_data = job.done && source_pos >= available &&
		                    (!decoder_open || decoder_needs_more);
		if (no_more_data && !eof) {
			if (job.stream.dl_failed) {
				snprintf(error, sizeof(error),
				         tr(STR_ERR_DOWNLOAD_FAILED_FMT), job.stream.dl_err);
				break;
			}
			if (!decoder_open) {
				snprintf(error, sizeof(error), "%s", tr(STR_ERR_NO_VORBIS));
				break;
			}
			input_len = 0;
			eof = true;
		}

		if (input_len == PLAYER_INPUT_CAPACITY) {
			snprintf(error, sizeof(error), "%s",
			         tr(STR_ERR_VORBIS_TOO_LARGE));
			break;
		}
		if (eof && pcm_frames == 0 && !final_wave_submitted)
			final_wave_submitted = true;
		if (eof && pcm_frames > 0 && !final_wave_submitted) {
			wave_index = free_wave_index(waves);
			if (wave_index >= 0) {
				waves[wave_index].data_pcm16 =
					(int16_t *)((uint8_t *)audio +
					wave_index * PLAYER_FRAMES_PER_BUFFER * 4);
				memcpy(waves[wave_index].data_pcm16,
				       pcm, pcm_frames * 4);
				submit_wave(&waves[wave_index], pcm_frames);
				wave_submitted[wave_index] = true;
				total_audio_frames += pcm_frames;
				pcm_frames = 0;
				final_wave_submitted = true;
				continue;
			}
		}
		if (eof && pcm_frames == 0 && final_wave_submitted) {
			bool playing = false;
			for (int i = 0; i < PLAYER_WAVE_BUFFERS; i++)
				if (waves[i].status == NDSP_WBUF_QUEUED ||
				    waves[i].status == NDSP_WBUF_PLAYING)
					playing = true;
			if (!playing) {
				success = total_audio_frames > 0;
				if (!success)
					snprintf(error, sizeof(error), "%s",
					         tr(STR_ERR_NO_AUDIO));
				break;
			}
		}

		frame();
		if (!aptMainLoop())
			break;
	}

cleanup:
	download_stop_join(&job);
	if (decoder)
		stb_vorbis_close(decoder);
	if (ndsp_ready) {
		ndspChnWaveBufClear(0);
		ndspExit();
	}
	if (job.stream.data)
		secure_clear(job.stream.data, job.stream.len);
	track_stream_free(&job.stream);
	if (input) {
		secure_clear(input, PLAYER_INPUT_CAPACITY);
		free(input);
	}
	if (audio)
		linearFree(audio);
	secure_clear(&job, sizeof(job));

	if (cancelled) {
		playback_result result =
			playback_cancel_result(exit_requested, track_changed);
		if (result != PLAYER_RETURN_RESULTS)
			return result;
		return_to_results = true;
	}
	if (!success) {
		if (return_to_results)
			return PLAYER_RETURN_RESULTS;
		show_playback_failed(error);
		while (aptMainLoop()) {
			hidScanInput();
			u32 down = hidKeysDown();
			if (down & KEY_START)
				return PLAYER_EXIT_APP;
			if (down & KEY_B)
				return PLAYER_RETURN_RESULTS;
			if (language_toggled(down))
				show_playback_failed(error);
			frame();
		}
		return PLAYER_EXIT_APP;
	}

	show_playback_finished(selected);
	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown();
		if (down & KEY_START)
			return PLAYER_EXIT_APP;
		if (down & KEY_B)
			return PLAYER_RETURN_RESULTS;
		if (language_toggled(down))
			show_playback_finished(selected);
		frame();
	}
	return PLAYER_EXIT_APP;
}

/* Renders a GPU frame only when something changed, else just waits for
 * VBlank: rendering shares the main thread with the Vorbis decoder. While a
 * track plays the status line changes once a second, so that is one GPU
 * frame per second.
 *
 * Both screens are drawn together. Until the F0 spike confirms on hardware
 * that a target left out of a frame keeps its last image, skipping one is
 * not worth the risk; the top screen costs a rectangle and a quad. */
static void frame(void)
{
	artwork_poll(&cover_art);
	bool restored = ui_take_restored();
	if (!restored && !artwork_dirty(&cover_art) && !textscreen_dirty()) {
		gspWaitForVBlank();
		return;
	}
	ui_begin();
	ui_scene(GFX_TOP);
	artwork_draw(&cover_art);
	ui_scene(GFX_BOTTOM);
	textscreen_draw();
	ui_end();
}

static void show_progress(const char *message)
{
	textscreen_clear();
	textscreen_printf("%s\n\n%s\n", tr(STR_LOGIN_TITLE), message);
	frame();
}

static void show_search_progress(const char *query)
{
	textscreen_clear();
	textscreen_printf("%s\n\n", tr(STR_SEARCH_PROGRESS_TITLE));
	textscreen_printf("%s: %s\n\n", tr(STR_LABEL_QUERY), query);
	textscreen_printf("%s\n", tr(STR_SEARCH_CONNECTING));
	frame();
}

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
	job->done = true;
}

static search_result search_tracks(const spotify_session *session,
                                   const char *query)
{
	memset(&search, 0, sizeof(search));
	snprintf(search.query, sizeof(search.query), "%s", query);
	snprintf(search.host, sizeof(search.host), "%s", session->host);
	snprintf(search.library_token, sizeof(search.library_token), "%s",
	         session->library_token);

	show_search_progress(search.query);
	Thread thread = threadCreate(search_worker, &search, 16 * 1024,
	                             0x18, -2, false);
	if (!thread) {
		snprintf(search.error, sizeof(search.error), "%s",
		         tr(STR_ERR_SEARCH_THREAD));
		search.result = -1;
		secure_clear(search.library_token, sizeof(search.library_token));
		return SEARCH_REQUEST_ERROR;
	}

	while (aptMainLoop() && !search.done) {
		hidScanInput();
		u32 down = hidKeysDown();
		if (down & KEY_START)
			search.exit_requested = true;
		if (!search.cancel_requested && (down & (KEY_B | KEY_START)))
			search.cancel_requested = true;
		if (search.cancel_requested)
			player_update_line(tr(STR_SEARCH_CANCELLING));
		frame();
	}
	if (!aptMainLoop()) {
		search.cancel_requested = true;
		search.exit_requested = true;
	}
	threadJoin(thread, U64_MAX);
	threadFree(thread);
	secure_clear(search.library_token, sizeof(search.library_token));
	secure_clear(search.host, sizeof(search.host));
	if (search.cancel_requested)
		return search.exit_requested
		       ? SEARCH_REQUEST_EXIT : SEARCH_REQUEST_CANCELLED;
	return search.result == 0 ? SEARCH_REQUEST_OK : SEARCH_REQUEST_ERROR;
}

static bool ask_search_query(char query[SEARCH_QUERY_SIZE])
{
	SwkbdState keyboard;
	swkbdInit(&keyboard, SWKBD_TYPE_WESTERN, 1, SEARCH_QUERY_SIZE - 1);
	swkbdSetValidation(&keyboard, SWKBD_NOTEMPTY_NOTBLANK,
	                   SWKBD_FILTER_PROFANITY, -1);
	swkbdSetHintText(&keyboard, tr(STR_SEARCH_HINT));
	bool confirmed = swkbdInputText(&keyboard, query, SEARCH_QUERY_SIZE) ==
	                 SWKBD_BUTTON_CONFIRM;
	/* The keyboard applet takes over both screens. */
	artwork_invalidate(&cover_art);
	textscreen_invalidate();
	return confirmed;
}

static void show_search_error(const char *query, const char *error)
{
	textscreen_clear();
	textscreen_printf("%s\n\n", tr(STR_SEARCH_FAILED_TITLE));
	textscreen_printf("%s: %s\n\n", tr(STR_LABEL_QUERY), query);
	textscreen_printf("%s\n\n", error);
	str_id advice = STR_SEARCH_CHECK_WIFI;
	if (strstr(error, "HTTP 401"))
		advice = STR_SEARCH_HTTP_401;
	else if (strstr(error, "HTTP 403"))
		advice = STR_SEARCH_HTTP_403;
	else if (strstr(error, "HTTP 429"))
		advice = STR_SEARCH_HTTP_429;
	textscreen_printf("%s\n\n", tr(advice));
	textscreen_printf("%s\n", tr(STR_KEYS_SEARCH_RETRY));
	textscreen_printf("%s\n", tr(STR_KEYS_BACK_EXIT));
	textscreen_printf("%s\n", tr(STR_KEYS_LANGUAGE));
}

static void show_results(const char *query, int selected)
{
	int first = (selected / RESULTS_PER_PAGE) * RESULTS_PER_PAGE;
	int page_count = (search.count + RESULTS_PER_PAGE - 1) /
	                 RESULTS_PER_PAGE;
	textscreen_clear();
	textscreen_printf("%s\n", tr(STR_RESULTS_TITLE));
	textscreen_printf("%s: %.44s\n\n", tr(STR_LABEL_QUERY), query);
	if (search.count == 0) {
		textscreen_printf("%s\n\n", tr(STR_RESULTS_EMPTY));
		textscreen_printf("%s\n", tr(STR_KEYS_NO_RESULTS));
		textscreen_printf("%s\n", tr(STR_KEYS_EXIT));
		textscreen_printf("%s\n", tr(STR_KEYS_LANGUAGE));
		return;
	}

	for (int i = first; i < search.count &&
	     i < first + RESULTS_PER_PAGE; i++) {
		const sp_track *track = &search.tracks[i];
		int seconds = track->duration_ms > 0
		              ? track->duration_ms / 1000 : 0;
		textscreen_printf("%c %02d. %.39s\n", i == selected ? '>' : ' ', i + 1,
		                  track->name[0] ? track->name : tr(STR_UNTITLED));
		textscreen_printf("   %.29s  %d:%02d\n", track->artist[0] ? track->artist
		                  : tr(STR_UNKNOWN_ARTIST), seconds / 60, seconds % 60);
	}
	textscreen_printf("\n");
	textscreen_printf(tr(STR_RESULTS_PAGE_FMT),
	                  first / RESULTS_PER_PAGE + 1, page_count, search.count);
	textscreen_printf("\n%s\n", tr(STR_KEYS_RESULTS_MOVE));
	textscreen_printf("%s\n", tr(STR_KEYS_RESULTS_NAV));
	textscreen_printf("%s\n", tr(STR_KEYS_LANGUAGE));
}

static void prepare_queue(void)
{
	playback_queue_count = search.count;
	memcpy(playback_queue, search.tracks,
	       (size_t)playback_queue_count * sizeof(playback_queue[0]));
}

static void show_selection(int selected)
{
	const sp_track *track = &playback_queue[selected];
	textscreen_clear();
	textscreen_printf("%s\n\n", tr(STR_SELECTION_TITLE));
	textscreen_printf("%.42s\n", track->name);
	textscreen_printf("%s: %.42s\n", tr(STR_LABEL_ARTIST), track->artist);
	textscreen_printf("%s: %.42s\n", tr(STR_LABEL_ALBUM), track->album);
	textscreen_printf("%s: %d:%02d\n\n", tr(STR_LABEL_DURATION),
	                  track->duration_ms / 60000,
	                  (track->duration_ms / 1000) % 60);
	textscreen_printf("%s: %d/%d\n", tr(STR_LABEL_QUEUE), selected + 1,
	                  playback_queue_count);
	textscreen_printf("%s\n", tr(STR_KEYS_SELECTION_PLAY));
	textscreen_printf("%s\n", tr(STR_KEYS_SELECTION_SEARCH));
	textscreen_printf("%s\n", tr(STR_KEYS_SELECTION_SESSION));
	textscreen_printf("%s\n", tr(STR_KEYS_LANGUAGE));
}

static bool search_flow(spotify_session *session,
                        char query[SEARCH_QUERY_SIZE])
{
	for (;;) {
		if (!ask_search_query(query))
			return true;
		if (!query[0])
			continue;
		search_result result = search_tracks(session, query);
		if (result == SEARCH_REQUEST_CANCELLED)
			return true;
		if (result == SEARCH_REQUEST_EXIT)
			return false;
		if (result == SEARCH_REQUEST_ERROR) {
			for (;;) {
				show_search_error(query, search.error);
				while (aptMainLoop()) {
					hidScanInput();
					u32 down = hidKeysDown();
					if (down & KEY_START)
						return false;
					if (down & KEY_B)
						return true;
					if (down & KEY_Y)
						break;
					if (language_toggled(down))
						show_search_error(query, search.error);
					if (down & KEY_A) {
						result = search_tracks(session, query);
						if (result == SEARCH_REQUEST_CANCELLED)
							return true;
						if (result == SEARCH_REQUEST_EXIT)
							return false;
						if (result == SEARCH_REQUEST_ERROR)
							break;
						goto show_results_screen;
					}
					frame();
				}
				if (!aptMainLoop())
					return false;
				if (hidKeysDown() & KEY_Y)
					break;
			}
			continue;
		}

show_results_screen:
		if (search.count == 0) {
			show_results(query, 0);
			for (;;) {
				while (aptMainLoop()) {
					hidScanInput();
					u32 down = hidKeysDown();
					if (down & KEY_START)
						return false;
					if (down & KEY_B)
						return true;
					if (down & KEY_Y)
						break;
					if (language_toggled(down))
						show_results(query, 0);
					frame();
				}
				if (!aptMainLoop())
					return false;
				if (hidKeysDown() & KEY_Y)
					break;
			}
			continue;
		}

		int selected = 0;
		show_results(query, selected);
		for (;;) {
			hidScanInput();
			u32 down = hidKeysDown();
			if (down & KEY_START)
				return false;
			if (down & KEY_B)
				return true;
			if (down & KEY_Y)
				break;
			bool selection_changed = false;
			if (down & KEY_UP && selected > 0) {
				selected--;
				selection_changed = true;
			}
			if (down & KEY_DOWN && selected + 1 < search.count) {
				selected++;
				selection_changed = true;
			}
			if (language_toggled(down))
				selection_changed = true;
			if (selection_changed)
				show_results(query, selected);
			if (down & KEY_A) {
				prepare_queue();
				int queue_index = selected;
				show_selection(queue_index);
				for (;;) {
					hidScanInput();
					down = hidKeysDown();
					if (down & KEY_START)
						return false;
					if (down & KEY_X)
						return true;
					if (down & KEY_B)
						break;
					if (down & KEY_Y)
						break;
					if (language_toggled(down))
						show_selection(queue_index);
					if (down & KEY_A) {
						for (;;) {
							playback_result result =
								playback(session, playback_queue,
								         playback_queue_count,
								         &queue_index);
							if (result == PLAYER_EXIT_APP)
								return false;
							if (result == PLAYER_TRACK_CHANGED)
								continue;
							if (result == PLAYER_RETURN_RESULTS) {
								selected = queue_index;
								goto return_to_results;
							}
							break;
						}
						show_selection(queue_index);
					}
					frame();
				}
				if (!aptMainLoop())
					return false;
				if (hidKeysDown() & KEY_Y)
					break;
				show_results(query, selected);
				continue;
return_to_results:
				show_results(query, selected);
				continue;
			}
			frame();
			if (!aptMainLoop())
				return false;
		}
		if (!aptMainLoop())
			return false;
		if (hidKeysDown() & KEY_Y)
			continue;
	}
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

static void show_session_ready(const spotify_session *session, const char *query)
{
	textscreen_clear();
	textscreen_printf("%s\n\n", tr(STR_SESSION_READY_TITLE));
	textscreen_printf("%s: %s\n\n", tr(STR_LABEL_SERVER), session->host);
	textscreen_printf("%s\n", tr(STR_KEYS_SESSION_SEARCH));
	textscreen_printf("%s: %.40s\n\n", tr(STR_LABEL_PREVIOUS_QUERY),
	                  query[0] ? query : tr(STR_NONE));
	textscreen_printf("%s\n", tr(STR_KEYS_SESSION_RECONNECT));
	textscreen_printf("%s\n", tr(STR_KEYS_SESSION_PAIR));
	textscreen_printf("%s\n", tr(STR_KEYS_EXIT));
	textscreen_printf("%s\n", tr(STR_KEYS_LANGUAGE));
}

int main(int argc, char **argv)
{
	(void)argc;
	gfxInitDefault();
	/* Without the renderer there is no way to show anything at all. */
	if (!ui_init()) {
		gfxExit();
		return 1;
	}
	if (!textscreen_init()) {
		ui_exit();
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
	char query[SEARCH_QUERY_SIZE] = {0};

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
		textscreen_clear();
		textscreen_printf("%s\n\n", tr(STR_LOGIN_TITLE));
		textscreen_printf("%s\n", tr(STR_LOGIN_CONNECTING));
		frame();
		if (start_session(force_pair, &error, &session)) {
			show_session_ready(&session, query);
			for (;;) {
				hidScanInput();
				u32 down = hidKeysDown();
				if (down & KEY_START)
					goto done;
				if (down & KEY_A) {
					if (!search_flow(&session, query))
						goto done;
					show_session_ready(&session, query);
				}
				if (down & KEY_X) {
					secure_clear(&session, sizeof(session));
					force_pair = false;
					break;
				}
				if (down & KEY_Y) {
					secure_clear(&session, sizeof(session));
					force_pair = true;
					break;
				}
				if (language_toggled(down))
					show_session_ready(&session, query);
				frame();
				if (!aptMainLoop())
					goto done;
			}
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
	textscreen_exit();
	ui_exit();
	gfxExit();
	return 0;
}
