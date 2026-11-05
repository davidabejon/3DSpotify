/* F0 del plan de citro2d: spike desechable en hardware.
 *
 * Answers one question before any UI code is migrated: can citro3d/citro2d
 * own both screens while the Vorbis decoder runs on the main thread, without
 * crackles and without running out of linear memory?
 *
 *   - bottom screen: system-font text via C2D_Text, re-parsed only when the
 *     content changes (once per second while playing);
 *   - top screen: the track's cover as a 256x256 GPU texture. Download,
 *     decode, box-resize and Morton swizzle run on a worker thread; only
 *     C3D_TexInit/copy/flush happen on the main thread;
 *   - rendering is on demand: C3D_FrameBegin/End only when a screen is dirty,
 *     otherwise just gspWaitForVBlank, as the app does today;
 *   - audio: the whole track in memory, looped until TARGET_SECONDS of audio
 *     have played, with the app's real wave buffer sizes.
 *
 * Controls while playing:
 *   A      re-download and re-upload the cover (worker + texture churn)
 *   X      toggle "draw only dirty targets" / "draw both targets"
 *   Y      toggle stress mode: render both screens every frame (60 fps)
 *   START  stop early
 *
 * Everything is also written to spotify-ui.log next to the .3dsx.
 */
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ap3ds.h"
#include "cfg3ds.h"
#include "http3ds.h"
#include "login5.h"
#include "pair3ds.h"
#include "spclient.h"
#include "track.h"
#include "tls3ds.h"

#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.h"

#define ART_MAX_BYTES (1024 * 1024)
#define ART_MAX_DIMENSION 1024
#define STBI_MAX_DIMENSIONS ART_MAX_DIMENSION
#define STB_IMAGE_IMPLEMENTATION
#include "../../include/stb_image.h"

#define SOC_BUFFER      0x100000
/* Same values as the player in main.c, so the margin measured here is the
 * margin the app will have. */
#define WAVE_BUFFERS    8
#define FRAMES_PER_BUF  2048
#define TRACK_LIMIT     (12 * 1024 * 1024)
#define TARGET_SECONDS  (10 * 60)
#define TEX_SIZE        256
#define LOG_LINES       11
#define STATUS_LINES    2
#define LINE_LEN        96

#define SAMPLE_TRACK "spotify:track:3n3Ppam7vgaVa1iaRUc9Lp"

/* Paleta del plan (seccion 2.1). */
#define COL_BG        C2D_Color32(0x10, 0x14, 0x18, 0xFF)
#define COL_SURFACE   C2D_Color32(0x1B, 0x21, 0x28, 0xFF)
#define COL_TEXT      C2D_Color32(0xF2, 0xF4, 0xF5, 0xFF)
#define COL_DIM       C2D_Color32(0x9A, 0xA5, 0xB1, 0xFF)
#define COL_ACCENT    C2D_Color32(0x4F, 0xD1, 0xC5, 0xFF)
#define COL_DANGER    C2D_Color32(0xF0, 0x71, 0x78, 0xFF)

/* ------------------------------------------------------------------ log */

static FILE *log_file;
static char log_lines[LOG_LINES][LINE_LEN];
static int log_count;
static char status_lines[STATUS_LINES][LINE_LEN];

/* ------------------------------------------------------------- renderer */

static C3D_RenderTarget *top_target;
static C3D_RenderTarget *bottom_target;
static C2D_TextBuf text_buf;
static C2D_Text log_text[LOG_LINES];
static C2D_Text status_text[STATUS_LINES];
static C2D_Text title_text;
static bool top_dirty = true;
static bool bottom_dirty = true;
static bool text_stale = true;
static bool draw_both = false;   /* X */
static bool stress = false;      /* Y */

static C3D_Tex cover_tex;
static bool cover_ready;
static C2D_Image cover_image;
static Tex3DS_SubTexture cover_sub = {
	TEX_SIZE, TEX_SIZE, 0.0f, 1.0f, 1.0f, 0.0f
};

typedef struct {
	unsigned renders;
	unsigned top_renders;
	unsigned bottom_renders;
	double render_ms_total;
	double render_ms_max;
	u32 linear_start;
	u32 linear_after_c3d;
	u32 linear_after_audio;
	u32 linear_min;
	unsigned covers_uploaded;
} ui_stats;

static ui_stats stats;

static void note_linear(void)
{
	u32 free_now = linearSpaceFree();
	if (stats.linear_min == 0 || free_now < stats.linear_min)
		stats.linear_min = free_now;
}

static void reparse_text(void)
{
	C2D_TextBufClear(text_buf);
	C2D_TextParse(&title_text, text_buf, "3DSpotify F0  citro2d spike");
	C2D_TextOptimize(&title_text);
	for (int i = 0; i < log_count; i++) {
		C2D_TextParse(&log_text[i], text_buf, log_lines[i]);
		C2D_TextOptimize(&log_text[i]);
	}
	for (int i = 0; i < STATUS_LINES; i++) {
		C2D_TextParse(&status_text[i], text_buf, status_lines[i]);
		C2D_TextOptimize(&status_text[i]);
	}
	text_stale = false;
}

static void draw_top(void)
{
	C2D_TargetClear(top_target, COL_BG);
	C2D_SceneBegin(top_target);
	/* El fondo degradado del tema: color por vertice, coste de CPU cero. */
	C2D_DrawRectangle(0, 0, 0, 400, 240,
	                  COL_SURFACE, COL_SURFACE, COL_BG, COL_BG);
	if (cover_ready) {
		/* 256 px escalada a 212, como la portada actual. */
		const float size = 212.0f;
		const float scale = size / TEX_SIZE;
		const float x = (400.0f - size) / 2.0f;
		const float y = (240.0f - size) / 2.0f;
		C2D_DrawRectSolid(x - 4, y - 4, 0, size + 8, size + 8, COL_TEXT);
		C2D_DrawImageAt(cover_image, x, y, 0, NULL, scale, scale);
	} else {
		C2D_DrawRectSolid(150, 70, 0, 100, 100, COL_SURFACE);
	}
}

static void draw_bottom(void)
{
	if (text_stale)
		reparse_text();
	C2D_TargetClear(bottom_target, COL_BG);
	C2D_SceneBegin(bottom_target);
	C2D_DrawRectSolid(0, 0, 0, 320, 24, COL_SURFACE);
	C2D_DrawRectSolid(0, 0, 0, 4, 24, COL_ACCENT);
	C2D_DrawText(&title_text, C2D_WithColor, 10, 4, 0, 0.55f, 0.55f,
	             COL_TEXT);
	for (int i = 0; i < log_count; i++) {
		u32 color = COL_DIM;
		if (log_lines[i][0] == '!')
			color = COL_DANGER;
		else if (log_lines[i][0] == '>')
			color = COL_TEXT;
		C2D_DrawText(&log_text[i], C2D_WithColor, 8, 28 + i * 15.0f, 0,
		             0.46f, 0.46f, color);
	}
	C2D_DrawRectSolid(0, 196, 0, 320, 44, COL_SURFACE);
	for (int i = 0; i < STATUS_LINES; i++)
		C2D_DrawText(&status_text[i], C2D_WithColor, 8, 200 + i * 18.0f, 0,
		             0.46f, 0.46f, i == 0 ? COL_ACCENT : COL_TEXT);
}

/* Renders one GPU frame if anything is dirty; otherwise only waits for
 * VBlank. Returns true when it rendered. */
static bool ui_frame(void)
{
	bool want_top = top_dirty || stress;
	bool want_bottom = bottom_dirty || stress;
	if (draw_both && (want_top || want_bottom))
		want_top = want_bottom = true;
	if (!want_top && !want_bottom) {
		gspWaitForVBlank();
		return false;
	}

	TickCounter t;
	osTickCounterStart(&t);
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	if (want_top) {
		draw_top();
		stats.top_renders++;
	}
	if (want_bottom) {
		draw_bottom();
		stats.bottom_renders++;
	}
	C3D_FrameEnd(0);
	osTickCounterUpdate(&t);
	double ms = osTickCounterRead(&t);
	stats.renders++;
	stats.render_ms_total += ms;
	if (ms > stats.render_ms_max)
		stats.render_ms_max = ms;
	top_dirty = bottom_dirty = false;
	note_linear();
	return true;
}

/* Appends to the on-screen log (scrolling) and the SD log, then paints it
 * right away: the network steps block for seconds at a time. */
static void say(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (log_file) {
		fputs(line, log_file);
		fputc('\n', log_file);
		fflush(log_file);
	}
	if (log_count == LOG_LINES) {
		memmove(log_lines[0], log_lines[1], sizeof(log_lines[0]) *
		        (LOG_LINES - 1));
		log_count--;
	}
	/* Long lines are cut on purpose: the SD log keeps them whole. */
	size_t n = strnlen(line, LINE_LEN - 1);
	memcpy(log_lines[log_count], line, n);
	log_lines[log_count++][n] = '\0';
	text_stale = true;
	bottom_dirty = true;
	ui_frame();
}

/* The fixed status block under the log, rewritten once per second. */
static void status_line(int index, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(status_lines[index], LINE_LEN, fmt, ap);
	va_end(ap);
	text_stale = true;
	bottom_dirty = true;
}

static void open_log(const char *argv0)
{
	if (argv0 && argv0[0]) {
		char p[320];
		snprintf(p, sizeof(p), "%s", argv0);
		char *slash = strrchr(p, '/');
		if (slash) {
			slash[1] = '\0';
			strncat(p, "spotify-ui.log", sizeof(p) - strlen(p) - 1);
			log_file = fopen(p, "w");
		}
	}
	if (!log_file)
		log_file = fopen("sdmc:/spotify-ui.log", "w");
}

/* ------------------------------------------------------------ cover art */

typedef struct {
	Thread thread;
	volatile bool done;
	char url[128];
	u8 *tex_pixels;   /* TEX_SIZE^2 * 4, GPU layout, owned until uploaded */
	double worker_ms;
	char err[96];
} cover_job;

static cover_job cover;

/* Index of (x, y) inside a texture whose 8x8 tiles are in Morton order.
 * The candidate for image_util.h in F1. */
static inline u32 morton_offset(u32 x, u32 y, u32 width)
{
	u32 tile = ((y >> 3) * (width >> 3) + (x >> 3)) << 6;
	u32 in_tile = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
	              ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
	return tile + in_tile;
}

/* Box-filters RGBA (w x h) to TEX_SIZE^2 and writes GPU_RGBA8 (bytes ABGR)
 * in tiled order. Pure CPU, safe off the main thread. */
static void to_texture(const u8 *src, int w, int h, u8 *dst)
{
	for (int dy = 0; dy < TEX_SIZE; dy++) {
		int y0 = dy * h / TEX_SIZE;
		int y1 = (dy + 1) * h / TEX_SIZE;
		if (y1 <= y0)
			y1 = y0 + 1;
		for (int dx = 0; dx < TEX_SIZE; dx++) {
			int x0 = dx * w / TEX_SIZE;
			int x1 = (dx + 1) * w / TEX_SIZE;
			if (x1 <= x0)
				x1 = x0 + 1;
			u32 r = 0, g = 0, b = 0, a = 0, n = 0;
			for (int y = y0; y < y1; y++) {
				const u8 *row = src + ((size_t)y * w + x0) * 4;
				for (int x = x0; x < x1; x++, row += 4) {
					r += row[0];
					g += row[1];
					b += row[2];
					a += row[3];
					n++;
				}
			}
			u8 *p = dst + morton_offset(dx, dy, TEX_SIZE) * 4;
			p[0] = (u8)(a / n);
			p[1] = (u8)(b / n);
			p[2] = (u8)(g / n);
			p[3] = (u8)(r / n);
		}
	}
}

static void cover_worker(void *arg)
{
	cover_job *job = arg;
	TickCounter t;
	osTickCounterStart(&t);
	http_reply reply = {0};
	int rc = http_get_url(job->url, ART_MAX_BYTES, &reply);
	if (rc != 0 || reply.status != 200 || !reply.body || reply.len == 0) {
		snprintf(job->err, sizeof(job->err), "descarga rc=%d http=%d",
		         rc, reply.status);
	} else {
		int w = 0, h = 0, ch = 0;
		stbi_uc *rgba = stbi_load_from_memory((const stbi_uc *)reply.body, (int)reply.len,
		                                      &w, &h, &ch, STBI_rgb_alpha);
		if (!rgba || w <= 0 || h <= 0) {
			snprintf(job->err, sizeof(job->err), "decodificar fallo");
		} else {
			u8 *tex = malloc((size_t)TEX_SIZE * TEX_SIZE * 4);
			if (tex) {
				to_texture(rgba, w, h, tex);
				job->tex_pixels = tex;
			} else {
				snprintf(job->err, sizeof(job->err), "sin heap");
			}
		}
		if (rgba)
			stbi_image_free(rgba);
	}
	http_reply_free(&reply);
	osTickCounterUpdate(&t);
	job->worker_ms = osTickCounterRead(&t);
	__sync_synchronize();
	job->done = true;
}

/* Same rule as background_priority() in main.c: one step below the calling
 * thread. At 0x18 (the value artwork.c uses) the worker outranks the main
 * thread, and its TLS handshake plus JPEG decode starve the Vorbis decoder
 * for seconds: the music stops until the cover arrives. */
static s32 background_priority(void)
{
	s32 priority = 0x30;
	if (R_FAILED(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE)))
		priority = 0x30;
	return priority < 0x3F ? priority + 1 : 0x3F;
}

static void cover_request(const char *url)
{
	if (cover.thread || !url || !url[0])
		return;
	memset(&cover, 0, sizeof(cover));
	snprintf(cover.url, sizeof(cover.url), "%s", url);
	cover.thread = threadCreate(cover_worker, &cover, 32 * 1024,
	                            background_priority(), -2, false);
	if (!cover.thread)
		say("! threadCreate de portada fallo");
}

/* Main thread only: every citro3d call and linearAlloc stays here. */
static void cover_poll(void)
{
	if (!cover.thread || !cover.done)
		return;
	__sync_synchronize();
	threadJoin(cover.thread, U64_MAX);
	threadFree(cover.thread);
	cover.thread = NULL;
	if (!cover.tex_pixels) {
		say("! portada: %s", cover.err);
		return;
	}
	TickCounter t;
	osTickCounterStart(&t);
	if (cover_ready) {
		C3D_TexDelete(&cover_tex);
		cover_ready = false;
	}
	if (C3D_TexInit(&cover_tex, TEX_SIZE, TEX_SIZE, GPU_RGBA8)) {
		memcpy(cover_tex.data, cover.tex_pixels,
		       (size_t)TEX_SIZE * TEX_SIZE * 4);
		C3D_TexFlush(&cover_tex);
		C3D_TexSetFilter(&cover_tex, GPU_LINEAR, GPU_LINEAR);
		cover_image.tex = &cover_tex;
		cover_image.subtex = &cover_sub;
		cover_ready = true;
		stats.covers_uploaded++;
	}
	osTickCounterUpdate(&t);
	free(cover.tex_pixels);
	cover.tex_pixels = NULL;
	note_linear();
	say("> portada %s: worker %.0f ms, subida %.1f ms",
	    cover_ready ? "ok" : "SIN MEMORIA LINEAL", cover.worker_ms,
	    osTickCounterRead(&t));
	top_dirty = true;
}

static void cover_close(void)
{
	if (cover.thread) {
		threadJoin(cover.thread, U64_MAX);
		threadFree(cover.thread);
		cover.thread = NULL;
	}
	free(cover.tex_pixels);
	cover.tex_pixels = NULL;
	if (cover_ready) {
		C3D_TexDelete(&cover_tex);
		cover_ready = false;
	}
}

/* ----------------------------------------------------------------- main */

static void wait_for_start(void)
{
	say("START sale.");
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
		ui_frame();
	}
}

static void log_summary(double audio_s, int underruns, double decode_ms)
{
	say("> %.0f s de audio, %d underruns", audio_s, underruns);
	if (decode_ms > 0)
		say("  decoder %.2fx tiempo real", audio_s * 1000.0 / decode_ms);
	say("  frames GPU %u (arriba %u, abajo %u)", stats.renders,
	    stats.top_renders, stats.bottom_renders);
	if (stats.renders)
		say("  frame medio %.2f ms, max %.2f ms",
		    stats.render_ms_total / stats.renders, stats.render_ms_max);
	say("  lineal KB: inicio %lu, tras C3D %lu, tras audio %lu, min %lu",
	    (unsigned long)(stats.linear_start / 1024),
	    (unsigned long)(stats.linear_after_c3d / 1024),
	    (unsigned long)(stats.linear_after_audio / 1024),
	    (unsigned long)(stats.linear_min / 1024));
	say("  portadas subidas: %u", stats.covers_uploaded);
	say(underruns == 0 ? "> PUERTA F0: sin underruns" :
	                     "! PUERTA F0: hubo underruns");
}

int main(int argc, char **argv)
{
	(void)argc;
	gfxInitDefault();
	stats.linear_start = linearSpaceFree();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();
	top_target = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	bottom_target = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	text_buf = C2D_TextBufNew(4096);
	stats.linear_after_c3d = linearSpaceFree();
	note_linear();

	const char *argv0 = (argv && argv[0]) ? argv[0] : NULL;
	open_log(argv0);
	cfg_set_path(argv0);

	say("built %s", BUILD_STAMP);
	say("lineal libre: %lu KB, tras C3D+C2D %lu KB",
	    (unsigned long)(stats.linear_start / 1024),
	    (unsigned long)(stats.linear_after_c3d / 1024));
	say("Acentos: canción, ñandú, 日本語");

	u32 *soc = memalign(0x1000, SOC_BUFFER);
	if (!soc || R_FAILED(socInit(soc, SOC_BUFFER))) {
		say("! socInit fallo; wifi encendido?");
		goto hold;
	}
	if (R_FAILED(psInit())) {
		say("! psInit fallo");
		goto hold;
	}
	int rc = tls_global_init();
	if (rc) {
		char reason[128];
		tls_strerror(rc, reason, sizeof(reason));
		say("! tls_global_init: %s", reason);
		goto hold;
	}

	char err[256];
	char stored[512];
	if (!cfg_load_refresh_token(stored, sizeof(stored))) {
		say("! Sin parear. Corre spotify3ds o probe_pair primero.");
		goto hold;
	}
	spotify_tokens tok;
	say("1. token OAuth...");
	if (token_refresh(stored, &tok, err, sizeof(err)) != 0) {
		say("! %s", err);
		goto hold;
	}
	if (tok.refresh_token[0] && strcmp(tok.refresh_token, stored) != 0)
		cfg_save_refresh_token(tok.refresh_token, tok.scope);

	char device_id[64];
	ap_device_id(device_id, sizeof(device_id));
	say("2. access point...");
	ap_candidate cands[AP_MAX_CANDIDATES];
	int ncands = ap_resolve_list(cands, AP_MAX_CANDIDATES, err, sizeof(err));
	if (ncands < 1) {
		say("! %s", err);
		goto hold;
	}
	ap_info info;
	int tried = 0;
	ap_session *ap = ap_login_any(cands, ncands, tok.access_token, device_id,
	                              &info, &tried, err, sizeof(err));
	if (!ap) {
		say("! %s", err);
		goto hold;
	}
	say("3. token de biblioteca...");
	login5_token l5;
	if (login5_from_credentials(info.username, info.reusable, info.reusable_len,
	                            device_id, &l5, err, sizeof(err)) != 0) {
		say("! %s", err);
		ap_close(ap);
		goto hold;
	}
	char sp_host[128], ignored[128];
	sp_resolve(sp_host, sizeof(sp_host), ignored, sizeof(ignored));

	say("4. pista completa...");
	track_audio tr;
	rc = track_fetch(sp_host, l5.access_token, ap, SAMPLE_TRACK, 0,
	                 TRACK_LIMIT, NULL, &tr, err, sizeof(err));
	ap_close(ap);
	if (rc != 0) {
		say("! %s", err);
		goto hold;
	}
	say("> %s - %s", tr.meta.artist, tr.meta.name);
	cover_request(tr.meta.art_large);

	int verr = 0;
	stb_vorbis *v = stb_vorbis_open_memory(track_vorbis(&tr),
	                                       (int)track_vorbis_len(&tr),
	                                       &verr, NULL);
	if (!v) {
		say("! stb_vorbis error %d", verr);
		track_free(&tr);
		goto hold;
	}
	stb_vorbis_info vi = stb_vorbis_get_info(v);
	if (R_FAILED(ndspInit())) {
		say("! ndspInit fallo (falta dspfirm.cdc?)");
		stb_vorbis_close(v);
		track_free(&tr);
		goto hold;
	}
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
	ndspChnSetRate(0, (float)vi.sample_rate);
	ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
	float mix[12] = {0};
	mix[0] = mix[1] = 1.0f;
	ndspChnSetMix(0, mix);

	const u32 buf_bytes = FRAMES_PER_BUF * 4;
	u8 *audio = linearAlloc(buf_bytes * WAVE_BUFFERS);
	static ndspWaveBuf waves[WAVE_BUFFERS];
	if (!audio) {
		say("! sin memoria lineal para audio");
		ndspExit();
		stb_vorbis_close(v);
		track_free(&tr);
		goto hold;
	}
	stats.linear_after_audio = linearSpaceFree();
	note_linear();
	memset(waves, 0, sizeof(waves));
	for (int i = 0; i < WAVE_BUFFERS; i++) {
		waves[i].data_vaddr = audio + i * buf_bytes;
		waves[i].status = NDSP_WBUF_DONE;
	}

	say("A portada  X ambos  Y estres  START fin");

	unsigned long long frames = 0;
	int underruns = 0, loops = 0;
	double decode_ms = 0;
	u64 last_status = 0;
	bool stopped = false;
	TickCounter t;

	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown();
		if (down & KEY_START) {
			stopped = true;
			break;
		}
		if (down & KEY_A)
			cover_request(tr.meta.art_large);
		if (down & KEY_X) {
			draw_both = !draw_both;
			last_status = 0;
		}
		if (down & KEY_Y) {
			stress = !stress;
			last_status = 0;
		}

		bool starved = true;
		for (int i = 0; i < WAVE_BUFFERS; i++)
			if (waves[i].status == NDSP_WBUF_QUEUED ||
			    waves[i].status == NDSP_WBUF_PLAYING)
				starved = false;
		if (starved && frames > 0)
			underruns++;

		for (int i = 0; i < WAVE_BUFFERS; i++) {
			if (waves[i].status != NDSP_WBUF_DONE &&
			    waves[i].status != NDSP_WBUF_FREE)
				continue;
			s16 *dst = (s16 *)waves[i].data_vaddr;
			osTickCounterStart(&t);
			int got = stb_vorbis_get_samples_short_interleaved(
			              v, vi.channels, dst, FRAMES_PER_BUF * 2);
			if (got <= 0) {
				/* Loop the track: the gate is minutes of audio,
				 * not one song. */
				stb_vorbis_seek_start(v);
				loops++;
				got = stb_vorbis_get_samples_short_interleaved(
				          v, vi.channels, dst, FRAMES_PER_BUF * 2);
			}
			osTickCounterUpdate(&t);
			decode_ms += osTickCounterRead(&t);
			if (got <= 0)
				break;
			frames += (unsigned)got;
			waves[i].nsamples = got;
			DSP_FlushDataCache(dst, buf_bytes);
			ndspChnWaveBufAdd(0, &waves[i]);
		}

		double audio_s = (double)frames / vi.sample_rate;
		if (audio_s >= TARGET_SECONDS)
			break;

		cover_poll();

		u64 now = osGetTime();
		if (now - last_status >= 1000) {
			last_status = now;
			note_linear();
			status_line(0, "%02d:%02d/%02d:00 und %d  lin %lu KB min %lu",
			            (int)audio_s / 60, (int)audio_s % 60,
			            TARGET_SECONDS / 60, underruns,
			            (unsigned long)(linearSpaceFree() / 1024),
			            (unsigned long)(stats.linear_min / 1024));
			status_line(1, "%s%s  frames %u  max %.1f ms  vueltas %d",
			            draw_both ? "ambos" : "solo sucios",
			            stress ? " +ESTRES" : "", stats.renders,
			            stats.render_ms_max, loops);
		}
		ui_frame();
	}

	ndspChnWaveBufClear(0);
	log_summary((double)frames / vi.sample_rate, underruns, decode_ms);
	if (stopped)
		say("  (cortado con START antes de %d min)", TARGET_SECONDS / 60);
	stb_vorbis_close(v);
	track_free(&tr);
	ndspExit();
	linearFree(audio);

hold:
	wait_for_start();
	cover_close();
	if (log_file)
		fclose(log_file);
	tls_global_exit();
	psExit();
	socExit();
	C2D_TextBufDelete(text_buf);
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	return 0;
}
