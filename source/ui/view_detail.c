#include "view_detail.h"

#include "detail_logic.h"
#include "draw.h"
#include "icons.h"
#include "lang.h"
#include "list_logic.h"
#include "press_logic.h"
#include "theme.h"

#include <citro2d.h>

#include <stdio.h>

#define TEXT_GLYPHS    640
#define SCRATCH_GLYPHS UI_FIELD_SIZE

/* The button bar (STYLES.md 5.6). */
enum { GLYPH_A, GLYPH_B, GLYPH_COUNT };
static const char *const glyph_letters[GLYPH_COUNT] = { "A", "B" };
static const u32 glyph_colors[GLYPH_COUNT] = { COL_BTN_A, COL_BTN_B };
static const str_id glyph_labels[GLYPH_COUNT] = {
	STR_HINT_CHOOSE, STR_HINT_BACK
};

/* Parsed text, for `parsed_doc` as it was at `parsed_serial` and
 * `parsed_state`, in `parsed_lang`. */
static C2D_TextBuf text_buf, scratch_buf;
static C2D_Text button_text[DETAIL_BUTTON_COUNT];
static C2D_Text letter_text[GLYPH_COUNT];
static C2D_Text hint_text[GLYPH_COUNT];
static C2D_Text bubble_text;
static lang_id parsed_lang = LANG_COUNT;
static const detail_doc *parsed_doc;
static unsigned parsed_serial;
static detail_state parsed_state;
/* Which buttons the track allows (6.1). */
static bool enabled[DETAIL_BUTTON_COUNT];

/* Interaction (STYLES.md 6). */
static detail_button focus = DETAIL_BUTTON_PLAY;
static press_state press_button[DETAIL_BUTTON_COUNT];
static press_state press_glyph[GLYPH_COUNT];
static touch_track touch = { -1 };
static int bubble_frames = -1;
static detail_action pending = DETAIL_ACTION_NONE;
static const press_state *pending_press;

typedef struct {
	detail_button focus;
	bool button[DETAIL_BUTTON_COUNT];
	bool glyph[GLYPH_COUNT];
	bool bubble;
} detail_look;
static detail_look drawn;
static bool drawn_valid;

/* --- Lifetime ---------------------------------------------------------- */

bool view_detail_init(void)
{
	text_buf = C2D_TextBufNew(TEXT_GLYPHS);
	scratch_buf = C2D_TextBufNew(SCRATCH_GLYPHS);
	if (!text_buf || !scratch_buf) {
		view_detail_exit();
		return false;
	}
	return true;
}

void view_detail_exit(void)
{
	if (text_buf)
		C2D_TextBufDelete(text_buf);
	if (scratch_buf)
		C2D_TextBufDelete(scratch_buf);
	text_buf = scratch_buf = NULL;
}

void view_detail_enter(void)
{
	for (int i = 0; i < DETAIL_BUTTON_COUNT; i++)
		press_reset(&press_button[i]);
	for (int i = 0; i < GLYPH_COUNT; i++)
		press_reset(&press_glyph[i]);
	touch.target = -1;
	bubble_frames = -1;
	pending = DETAIL_ACTION_NONE;
	pending_press = NULL;
	drawn_valid = false;
}

/* --- Text -------------------------------------------------------------- */

static void parse(C2D_Text *text, const char *string)
{
	C2D_TextParse(text, text_buf, string);
	C2D_TextOptimize(text);
}

/* A secondary button's label: `fmt` with `name`, cut with "..." to fit
 * (3.3); the whole name is on the top screen. */
static void parse_button(detail_button button, str_id fmt, const char *name)
{
	char label[192];
	snprintf(label, sizeof(label), tr(fmt), name);
	int width = detail_button_rect(button).w - 2 * SP_4;
	ui_parse_fitted(&button_text[button], text_buf, scratch_buf, label,
	                TXT_CUERPO, width);
}

static bool doc_changed(const detail_doc *doc)
{
	return doc != parsed_doc || !doc || doc->serial != parsed_serial ||
	       doc->state != parsed_state;
}

/* Parses what `doc` shows when it or the language changed. A different
 * document also starts the view afresh, focus on Play. */
static void sync(const detail_doc *doc)
{
	bool changed = doc_changed(doc);
	if (!changed && parsed_lang == lang_get())
		return;
	if (changed) {
		view_detail_enter();
		focus = DETAIL_BUTTON_PLAY;
	}
	C2D_TextBufClear(text_buf);
	for (int i = 0; i < GLYPH_COUNT; i++) {
		parse(&letter_text[i], glyph_letters[i]);
		parse(&hint_text[i], tr(glyph_labels[i]));
	}
	parse(&bubble_text, tr(STR_LANG_NAME));

	for (int i = 0; i < DETAIL_BUTTON_COUNT; i++)
		enabled[i] = false;
	if (doc && doc->kind == DETAIL_TRACK && doc->count > 0) {
		const sp_track *t = &doc->tracks[doc->selected];
		enabled[DETAIL_BUTTON_PLAY] = true;
		enabled[DETAIL_BUTTON_ARTIST] = t->artist_uri[0] != '\0';
		enabled[DETAIL_BUTTON_ALBUM] = t->album_uri[0] != '\0';
		parse(&button_text[DETAIL_BUTTON_PLAY], tr(STR_DETAIL_PLAY));
		parse_button(DETAIL_BUTTON_ARTIST, STR_DETAIL_ARTIST_FMT,
		             t->artist[0] ? t->artist : tr(STR_UNKNOWN_ARTIST));
		parse_button(DETAIL_BUTTON_ALBUM, STR_DETAIL_ALBUM_FMT,
		             t->album[0] ? t->album : tr(STR_UNKNOWN_ALBUM));
	}

	parsed_doc = doc;
	parsed_serial = doc ? doc->serial : 0;
	parsed_state = doc ? doc->state : DETAIL_LOADING;
	parsed_lang = lang_get();
}

static bool track_mode(const detail_doc *doc)
{
	return doc && doc->kind == DETAIL_TRACK && doc->count > 0;
}

/* --- State ------------------------------------------------------------- */

static detail_look current_look(void)
{
	detail_look look = { .focus = focus, .bubble = bubble_frames >= 0 };
	for (int i = 0; i < DETAIL_BUTTON_COUNT; i++)
		look.button[i] = press_down(&press_button[i]);
	for (int i = 0; i < GLYPH_COUNT; i++)
		look.glyph[i] = press_down(&press_glyph[i]);
	return look;
}

static bool same_look(const detail_look *a, const detail_look *b)
{
	if (a->focus != b->focus || a->bubble != b->bubble)
		return false;
	for (int i = 0; i < DETAIL_BUTTON_COUNT; i++)
		if (a->button[i] != b->button[i])
			return false;
	for (int i = 0; i < GLYPH_COUNT; i++)
		if (a->glyph[i] != b->glyph[i])
			return false;
	return true;
}

static void tick(void)
{
	for (int i = 0; i < DETAIL_BUTTON_COUNT; i++)
		press_tick(&press_button[i]);
	for (int i = 0; i < GLYPH_COUNT; i++)
		press_tick(&press_glyph[i]);
	if (bubble_frames >= 0 && ++bubble_frames >= BUBBLE_SHOW)
		bubble_frames = -1;
}

/* --- Input ------------------------------------------------------------- */

static detail_action button_action(detail_button button)
{
	switch (button) {
	case DETAIL_BUTTON_ARTIST:
		return DETAIL_ACTION_ARTIST;
	case DETAIL_BUTTON_ALBUM:
		return DETAIL_ACTION_ALBUM;
	default:
		return DETAIL_ACTION_PLAY;
	}
}

static void queue(detail_action action, const press_state *press)
{
	pending = action;
	pending_press = press;
}

static void release_keys(u32 up)
{
	if (up & KEY_A) {
		press_end(&press_glyph[GLYPH_A]);
		if (touch.target < 0)
			for (int i = 0; i < DETAIL_BUTTON_COUNT; i++)
				press_end(&press_button[i]);
	}
	if (up & KEY_B)
		press_end(&press_glyph[GLYPH_B]);
}

/* Returns true while the stylus owns the input. */
static bool handle_touch(u32 down, u32 held, u32 up)
{
	int hit = -1;
	if (held & KEY_TOUCH) {
		touchPosition pos;
		hidTouchRead(&pos);
		hit = detail_hit_test(pos.px, pos.py, enabled);
	}
	int control;
	touch_event event = touch_track_update(&touch, (down & KEY_TOUCH) != 0,
	                                       (held & KEY_TOUCH) != 0,
	                                       (up & KEY_TOUCH) != 0, hit,
	                                       &control);
	switch (event) {
	case TOUCH_PRESS:
		focus = (detail_button)control;
		press_begin(&press_button[control]);
		break;
	case TOUCH_CANCEL:
		press_cancel(&press_button[control]);
		break;
	case TOUCH_ACTIVATE:
		press_end(&press_button[control]);
		queue(button_action((detail_button)control), &press_button[control]);
		break;
	case TOUCH_NONE:
		break;
	}
	return touch.target >= 0;
}

detail_action view_detail_update(const detail_doc *doc, u32 down)
{
	u32 held = hidKeysHeld();
	u32 up = hidKeysUp();
	sync(doc);
	tick();
	release_keys(up);

	if (pending != DETAIL_ACTION_NONE) {
		if (!press_shown(pending_press))
			return DETAIL_ACTION_NONE;
		detail_action action = pending;
		pending = DETAIL_ACTION_NONE;
		pending_press = NULL;
		return action;
	}

	if (track_mode(doc)) {
		if (handle_touch(down, held, up) || pending != DETAIL_ACTION_NONE)
			return DETAIL_ACTION_NONE;
		/* 6.2: the focus moves, and shows, in the same frame. */
		if (down & (KEY_UP | KEY_DOWN))
			focus = detail_focus_move(focus, (down & KEY_UP) ? -1 : 1,
			                          enabled);
		if (down & KEY_A) {
			press_begin(&press_button[focus]);
			press_begin(&press_glyph[GLYPH_A]);
			queue(button_action(focus), &press_button[focus]);
			return DETAIL_ACTION_NONE;
		}
	}

	if (down & KEY_B) {
		press_begin(&press_glyph[GLYPH_B]);
		queue(DETAIL_ACTION_BACK, &press_glyph[GLYPH_B]);
	} else if (down & KEY_SELECT) {
		/* No language control on screen: a bubble with the result (6.5). */
		bubble_frames = 0;
		return DETAIL_ACTION_LANGUAGE;
	}
	return DETAIL_ACTION_NONE;
}

bool view_detail_dirty(const detail_doc *doc)
{
	if (doc_changed(doc) || parsed_lang != lang_get() || !drawn_valid)
		return true;
	detail_look look = current_look();
	return !same_look(&look, &drawn);
}

/* --- Drawing ----------------------------------------------------------- */

static void draw_track(const detail_look *look)
{
	C2D_Image play = icon_image(ICON_PLAY);
	for (int i = 0; i < DETAIL_BUTTON_COUNT; i++) {
		ui_control_look button = {
			.pressed = look->button[i],
			.focused = look->focus == (detail_button)i,
			.disabled = !enabled[i],
		};
		ui_button(detail_button_rect(i),
		          i == DETAIL_BUTTON_PLAY ? UI_BUTTON_PRIMARY
		                                  : UI_BUTTON_SECONDARY,
		          button, &button_text[i],
		          i == DETAIL_BUTTON_PLAY ? &play : NULL);
	}
}

void view_detail_draw_bottom(const detail_doc *doc)
{
	sync(doc);
	detail_look look = current_look();

	bool track = track_mode(doc);
	if (track)
		draw_track(&look);

	ui_hint hints[GLYPH_COUNT];
	int n = 0;
	if (track)
		hints[n++] = (ui_hint){ &letter_text[GLYPH_A], &hint_text[GLYPH_A],
		                        glyph_colors[GLYPH_A], look.glyph[GLYPH_A] };
	hints[n++] = (ui_hint){ &letter_text[GLYPH_B], &hint_text[GLYPH_B],
	                        glyph_colors[GLYPH_B], look.glyph[GLYPH_B] };
	ui_button_bar(detail_hints_rect(), hints, n);

	if (look.bubble) {
		C2D_Image globe = icon_image(ICON_LANGUAGE);
		ui_bubble(list_bubble_slot_rect(ui_bubble_content_width(&bubble_text),
		                                0),
		          &globe, &bubble_text);
	}

	drawn = look;
	drawn_valid = true;
}
