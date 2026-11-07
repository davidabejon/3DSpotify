#include "top_now.h"

#include "draw.h"
#include "lang.h"
#include "now_layout.h"
#include "player_logic.h"
#include "theme.h"

#include <citro2d.h>

#include <stdio.h>

#define STATIC_GLYPHS 64
#define TRACK_GLYPHS (3 * UI_FIELD_SIZE)
#define TIME_GLYPHS 24
#define SCRATCH_GLYPHS UI_FIELD_SIZE

static C2D_TextBuf static_buf, track_buf, time_buf, scratch_buf;
static C2D_Text finished_text;
static C2D_Text title_text, artist_text, album_text;
static C2D_Text elapsed_text, duration_text;
static bool has_album;

/* What was parsed, to parse again only on a change. */
static lang_id parsed_lang = LANG_COUNT;
static unsigned parsed_serial = ~0u;
static const sp_track *parsed_track;
static unsigned parsed_elapsed = ~0u, parsed_duration = ~0u;

static player_info info;
static player_download_sample download;

typedef struct {
	bool big;
	player_phase phase;
	unsigned serial;
	unsigned elapsed, duration, download;
	lang_id lang;
} now_look;
static now_look drawn;
static bool drawn_valid;

/* --- Lifetime --------------------------------------------------------- */

bool top_now_init(void)
{
	static_buf = C2D_TextBufNew(STATIC_GLYPHS);
	track_buf = C2D_TextBufNew(TRACK_GLYPHS);
	time_buf = C2D_TextBufNew(TIME_GLYPHS);
	scratch_buf = C2D_TextBufNew(SCRATCH_GLYPHS);
	if (!static_buf || !track_buf || !time_buf || !scratch_buf) {
		top_now_exit();
		return false;
	}
	return true;
}

void top_now_exit(void)
{
	C2D_TextBuf *bufs[] = { &static_buf, &track_buf, &time_buf,
	                        &scratch_buf };
	for (size_t i = 0; i < sizeof(bufs) / sizeof(bufs[0]); i++) {
		if (*bufs[i])
			C2D_TextBufDelete(*bufs[i]);
		*bufs[i] = NULL;
	}
	parsed_lang = LANG_COUNT;
	parsed_serial = ~0u;
	parsed_track = NULL;
	parsed_elapsed = parsed_duration = ~0u;
	drawn_valid = false;
}

void top_now_set(const player_info *next)
{
	bool tick = next->elapsed_s != info.elapsed_s ||
	            next->phase != info.phase || next->serial != info.serial;
	info = *next;
	/* Only the fields above are read: the error and the stage belong to
	 * the bottom screen. */
	info.error = NULL;
	player_sample_download(&download, tick, info.download_pct);
}

void top_now_invalidate(void)
{
	drawn_valid = false;
}

/* --- Text ------------------------------------------------------------- */

static void parse(C2D_Text *text, C2D_TextBuf buf, const char *string)
{
	C2D_TextParse(text, buf, string);
	C2D_TextOptimize(text);
}

static void parse_track(void)
{
	C2D_TextBufClear(track_buf);
	const sp_track *t = info.track;
	const char *name = t && t->name[0] ? t->name : tr(STR_UNTITLED);
	const char *artist = t && t->artist[0] ? t->artist
	                                       : tr(STR_UNKNOWN_ARTIST);
	int width = now_text_w();
	ui_parse_fitted(&title_text, track_buf, scratch_buf, name, TXT_TITULO,
	                width);
	ui_parse_fitted(&artist_text, track_buf, scratch_buf, artist,
	                TXT_CUERPO, width);
	has_album = t && t->album[0];
	if (has_album)
		ui_parse_fitted(&album_text, track_buf, scratch_buf, t->album,
		                TXT_CUERPO, width);
}

static void parse_times(void)
{
	C2D_TextBufClear(time_buf);
	char text[16];
	player_format_time(info.elapsed_s, text, sizeof(text));
	parse(&elapsed_text, time_buf, text);
	player_format_time(info.duration_s, text, sizeof(text));
	parse(&duration_text, time_buf, text);
}

static void ensure_parsed(void)
{
	bool lang_changed = parsed_lang != lang_get();
	if (lang_changed) {
		C2D_TextBufClear(static_buf);
		parse(&finished_text, static_buf, tr(STR_PLAYER_FINISHED_TITLE));
	}
	if (lang_changed || parsed_serial != info.serial ||
	    parsed_track != info.track) {
		parse_track();
		parsed_serial = info.serial;
		parsed_track = info.track;
	}
	if (parsed_elapsed != info.elapsed_s ||
	    parsed_duration != info.duration_s) {
		parse_times();
		parsed_elapsed = info.elapsed_s;
		parsed_duration = info.duration_s;
	}
	parsed_lang = lang_get();
}

/* --- State ------------------------------------------------------------ */

/* The big cover shows nothing but the cover: no clock, no text. */
static now_look current_look(bool big)
{
	now_look look = { .big = big };
	if (!big) {
		look.phase = info.phase;
		look.serial = info.serial;
		look.elapsed = info.elapsed_s;
		look.duration = info.duration_s;
		look.download = download.shown;
		look.lang = lang_get();
	}
	return look;
}

static bool same_look(const now_look *a, const now_look *b)
{
	return a->big == b->big && a->phase == b->phase &&
	       a->serial == b->serial && a->elapsed == b->elapsed &&
	       a->duration == b->duration && a->download == b->download &&
	       a->lang == b->lang;
}

bool top_now_dirty(bool big, const artwork_state *art)
{
	if (!drawn_valid || artwork_dirty(art))
		return true;
	now_look look = current_look(big);
	return !same_look(&look, &drawn);
}

/* --- Drawing ---------------------------------------------------------- */

static void draw_bar(const now_look *look)
{
	int meta_h = ui_line_height(TXT_META);
	int cy = now_times_cy(meta_h);
	bool finished = look->phase == PLAYER_PHASE_FINISHED;
	/* As the bottom player (7.5): "Track finished" instead of the
	 * times. */
	if (finished) {
		ui_text(&finished_text, SP_3, cy, TXT_META, COL_TEXTO_2);
	} else {
		ui_text(&elapsed_text, SP_3, cy, TXT_META, COL_TEXTO_2);
		int w = ui_px_ceil(ui_text_width(&duration_text, TXT_META));
		ui_text(&duration_text, SCREEN_TOP_W - SP_3 - w, cy, TXT_META,
		        COL_TEXTO_2);
	}
	ui_progress(now_progress_rect(meta_h),
	            player_permille(look->download, 100),
	            player_progress_permille(finished, look->elapsed,
	                                     look->duration),
	            true);
}

void top_now_draw_card(artwork_state *art)
{
	ensure_parsed();
	now_look look = current_look(false);

	artwork_draw_in(art, now_art_rect(), ART_SHADOW_OFF);
	int title_h = ui_line_height(TXT_TITULO);
	int body_h = ui_line_height(TXT_CUERPO);
	int x = now_text_x();
	ui_text(&title_text, x, now_title_cy(title_h), TXT_TITULO, COL_TEXTO);
	ui_text(&artist_text, x, now_artist_cy(title_h, body_h), TXT_CUERPO,
	        COL_TEXTO_2);
	if (has_album)
		ui_text(&album_text, x, now_album_cy(title_h, body_h), TXT_CUERPO,
		        COL_TEXTO_2);
	draw_bar(&look);

	drawn = look;
	drawn_valid = true;
}

void top_now_draw_big(artwork_state *art)
{
	artwork_draw_in(art, now_big_art_rect(), ART_SHADOW_OFF);
	drawn = current_look(true);
	drawn_valid = true;
}
