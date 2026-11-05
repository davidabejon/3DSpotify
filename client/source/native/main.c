#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds/thread.h>

#include "ap3ds.h"
#include "cfg3ds.h"
#include "login5.h"
#include "pair3ds.h"
#include "spclient.h"
#include "track.h"
#include "tls3ds.h"
#include "stb_vorbis.h"

#define SOC_BUFFER 0x100000
#define SEARCH_LIMIT 24
#define SEARCH_QUERY_SIZE 96
#define RESULTS_PER_PAGE 7
#define PLAYER_WAVE_BUFFERS 4
#define PLAYER_FRAMES_PER_BUFFER 2048
#define PLAYER_INPUT_CAPACITY (128 * 1024)

typedef struct {
	char host[128];
	char library_token[1024];
	char oauth_token[1280];
	char refresh_token[512];
	time_t oauth_expires_at;
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
	volatile bool done;
} search_job;

typedef struct {
	const char *category;
	char detail[256];
} startup_error;

static search_job search;
static sp_track playback_queue[SEARCH_LIMIT];
static int playback_queue_count;

static void frame(void);

static void secure_clear(void *data, size_t len)
{
	volatile unsigned char *p = data;
	while (len--)
		*p++ = 0;
}

static void show_player_stage(const char *message)
{
	consoleClear();
	printf("Reproduccion local\n\n%s\n", message);
	printf("\nB: cancelar   START: salir\n");
	frame();
}

static void player_update_line(const char *message)
{
	PrintConsole *console = consoleGetDefault();
	if (!console)
		return;
	console->cursorX = 0;
	console->cursorY = 7;
	printf("%-50.50s", message);
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
		player_update_line("Cancelando la descarga...");
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
		snprintf(error, error_size, "No hay una credencial para renovar OAuth.");
		return false;
	}

	spotify_tokens tokens;
	memset(&tokens, 0, sizeof(tokens));
	if (token_refresh(session->refresh_token, &tokens, error, error_size) != 0) {
		secure_clear(&tokens, sizeof(tokens));
		return false;
	}
	if (!tokens.access_token[0]) {
		snprintf(error, error_size, "Spotify no devolvio un access token.");
		secure_clear(&tokens, sizeof(tokens));
		return false;
	}
	if (tokens.refresh_token[0] &&
	    strcmp(tokens.refresh_token, session->refresh_token) != 0) {
		if (!cfg_save_refresh_token(tokens.refresh_token, tokens.scope)) {
			snprintf(error, error_size,
			         "No se pudo guardar la credencial OAuth renovada.");
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
	int count = ap_resolve_list(candidates, AP_MAX_CANDIDATES,
	                            error, error_size);
	if (count < 1) {
		if (!error[0])
			snprintf(error, error_size, "No se encontro un access point.");
		return NULL;
	}
	char device_id[64];
	ap_device_id(device_id, sizeof(device_id));
	ap_info info;
	memset(&info, 0, sizeof(info));
	int tried = 0;
	ap_session *ap = ap_login_any(candidates, count, session->oauth_token,
	                              device_id, &info, &tried,
	                              error, error_size);
	secure_clear(device_id, sizeof(device_id));
	secure_clear(&info, sizeof(info));
	if (!ap && !error[0])
		snprintf(error, error_size, "No se pudo abrir la sesion del access point.");
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

static bool playback(spotify_session *session, const sp_track *selected)
{
	char error[256] = {0};
	download_job job;
	memset(&job, 0, sizeof(job));
	ndspWaveBuf waves[PLAYER_WAVE_BUFFERS];
	memset(waves, 0, sizeof(waves));
	uint8_t *input = NULL;
	int16_t *audio = NULL;
	bool ndsp_ready = false;
	bool decoder_open = false;
	stb_vorbis *decoder = NULL;
	bool success = false;
	bool cancelled = false;
	bool exit_requested = false;
	bool eof = false;
	bool final_wave_submitted = false;
	unsigned last_progress = UINT32_MAX;
	unsigned last_sample_rate = UINT32_MAX;
	size_t input_len = 0;
	size_t source_pos = SPOTIFY_OGG_HEADER;
	size_t pcm_frames = 0;
	size_t total_audio_frames = 0;
	int16_t pcm[PLAYER_FRAMES_PER_BUFFER * 2];
	float **decoded = NULL;
	int decoded_samples = 0;
	int decoded_offset = 0;
	stb_vorbis_info info = {0};

	show_player_stage("Preparando la salida de audio...");
	if (R_FAILED(ndspInit())) {
		snprintf(error, sizeof(error),
		         "No se pudo iniciar NDSP (DSP no disponible).");
		goto cleanup;
	}
	ndsp_ready = true;

	input = malloc(PLAYER_INPUT_CAPACITY);
	audio = linearAlloc(PLAYER_FRAMES_PER_BUFFER * 4 * PLAYER_WAVE_BUFFERS);
	if (!input || !audio) {
		snprintf(error, sizeof(error), "No hay memoria para los buffers de audio.");
		goto cleanup;
	}
	for (int i = 0; i < PLAYER_WAVE_BUFFERS; i++) {
		waves[i].data_vaddr = (uint8_t *)audio +
		                      i * PLAYER_FRAMES_PER_BUFFER * 4;
		waves[i].nsamples = PLAYER_FRAMES_PER_BUFFER;
		waves[i].status = NDSP_WBUF_DONE;
	}

	show_player_stage("Autenticando el access point...");
	ap_session *ap = open_playback_ap(session, error, sizeof(error));
	if (!ap)
		goto cleanup;

	show_player_stage("Obteniendo la clave de audio...");
	int rc = track_stream_begin(session->host, session->library_token, ap,
	                            selected->uri, NULL, &job.stream,
	                            error, sizeof(error));
	ap_close(ap);
	secure_clear(session->oauth_token, sizeof(session->oauth_token));
	session->oauth_expires_at = 0;
	if (rc != 0) {
		if (rc == AP_KEY_REFUSED)
			snprintf(error, sizeof(error),
			         "La cuenta no tiene acceso a la clave de esta pista.");
		goto cleanup;
	}
	consoleClear();
	printf("Reproduccion local\n\n");
	printf("%.42s\n", selected->name);
	printf("Artista: %.42s\n\n", selected->artist);
	printf("Descargando y reproduciendo...\n\n");
	printf("B: detener   START: salir\n");
	frame();

	job.thread = threadCreate(download_worker, &job, 32 * 1024,
	                          0x18, -2, false);
	if (!job.thread) {
		snprintf(error, sizeof(error),
		         "No se pudo iniciar la descarga en segundo plano.");
		goto cleanup;
	}

	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
	ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
	float mix[12] = {0};
	mix[0] = mix[1] = 1.0f;
	ndspChnSetMix(0, mix);

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
			break;
		}

		__sync_synchronize();
		size_t available = job.stream.downloaded;
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
					snprintf(error, sizeof(error),
					         "El decodificador devolvio un desplazamiento invalido.");
					break;
				}
				memmove(input, input + consumed, input_len - (size_t)consumed);
				input_len -= (size_t)consumed;
				info = stb_vorbis_get_info(decoder);
				if ((info.channels != 1 && info.channels != 2) ||
				    info.sample_rate == 0) {
					snprintf(error, sizeof(error),
					         "Formato Vorbis no compatible (%d canales).",
					         info.channels);
					break;
				}
				ndspChnSetRate(0, (float)info.sample_rate);
				decoder_open = true;
			} else if (vorbis_error != VORBIS_need_more_data) {
				snprintf(error, sizeof(error),
				         "No se pudo abrir el audio Vorbis (error %d).",
				         vorbis_error);
				break;
			}
		}

		int wave_index = free_wave_index(waves);
		if (pcm_frames == PLAYER_FRAMES_PER_BUFFER) {
			if (wave_index >= 0) {
				memcpy((uint8_t *)audio +
				       wave_index * PLAYER_FRAMES_PER_BUFFER * 4,
				       pcm, pcm_frames * 4);
				submit_wave(&waves[wave_index], pcm_frames);
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
				snprintf(error, sizeof(error),
				         "El decodificador devolvio un bloque invalido.");
				break;
			}
			if (used > 0) {
				memmove(input, input + used, input_len - (size_t)used);
				input_len -= (size_t)used;
			}
			if (samples > 0) {
				if (channels != info.channels) {
					snprintf(error, sizeof(error),
					         "El numero de canales Vorbis cambio durante la pista.");
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
				snprintf(error, sizeof(error), "Fallo de descarga: %.190s",
				         job.stream.dl_err);
				break;
			}
			if (!decoder_open) {
				snprintf(error, sizeof(error),
				         "La descarga no contiene una pista Vorbis valida.");
				break;
			}
			input_len = 0;
			eof = true;
		}

		if (input_len == PLAYER_INPUT_CAPACITY) {
			snprintf(error, sizeof(error),
			         "Un bloque Vorbis supera el limite de memoria.");
			break;
		}
		if (eof && pcm_frames == 0 && !final_wave_submitted)
			final_wave_submitted = true;
		if (eof && pcm_frames > 0 && !final_wave_submitted) {
			wave_index = free_wave_index(waves);
			if (wave_index >= 0) {
				memcpy((uint8_t *)audio +
				       wave_index * PLAYER_FRAMES_PER_BUFFER * 4,
				       pcm, pcm_frames * 4);
				submit_wave(&waves[wave_index], pcm_frames);
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
					snprintf(error, sizeof(error),
					         "La pista no contiene audio reproducible.");
				break;
			}
		}

		unsigned percent = job.stream.total
		                   ? (unsigned)(available * 100 /
		                                job.stream.total) : 0;
		if (job.done && job.stream.dl_done)
			percent = 100;
		unsigned sample_rate = decoder_open ? info.sample_rate : 0;
		if (percent != last_progress || sample_rate != last_sample_rate) {
			char status[64];
			snprintf(status, sizeof(status),
			         "Descarga: %u%%   Audio: %u Hz",
			         percent, sample_rate);
			player_update_line(status);
			last_progress = percent;
			last_sample_rate = sample_rate;
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

	if (cancelled)
		return !exit_requested;
	if (!success) {
		consoleClear();
		printf("No se pudo reproducir la pista\n\n");
		printf("%s\n\n", error[0] ? error : "La reproduccion se interrumpio.");
		printf("B: volver   START: salir\n");
		while (aptMainLoop()) {
			hidScanInput();
			u32 down = hidKeysDown();
			if (down & KEY_START)
				return false;
			if (down & KEY_B)
				return true;
			frame();
		}
		return false;
	}

	consoleClear();
	printf("Pista terminada\n\n");
	printf("%.42s\n", selected->name);
	printf("Artista: %.42s\n\n", selected->artist);
	printf("B: volver a la seleccion   START: salir\n");
	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown();
		if (down & KEY_START)
			return false;
		if (down & KEY_B)
			return true;
		frame();
	}
	return false;
}

static void frame(void)
{
	gfxFlushBuffers();
	gfxSwapBuffers();
	gspWaitForVBlank();
}

static void show_progress(const char *message)
{
	consoleClear();
	printf("Spotify 3DS - iniciando sesion directa\n\n%s\n", message);
	frame();
}

static void show_search_progress(const char *query)
{
	consoleClear();
	printf("Buscando canciones\n\n");
	printf("Consulta: %s\n\n", query);
	printf("Conectando con Spotify y cargando resultados...\n");
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
	if (job->count >= 0 && job->count > 0) {
		for (int i = 0; i < job->count; i++)
			uri_ptrs[i] = uris[i];
		job->count = sp_track_metadata_batch(job->host,
		                                     job->library_token,
		                                     uri_ptrs, job->count,
		                                     job->tracks, error,
		                                     sizeof(error));
	}
	if (job->count < 0)
		snprintf(job->error, sizeof(job->error), "%s",
		         error[0] ? error : "No se pudieron cargar los resultados.");
	job->result = job->count < 0 ? -1 : 0;
	secure_clear(uris, sizeof(uris));
	secure_clear(uri_ptrs, sizeof(uri_ptrs));
	secure_clear(error, sizeof(error));
	job->done = true;
}

static bool search_tracks(const spotify_session *session, const char *query)
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
		snprintf(search.error, sizeof(search.error),
		         "No se pudo iniciar la tarea de busqueda.");
		search.result = -1;
		secure_clear(search.library_token, sizeof(search.library_token));
		return false;
	}

	while (aptMainLoop() && !search.done) {
		frame();
	}
	threadJoin(thread, U64_MAX);
	threadFree(thread);
	secure_clear(search.library_token, sizeof(search.library_token));
	secure_clear(search.host, sizeof(search.host));
	return search.result == 0;
}

static bool ask_search_query(char query[SEARCH_QUERY_SIZE])
{
	SwkbdState keyboard;
	swkbdInit(&keyboard, SWKBD_TYPE_WESTERN, 1, SEARCH_QUERY_SIZE - 1);
	swkbdSetValidation(&keyboard, SWKBD_NOTEMPTY_NOTBLANK,
	                   SWKBD_FILTER_PROFANITY, -1);
	swkbdSetHintText(&keyboard, "Nombre de cancion o artista");
	return swkbdInputText(&keyboard, query, SEARCH_QUERY_SIZE) ==
	       SWKBD_BUTTON_CONFIRM;
}

static void show_search_error(const char *query, const char *error)
{
	consoleClear();
	printf("No se pudo completar la busqueda\n\n");
	printf("Consulta: %s\n\n", query);
	printf("%s\n\n", error);
	printf("A: reintentar   Y: cambiar consulta\n");
	printf("B: volver   START: salir\n");
}

static void show_results(const char *query, int selected)
{
	int first = (selected / RESULTS_PER_PAGE) * RESULTS_PER_PAGE;
	int page_count = (search.count + RESULTS_PER_PAGE - 1) /
	                 RESULTS_PER_PAGE;
	consoleClear();
	printf("Resultados de Spotify\n");
	printf("Consulta: %.44s\n\n", query);
	if (search.count == 0) {
		printf("No se encontraron canciones.\n\n");
		printf("Y: nueva busqueda   B: volver\n");
		printf("START: salir\n");
		return;
	}

	for (int i = first; i < search.count &&
	     i < first + RESULTS_PER_PAGE; i++) {
		const sp_track *track = &search.tracks[i];
		int seconds = track->duration_ms > 0
		              ? track->duration_ms / 1000 : 0;
		printf("%c %02d. %.39s\n", i == selected ? '>' : ' ', i + 1,
		       track->name[0] ? track->name : "(sin titulo)");
		printf("   %.29s  %d:%02d\n", track->artist[0] ? track->artist
		       : "(artista desconocido)", seconds / 60, seconds % 60);
	}
	printf("\nPagina %d/%d  (%d canciones)\n",
	       first / RESULTS_PER_PAGE + 1, page_count, search.count);
	printf("Arriba/abajo: mover  A: seleccionar\n");
	printf("Y: nueva busqueda  B: volver  START: salir\n");
}

static void prepare_queue(int selected)
{
	playback_queue_count = 0;
	playback_queue[playback_queue_count++] = search.tracks[selected];
	for (int i = 0; i < search.count; i++) {
		if (i != selected)
			playback_queue[playback_queue_count++] = search.tracks[i];
	}
}

static void show_selection(void)
{
	const sp_track *track = &playback_queue[0];
	consoleClear();
	printf("Cancion seleccionada\n\n");
	printf("%.42s\n", track->name);
	printf("Artista: %.42s\n", track->artist);
	printf("Album: %.42s\n", track->album);
	printf("Duracion: %d:%02d\n\n",
	       track->duration_ms / 60000,
	       (track->duration_ms / 1000) % 60);
	printf("Cola preparada: %d canciones\n", playback_queue_count);
	printf("A: reproducir   B: resultados\n");
	printf("Y: nueva busqueda\n");
	printf("X: sesion   START: salir\n");
}

static bool search_flow(spotify_session *session,
                        char query[SEARCH_QUERY_SIZE])
{
	for (;;) {
		if (!ask_search_query(query))
			return true;
		if (!query[0])
			continue;
		if (!search_tracks(session, query)) {
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
					if (down & KEY_A) {
						if (!search_tracks(session, query))
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
			if (selection_changed)
				show_results(query, selected);
			if (down & KEY_A) {
				prepare_queue(selected);
				show_selection();
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
					if (down & KEY_A) {
						if (!playback(session, &playback_queue[0]))
							return false;
						show_selection();
					}
					frame();
				}
				if (!aptMainLoop())
					return false;
				if (hidKeysDown() & KEY_Y)
					break;
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
	consoleClear();
	printf("Spotify 3DS - emparejamiento\n\n");
	printf("Abre esta direccion en otro dispositivo:\n%s\n\n",
	       req->verification_uri);
	printf("Codigo: %s\n", req->user_code);
	if (req->verification_uri_complete[0])
		printf("\nEnlace directo:\n%s\n", req->verification_uri_complete);
	printf("\n%s\n", status);
	printf("Comprobando cada %d s. B cancela.\n", interval);
}

static bool pair_account(spotify_tokens *tokens, startup_error *error)
{
	pair_request req;
	char detail[192] = {0};
	show_progress("Solicitando un codigo de emparejamiento...");
	if (pair_begin(&req, detail, sizeof(detail)) != 0) {
		error->category = "Emparejamiento";
		snprintf(error->detail, sizeof(error->detail), "%s", detail);
		secure_clear(&req, sizeof(req));
		return false;
	}

	int interval = req.interval_s > 0 ? req.interval_s : 5;
	time_t deadline = time(NULL) + req.expires_in_s;
	char status[192] = "Esperando autorizacion...";
	while (aptMainLoop()) {
		show_pair_screen(&req, interval, status);
		for (int i = 0; i < interval * 60; i++) {
			hidScanInput();
			if (hidKeysDown() & KEY_B) {
				error->category = "Emparejamiento cancelado";
				snprintf(error->detail, sizeof(error->detail),
				         "No se modificaron las credenciales guardadas.");
				secure_clear(&req, sizeof(req));
				return false;
			}
			frame();
			if (!aptMainLoop()) {
				secure_clear(&req, sizeof(req));
				return false;
			}
		}
		if (time(NULL) >= deadline) {
			error->category = "Codigo de emparejamiento caducado";
			snprintf(error->detail, sizeof(error->detail),
			         "Vuelve a intentarlo para obtener un codigo nuevo.");
			secure_clear(&req, sizeof(req));
			return false;
		}

		detail[0] = '\0';
		pair_status result = pair_poll(&req, tokens, detail, sizeof(detail));
		if (result == PAIR_PENDING) {
			snprintf(status, sizeof(status), "Aun esperando autorizacion.%s%s",
			         detail[0] ? " " : "", detail);
			continue;
		}
		if (result == PAIR_SLOW_DOWN) {
			interval += 5;
			snprintf(status, sizeof(status),
			         "Spotify pidio esperar mas; nuevo intervalo: %d s.",
			         interval);
			continue;
		}
		if (result == PAIR_OK) {
			if (!tokens->refresh_token[0] ||
			    !cfg_save_refresh_token(tokens->refresh_token, tokens->scope)) {
				error->category = "No se pudieron guardar las credenciales";
				snprintf(error->detail, sizeof(error->detail),
				         "Comprueba el espacio disponible en la tarjeta SD.");
				secure_clear(tokens, sizeof(*tokens));
				secure_clear(&req, sizeof(req));
				return false;
			}
			secure_clear(&req, sizeof(req));
			return true;
		}

		error->category = result == PAIR_DENIED
		                  ? "Emparejamiento rechazado"
		                  : result == PAIR_EXPIRED
		                    ? "Codigo de emparejamiento caducado"
		                    : "Fallo de emparejamiento";
		snprintf(error->detail, sizeof(error->detail), "%s",
		         detail[0] ? detail : "No se recibio una respuesta valida.");
		secure_clear(tokens, sizeof(*tokens));
		secure_clear(&req, sizeof(req));
		return false;
	}
	secure_clear(&req, sizeof(req));
	error->category = "Emparejamiento interrumpido";
	snprintf(error->detail, sizeof(error->detail),
	         "La aplicacion se esta cerrando.");
	return false;
}

static bool resolve_and_login(spotify_tokens *tokens, startup_error *error,
                              spotify_session *session)
{
	char detail[192] = {0};
	char device_id[64];
	ap_device_id(device_id, sizeof(device_id));

	ap_candidate candidates[AP_MAX_CANDIDATES];
	show_progress("Buscando los access points de Spotify...");
	int count = ap_resolve_list(candidates, AP_MAX_CANDIDATES,
	                            detail, sizeof(detail));
	if (count < 1) {
		error->category = "No se pudo resolver el access point";
		snprintf(error->detail, sizeof(error->detail),
		         "Comprueba Wi-Fi, DNS y TLS. %s", detail);
		secure_clear(tokens, sizeof(*tokens));
		return false;
	}

	ap_info info;
	memset(&info, 0, sizeof(info));
	int tried = 0;
	show_progress("Autenticando con el access point...");
	ap_session *ap = ap_login_any(candidates, count, tokens->access_token,
	                              device_id, &info, &tried,
	                              detail, sizeof(detail));
	if (ap) {
		snprintf(session->oauth_token, sizeof(session->oauth_token), "%s",
		         tokens->access_token);
		snprintf(session->refresh_token, sizeof(session->refresh_token), "%s",
		         tokens->refresh_token);
		session->oauth_expires_at = tokens->expires_at;
	}
	secure_clear(tokens, sizeof(*tokens));
	if (!ap) {
		error->category = info.login_error
		                  ? "La cuenta fue rechazada por el access point"
		                  : "Fallo de conexion al access point";
		if (info.login_error)
			snprintf(error->detail, sizeof(error->detail), "%s (codigo %d).",
			         ap_login_error_text(info.login_error), info.login_error);
		else
			snprintf(error->detail, sizeof(error->detail), "%s",
			         detail[0] ? detail : "No respondio ningun access point.");
		secure_clear(&info, sizeof(info));
		return false;
	}

	login5_token library;
	show_progress("Abriendo la sesion de biblioteca...");
	int rc = login5_from_credentials(info.username, info.reusable,
	                                 info.reusable_len, device_id,
	                                 &library, detail, sizeof(detail));
	ap_close(ap);
	secure_clear(&info, sizeof(info));
	secure_clear(device_id, sizeof(device_id));
	if (rc != 0) {
		error->category = strstr(detail, "login5 refused:")
		                  ? "La cuenta rechazo el inicio de sesion"
		                  : "Fallo TLS o de inicio de sesion Spotify";
		snprintf(error->detail, sizeof(error->detail), "%s", detail);
		secure_clear(&library, sizeof(library));
		return false;
	}

	detail[0] = '\0';
	show_progress("Resolviendo el servidor Spotify...");
	rc = sp_resolve(session->host, sizeof(session->host),
	                 detail, sizeof(detail));
	if (rc != 0 || !session->host[0]) {
		error->category = "No se pudo resolver el servidor Spotify";
		snprintf(error->detail, sizeof(error->detail), "%s",
		         detail[0] ? detail : "Respuesta de servidor no valida.");
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
	show_progress("Renovando la credencial de Spotify...");
	if (token_refresh(stored, &tokens, detail, sizeof(detail)) != 0) {
		secure_clear(stored, sizeof(stored));
		secure_clear(&tokens, sizeof(tokens));
		if (strstr(detail, "invalid_grant")) {
			consoleClear();
			printf("La credencial guardada ha caducado.\n");
			printf("Se necesita emparejar de nuevo.\n");
			for (int i = 0; i < 90 && aptMainLoop(); i++)
				frame();
			if (!pair_account(&tokens, error))
				return false;
			return resolve_and_login(&tokens, error, session);
		}
		error->category = "Fallo OAuth o TLS al renovar la credencial";
		snprintf(error->detail, sizeof(error->detail), "%s",
		         detail[0] ? detail : "No se recibio una respuesta valida.");
		return false;
	}

	if (tokens.refresh_token[0] &&
	    strcmp(tokens.refresh_token, stored) != 0 &&
	    !cfg_save_refresh_token(tokens.refresh_token, tokens.scope)) {
		error->category = "No se pudo guardar la credencial renovada";
		snprintf(error->detail, sizeof(error->detail),
		         "Comprueba el espacio disponible en la tarjeta SD.");
		secure_clear(stored, sizeof(stored));
		secure_clear(&tokens, sizeof(tokens));
		return false;
	}
	secure_clear(stored, sizeof(stored));
	return resolve_and_login(&tokens, error, session);
}

static void show_error(const startup_error *error)
{
	consoleClear();
	printf("No se pudo iniciar la sesion\n\n");
	printf("%s\n\n", error->category ? error->category : "Error");
	printf("%s\n\n", error->detail);
	printf("A: reintentar   Y: emparejar   START: salir\n");
}

static void show_session_ready(const spotify_session *session, const char *query)
{
	consoleClear();
	printf("Sesion Spotify lista\n\n");
	printf("Servidor: %s\n\n", session->host);
	printf("A: buscar canciones\n");
	printf("Consulta anterior: %.40s\n\n", query[0] ? query : "(ninguna)");
	printf("X: volver a conectar\n");
	printf("Y: emparejar otra cuenta\n");
	printf("START: salir\n");
}

int main(int argc, char **argv)
{
	(void)argc;
	gfxInitDefault();
	consoleInit(GFX_TOP, NULL);
	cfg_set_path((argv && argv[0]) ? argv[0] : NULL);

	u32 *soc_buffer = memalign(0x1000, SOC_BUFFER);
	bool soc_ready = false;
	bool ps_ready = false;
	bool tls_ready = false;
	startup_error error = {0};
	spotify_session session = {0};
	char query[SEARCH_QUERY_SIZE] = {0};

	if (!soc_buffer || R_FAILED(socInit(soc_buffer, SOC_BUFFER))) {
		error.category = "No hay servicio de red";
		snprintf(error.detail, sizeof(error.detail),
		         "Comprueba que el Wi-Fi de la consola esta conectado.");
		goto failed;
	}
	soc_ready = true;
	if (R_FAILED(psInit())) {
		error.category = "No se pudo iniciar el servicio de seguridad";
		snprintf(error.detail, sizeof(error.detail),
		         "No se pudo habilitar la fuente de aleatoriedad de la consola.");
		goto failed;
	}
	ps_ready = true;

	{
		int rc = tls_global_init();
		if (rc != 0) {
			char reason[128];
			tls_strerror(rc, reason, sizeof(reason));
			error.category = "No se pudo inicializar TLS";
			snprintf(error.detail, sizeof(error.detail), "%s", reason);
			goto failed;
		}
	}
	tls_ready = true;

	bool force_pair = false;
	for (;;) {
		consoleClear();
		printf("Spotify 3DS - iniciando sesion directa\n\n");
		printf("Renovando credenciales y conectando...\n");
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
		if (hidKeysDown() & KEY_START)
			break;
		frame();
	}

done:
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
	gfxExit();
	return 0;
}
