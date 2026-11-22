#include "top_mini.h"

#include "draw.h"
#include "icons.h"
#include "lang.h"
#include "now_layout.h"
#include "player_logic.h"
#include "theme.h"
#include "ui_font.h"

#include <citro2d.h>

#define STATIC_GLYPHS 32
#define TRACK_GLYPHS (2 * UI_FIELD_SIZE)
#define SCRATCH_GLYPHS UI_FIELD_SIZE

static C2D_TextBuf static_buf, track_buf, scratch_buf;
static C2D_Text finished_text;
static C2D_Text title_text, artist_text;

/* What was parsed, to parse again only on a change. */
static lang_id parsed_lang = LANG_COUNT;
static unsigned parsed_serial = ~0u;
static const sp_track *parsed_track;

static player_info info;

typedef struct {
	player_phase phase;
	unsigned serial;
	bool paused;
	int band_px;
	lang_id lang;
} mini_look;
static mini_look drawn;
static bool drawn_valid;

/* --- Lifetime --------------------------------------------------------- */

bool top_mini_init(void)
{
	static_buf = C2D_TextBufNew(STATIC_GLYPHS);
	track_buf = C2D_TextBufNew(TRACK_GLYPHS);
	scratch_buf = C2D_TextBufNew(SCRATCH_GLYPHS);
	if (!static_buf || !track_buf || !scratch_buf) {
		top_mini_exit();
		return false;
	}
	return true;
}

void top_mini_exit(void)
{
	C2D_TextBuf *bufs[] = { &static_buf, &track_buf, &scratch_buf };
	for (size_t i = 0; i < sizeof(bufs) / sizeof(bufs[0]); i++) {
		if (*bufs[i])
			C2D_TextBufDelete(*bufs[i]);
		*bufs[i] = NULL;
	}
	parsed_lang = LANG_COUNT;
	parsed_serial = ~0u;
	parsed_track = NULL;
	drawn_valid = false;
}

void top_mini_set(const player_info *next)
{
	info = *next;
	/* Only the phase, the track, the pause and the times are read. */
	info.error = NULL;
}

void top_mini_invalidate(void)
{
	drawn_valid = false;
}

/* --- Text ------------------------------------------------------------- */

static void parse_track(void)
{
	C2D_TextBufClear(track_buf);
	const sp_track *t = info.track;
	const char *name = t && t->name[0] ? t->name : tr(STR_UNTITLED);
	const char *artist = t && t->artist[0] ? t->artist
	                                       : tr(STR_UNKNOWN_ARTIST);
	int width = now_mini_text_w();
	ui_parse_fitted(&title_text, track_buf, scratch_buf, name, TXT_CUERPO,
	                width);
	ui_parse_fitted(&artist_text, track_buf, scratch_buf, artist, TXT_META,
	                width);
}

static void ensure_parsed(void)
{
	bool lang_changed = parsed_lang != lang_get();
	if (lang_changed) {
		C2D_TextBufClear(static_buf);
		ui_parse_fitted(&finished_text, static_buf, scratch_buf,
		                tr(STR_PLAYER_FINISHED_TITLE), TXT_META,
		                now_mini_text_w());
	}
	if (lang_changed || parsed_serial != info.serial ||
	    parsed_track != info.track) {
		parse_track();
		parsed_serial = info.serial;
		parsed_track = info.track;
	}
	parsed_lang = lang_get();
}

/* --- State ------------------------------------------------------------ */

/* The band only moves by whole pixels: with 198 of them a four-minute
 * track takes more than a second per pixel. */
static mini_look current_look(void)
{
	bool finished = info.phase == PLAYER_PHASE_FINISHED;
	mini_look look = {
		.phase = info.phase,
		.serial = info.serial,
		.paused = info.paused,
		.band_px = info.phase == PLAYER_PHASE_LOADING
		         ? 0
		         : now_mini_band_px(finished, info.elapsed_s,
		                            info.duration_s),
		.lang = lang_get(),
	};
	return look;
}

static bool same_look(const mini_look *a, const mini_look *b)
{
	return a->phase == b->phase && a->serial == b->serial &&
	       a->paused == b->paused && a->band_px == b->band_px &&
	       a->lang == b->lang;
}

bool top_mini_dirty(const artwork_state *art)
{
	if (!drawn_valid || artwork_dirty(art))
		return true;
	mini_look look = current_look();
	return !same_look(&look, &drawn);
}

/* --- Drawing ---------------------------------------------------------- */

/* What it is doing: the equaliser while it plays, the pause sign while
 * paused; none once finished, or while it loads and the spinner has not
 * taken the place. */
static bool state_icon(const mini_look *look, icon_id *id)
{
	if (look->phase != PLAYER_PHASE_PLAYING)
		return false;
	*id = look->paused ? ICON_PAUSE : ICON_PLAYING;
	return true;
}

void top_mini_draw(artwork_state *art, ui_rect panel, int spin_step)
{
	ensure_parsed();
	mini_look look = current_look();

	ui_panel(panel);
	artwork_draw_in(art, now_mini_cover_rect(panel), 0);

	int body_h = ui_line_height(TXT_CUERPO);
	int meta_h = ui_line_height(TXT_META);
	int x = now_mini_text_x(panel);
	ui_text(&title_text, x, now_mini_title_cy(panel, body_h, meta_h),
	        TXT_CUERPO, COL_TEXTO);
	const C2D_Text *line = look.phase == PLAYER_PHASE_FINISHED
	                     ? &finished_text : &artist_text;
	ui_text(line, x, now_mini_line_cy(panel, body_h, meta_h), TXT_META,
	        COL_TEXTO_2);

	/* The download spinner, while it turns, in place of the icon. */
	icon_id id;
	if (spin_step >= 0) {
		ui_spinner_mini(now_mini_icon_rect(panel), spin_step);
	} else if (state_icon(&look, &id)) {
		C2D_Image icon = icon_image(id);
		ui_rect r = now_mini_icon_rect(panel);
		ui_icon(&icon, r.x, r.y, COL_TEXTO);
	}

	ui_rect band = now_mini_band_rect(panel);
	ui_fill(band, COL_CAPA_ALTA);
	if (look.band_px > 0)
		ui_fill((ui_rect){ band.x, band.y, look.band_px, band.h },
		        COL_CLOROFILA);

	drawn = look;
	drawn_valid = true;
}
