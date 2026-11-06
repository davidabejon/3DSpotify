#include "view_settings.h"

#include "draw.h"
#include "icons.h"
#include "lang.h"
#include "press_logic.h"
#include "settings_logic.h"
#include "theme.h"

#include <citro2d.h>

#include <stdio.h>

#define TEXT_GLYPHS 256

/* The button bar (STYLES.md 5.6). */
enum { GLYPH_A, GLYPH_B, GLYPH_COUNT };
static const char *const glyph_letters[GLYPH_COUNT] = { "A", "B" };
static const u32 glyph_colors[GLYPH_COUNT] = { COL_BTN_A, COL_BTN_B };
static const str_id glyph_labels[GLYPH_COUNT] = {
	STR_HINT_CHOOSE, STR_HINT_BACK
};

static C2D_TextBuf text_buf;
static C2D_Text brand_text;
static C2D_Text title_text;
static C2D_Text item_text[SETTINGS_ITEM_COUNT];
static C2D_Text letter_text[GLYPH_COUNT];
static C2D_Text hint_text[GLYPH_COUNT];
static lang_id parsed_lang = LANG_COUNT;

static settings_item focus = SETTINGS_ITEM_LANGUAGE;
static press_state press_item[SETTINGS_ITEM_COUNT];
static press_state press_glyph[GLYPH_COUNT];
static touch_track touch = { -1 };
static settings_action pending = SETTINGS_ACTION_NONE;
static const press_state *pending_press;

typedef struct {
	settings_item focus;
	bool item[SETTINGS_ITEM_COUNT];
	bool glyph[GLYPH_COUNT];
} settings_look;
static settings_look drawn;
static bool drawn_valid;

bool view_settings_init(void)
{
	text_buf = C2D_TextBufNew(TEXT_GLYPHS);
	return text_buf != NULL;
}

void view_settings_exit(void)
{
	if (text_buf)
		C2D_TextBufDelete(text_buf);
	text_buf = NULL;
}

void view_settings_enter(void)
{
	for (int i = 0; i < SETTINGS_ITEM_COUNT; i++)
		press_reset(&press_item[i]);
	for (int i = 0; i < GLYPH_COUNT; i++)
		press_reset(&press_glyph[i]);
	touch.target = -1;
	pending = SETTINGS_ACTION_NONE;
	pending_press = NULL;
	drawn_valid = false;
}

/* --- Text ------------------------------------------------------------- */

static void parse(C2D_Text *text, const char *string)
{
	C2D_TextParse(text, text_buf, string);
	C2D_TextOptimize(text);
}

static void ensure_parsed(void)
{
	if (parsed_lang == lang_get())
		return;
	C2D_TextBufClear(text_buf);
	parse(&brand_text, tr(STR_APP_NAME));
	parse(&title_text, tr(STR_SETTINGS_TITLE));
	char language[64];
	snprintf(language, sizeof(language), tr(STR_SETTINGS_LANGUAGE_FMT),
	         tr(STR_LANG_NAME));
	parse(&item_text[SETTINGS_ITEM_LANGUAGE], language);
	parse(&item_text[SETTINGS_ITEM_RECONNECT], tr(STR_SETTINGS_RECONNECT));
	parse(&item_text[SETTINGS_ITEM_PAIR], tr(STR_SETTINGS_PAIR));
	for (int i = 0; i < GLYPH_COUNT; i++) {
		parse(&letter_text[i], glyph_letters[i]);
		parse(&hint_text[i], tr(glyph_labels[i]));
	}
	parsed_lang = lang_get();
}

/* --- State ------------------------------------------------------------ */

static settings_look current_look(void)
{
	settings_look look = { .focus = focus };
	for (int i = 0; i < SETTINGS_ITEM_COUNT; i++)
		look.item[i] = press_down(&press_item[i]);
	for (int i = 0; i < GLYPH_COUNT; i++)
		look.glyph[i] = press_down(&press_glyph[i]);
	return look;
}

static bool same_look(const settings_look *a, const settings_look *b)
{
	if (a->focus != b->focus)
		return false;
	for (int i = 0; i < SETTINGS_ITEM_COUNT; i++)
		if (a->item[i] != b->item[i])
			return false;
	for (int i = 0; i < GLYPH_COUNT; i++)
		if (a->glyph[i] != b->glyph[i])
			return false;
	return true;
}

static void tick(void)
{
	for (int i = 0; i < SETTINGS_ITEM_COUNT; i++)
		press_tick(&press_item[i]);
	for (int i = 0; i < GLYPH_COUNT; i++)
		press_tick(&press_glyph[i]);
}

/* --- Input ------------------------------------------------------------ */

static settings_action item_action(settings_item item)
{
	switch (item) {
	case SETTINGS_ITEM_RECONNECT:
		return SETTINGS_ACTION_RECONNECT;
	case SETTINGS_ITEM_PAIR:
		return SETTINGS_ACTION_PAIR;
	default:
		return SETTINGS_ACTION_LANGUAGE;
	}
}

static void queue(settings_action action, const press_state *press)
{
	pending = action;
	pending_press = press;
}

static void release_keys(u32 up)
{
	if (up & KEY_A) {
		press_end(&press_glyph[GLYPH_A]);
		if (touch.target < 0)
			for (int i = 0; i < SETTINGS_ITEM_COUNT; i++)
				press_end(&press_item[i]);
	}
	if (up & KEY_B)
		press_end(&press_glyph[GLYPH_B]);
	if (up & KEY_SELECT)
		press_end(&press_item[SETTINGS_ITEM_LANGUAGE]);
}

/* Returns true while the stylus owns the input. */
static bool handle_touch(u32 down, u32 held, u32 up)
{
	int hit = -1;
	if (held & KEY_TOUCH) {
		touchPosition pos;
		hidTouchRead(&pos);
		hit = settings_hit_test(pos.px, pos.py);
	}
	int control;
	touch_event event = touch_track_update(&touch, (down & KEY_TOUCH) != 0,
	                                       (held & KEY_TOUCH) != 0,
	                                       (up & KEY_TOUCH) != 0, hit,
	                                       &control);
	switch (event) {
	case TOUCH_PRESS:
		focus = (settings_item)control;
		press_begin(&press_item[control]);
		break;
	case TOUCH_CANCEL:
		press_cancel(&press_item[control]);
		break;
	case TOUCH_ACTIVATE:
		press_end(&press_item[control]);
		queue(item_action((settings_item)control), &press_item[control]);
		break;
	case TOUCH_NONE:
		break;
	}
	return touch.target >= 0;
}

settings_action view_settings_update(u32 down)
{
	u32 held = hidKeysHeld();
	u32 up = hidKeysUp();
	ensure_parsed();
	tick();
	release_keys(up);

	if (pending != SETTINGS_ACTION_NONE) {
		/* The language switches at once; leaving waits for the press. */
		if (pending != SETTINGS_ACTION_LANGUAGE &&
		    !press_shown(pending_press))
			return SETTINGS_ACTION_NONE;
		settings_action action = pending;
		pending = SETTINGS_ACTION_NONE;
		pending_press = NULL;
		return action;
	}

	if (handle_touch(down, held, up) || pending != SETTINGS_ACTION_NONE)
		return SETTINGS_ACTION_NONE;

	/* 6.2: the focus moves, and shows, in the same frame. */
	if (down & (KEY_UP | KEY_DOWN))
		focus = settings_focus_move(focus, (down & KEY_UP) ? -1 : 1);
	if (down & KEY_A) {
		press_begin(&press_item[focus]);
		press_begin(&press_glyph[GLYPH_A]);
		queue(item_action(focus), &press_item[focus]);
	} else if (down & KEY_B) {
		press_begin(&press_glyph[GLYPH_B]);
		queue(SETTINGS_ACTION_BACK, &press_glyph[GLYPH_B]);
	} else if (down & KEY_SELECT) {
		/* 6.5: the control on screen shows the press. */
		press_begin(&press_item[SETTINGS_ITEM_LANGUAGE]);
		return SETTINGS_ACTION_LANGUAGE;
	}
	return SETTINGS_ACTION_NONE;
}

bool view_settings_dirty(void)
{
	if (parsed_lang != lang_get() || !drawn_valid)
		return true;
	settings_look look = current_look();
	return !same_look(&look, &drawn);
}

/* --- Drawing ---------------------------------------------------------- */

void view_settings_draw_top(void)
{
	ensure_parsed();
	int title_line = ui_line_height(TXT_CABEZA);
	ui_text(&title_text, SP_3, SP_3 + title_line / 2, TXT_CABEZA,
	        COL_TEXTO);
	/* Otherwise a single message: centred (3.3). */
	ui_text_centered(&brand_text, SCREEN_TOP_W / 2, SCREEN_H / 2,
	                 TXT_TITULO, COL_TEXTO);
}

void view_settings_draw_bottom(void)
{
	ensure_parsed();
	settings_look look = current_look();

	C2D_Image globe = icon_image(ICON_LANGUAGE);
	for (int i = 0; i < SETTINGS_ITEM_COUNT; i++) {
		ui_control_look button = {
			.pressed = look.item[i],
			.focused = focus == (settings_item)i,
		};
		ui_button(settings_item_rect(i), UI_BUTTON_SECONDARY, button,
		          &item_text[i],
		          i == SETTINGS_ITEM_LANGUAGE ? &globe : NULL);
	}

	ui_hint hints[GLYPH_COUNT];
	for (int i = 0; i < GLYPH_COUNT; i++)
		hints[i] = (ui_hint){ &letter_text[i], &hint_text[i],
		                      glyph_colors[i], look.glyph[i] };
	ui_button_bar(settings_hints_rect(), hints, GLYPH_COUNT);

	drawn = look;
	drawn_valid = true;
}
