#include "view_home.h"

#include "draw.h"
#include "home_logic.h"
#include "icons.h"
#include "lang.h"
#include "press_logic.h"
#include "text_fit.h"
#include "theme.h"

#include <citro2d.h>

#include <stdio.h>
#include <string.h>

#define QUERY_SIZE 96
/* Labels, the button bar and a repeat button of up to QUERY_SIZE glyphs. */
#define TEXT_GLYPHS 512
#define SCRATCH_GLYPHS (QUERY_SIZE + 8)

/* The button bar (STYLES.md 5.6): the three actions of this view. */
enum { GLYPH_A, GLYPH_X, GLYPH_Y, GLYPH_COUNT };
static const char *const glyph_letters[GLYPH_COUNT] = { "A", "X", "Y" };
static const u32 glyph_colors[GLYPH_COUNT] = {
	COL_BTN_A, COL_BTN_X, COL_BTN_Y
};
static const str_id glyph_labels[GLYPH_COUNT] = {
	STR_HINT_SEARCH, STR_HINT_RECONNECT, STR_HINT_PAIR
};

static C2D_TextBuf text_buf;
/* Only for measuring candidates while fitting the last query. */
static C2D_TextBuf scratch_buf;

static C2D_Text brand_text;
static C2D_Text title_text;
static C2D_Text search_text;
static C2D_Text last_label_text;
static C2D_Text repeat_text;
static C2D_Text bubble_text;
static C2D_Text letter_text[GLYPH_COUNT];
static C2D_Text hint_text[GLYPH_COUNT];
static float search_content_width;
static float repeat_text_width;

static char last_query[QUERY_SIZE];
/* `stale`: the parsed text no longer matches the query or the language. */
static bool stale = true;
static lang_id parsed_lang = LANG_COUNT;

/* Interaction state (STYLES.md 6). Every change shows in one frame. */
static home_item focus = HOME_ITEM_SEARCH;
static press_state press_search;
static press_state press_repeat;
static press_state press_glyph[GLYPH_COUNT];
static touch_track touch = { -1 };
static int bubble_frames = -1; /* -1: no bubble */
/* An action waiting for its pressed state to be seen. */
static home_action pending = HOME_ACTION_NONE;
static const press_state *pending_press;

/* What the screen shows besides the text; a frame is needed when it
 * differs from what was last drawn. */
typedef struct {
	home_item focus;
	bool search;
	bool repeat;
	bool glyph[GLYPH_COUNT];
	bool bubble;
} home_look;
static home_look drawn;
static bool drawn_valid;

bool view_home_init(void)
{
	text_buf = C2D_TextBufNew(TEXT_GLYPHS);
	scratch_buf = C2D_TextBufNew(SCRATCH_GLYPHS);
	if (!text_buf || !scratch_buf) {
		view_home_exit();
		return false;
	}
	return true;
}

void view_home_exit(void)
{
	if (text_buf)
		C2D_TextBufDelete(text_buf);
	if (scratch_buf)
		C2D_TextBufDelete(scratch_buf);
	text_buf = scratch_buf = NULL;
}

static bool has_query(void)
{
	return last_query[0] != '\0';
}

void view_home_enter(const char *query)
{
	snprintf(last_query, sizeof(last_query), "%s", query ? query : "");
	if (!has_query())
		focus = HOME_ITEM_SEARCH;
	press_reset(&press_search);
	press_reset(&press_repeat);
	for (int i = 0; i < GLYPH_COUNT; i++)
		press_reset(&press_glyph[i]);
	touch.target = -1;
	bubble_frames = -1;
	pending = HOME_ACTION_NONE;
	pending_press = NULL;
	stale = true;
	drawn_valid = false;
}

/* --- Text ------------------------------------------------------------- */

static void parse(C2D_Text *text, const char *string)
{
	C2D_TextParse(text, text_buf, string);
	C2D_TextOptimize(text);
}

static float measure_body(const char *string, void *ctx)
{
	(void)ctx;
	C2D_Text text;
	C2D_TextBufClear(scratch_buf);
	C2D_TextParse(&text, scratch_buf, string);
	return ui_text_width(&text, TXT_CUERPO);
}

static void ensure_parsed(void)
{
	if (!stale && parsed_lang == lang_get())
		return;
	C2D_TextBufClear(text_buf);
	parse(&brand_text, tr(STR_APP_NAME));
	parse(&title_text, tr(STR_HOME_TITLE));
	parse(&search_text, tr(STR_HOME_SEARCH));
	parse(&last_label_text, tr(STR_HOME_LAST_SEARCH));
	parse(&bubble_text, tr(STR_LANG_NAME));
	for (int i = 0; i < GLYPH_COUNT; i++) {
		parse(&letter_text[i], glyph_letters[i]);
		parse(&hint_text[i], tr(glyph_labels[i]));
	}
	C2D_Image icon = icon_image(ICON_SEARCH);
	search_content_width = ui_button_content_width(&search_text,
	                                               icon.tex != NULL);
	repeat_text_width = 0;
	if (has_query()) {
		/* Room for the query plus "...". */
		char fitted[QUERY_SIZE + 4];
		text_fit(last_query, (float)home_repeat_max_text_width(),
		         measure_body, NULL, fitted, sizeof(fitted));
		parse(&repeat_text, fitted);
		repeat_text_width = ui_text_width(&repeat_text, TXT_CUERPO);
	}
	parsed_lang = lang_get();
	stale = false;
}

/* --- State ------------------------------------------------------------ */

static home_look current_look(void)
{
	home_look look = {
		.focus = focus,
		.search = press_down(&press_search),
		.repeat = press_down(&press_repeat),
		.bubble = bubble_frames >= 0,
	};
	for (int i = 0; i < GLYPH_COUNT; i++)
		look.glyph[i] = press_down(&press_glyph[i]);
	return look;
}

static bool same_look(const home_look *a, const home_look *b)
{
	if (a->focus != b->focus || a->search != b->search ||
	    a->repeat != b->repeat || a->bubble != b->bubble)
		return false;
	for (int i = 0; i < GLYPH_COUNT; i++)
		if (a->glyph[i] != b->glyph[i])
			return false;
	return true;
}

static void tick(void)
{
	press_tick(&press_search);
	press_tick(&press_repeat);
	for (int i = 0; i < GLYPH_COUNT; i++)
		press_tick(&press_glyph[i]);
	/* 5.9: shown BUBBLE_SHOW frames, gone without a transition. */
	if (bubble_frames >= 0 && ++bubble_frames >= BUBBLE_SHOW)
		bubble_frames = -1;
}

/* --- Input ------------------------------------------------------------ */

static press_state *item_press(home_item item)
{
	return item == HOME_ITEM_REPEAT ? &press_repeat : &press_search;
}

static home_action item_action(home_item item)
{
	return item == HOME_ITEM_REPEAT && has_query()
	       ? HOME_ACTION_REPEAT : HOME_ACTION_SEARCH;
}

static void queue(home_action action, const press_state *press)
{
	pending = action;
	pending_press = press;
}

static void release_keys(u32 up)
{
	if (up & KEY_A) {
		press_end(&press_glyph[GLYPH_A]);
		/* Not a stylus press in progress on the same control. */
		if (touch.target < 0) {
			press_end(&press_search);
			press_end(&press_repeat);
		}
	}
	if (up & KEY_X)
		press_end(&press_glyph[GLYPH_X]);
	if (up & KEY_Y)
		press_end(&press_glyph[GLYPH_Y]);
}

/* Returns true while the stylus owns the input. */
static bool handle_touch(u32 down, u32 held, u32 up)
{
	int hit = -1;
	if (held & KEY_TOUCH) {
		touchPosition pos;
		hidTouchRead(&pos);
		hit = home_hit_test(pos.px, pos.py, search_content_width,
		                    has_query(), repeat_text_width);
	}
	int control;
	touch_event event = touch_track_update(&touch, (down & KEY_TOUCH) != 0,
	                                       (held & KEY_TOUCH) != 0,
	                                       (up & KEY_TOUCH) != 0, hit,
	                                       &control);
	home_item item = (home_item)control;
	switch (event) {
	case TOUCH_PRESS:
		focus = item;
		press_begin(item_press(item));
		break;
	case TOUCH_CANCEL:
		press_cancel(item_press(item));
		break;
	case TOUCH_ACTIVATE:
		press_end(item_press(item));
		queue(item_action(item), item_press(item));
		break;
	case TOUCH_NONE:
		break;
	}
	return touch.target >= 0;
}

home_action view_home_update(u32 down)
{
	u32 held = hidKeysHeld();
	u32 up = hidKeysUp();
	ensure_parsed();
	tick();
	release_keys(up);

	if (pending != HOME_ACTION_NONE) {
		if (!press_shown(pending_press))
			return HOME_ACTION_NONE;
		home_action action = pending;
		pending = HOME_ACTION_NONE;
		pending_press = NULL;
		return action;
	}

	if (handle_touch(down, held, up) || pending != HOME_ACTION_NONE)
		return HOME_ACTION_NONE;

	/* 6.2: the focus moves, and shows, in the same frame. */
	if (down & (KEY_UP | KEY_DOWN))
		focus = home_focus_move(focus, (down & KEY_UP) ? -1 : 1,
		                        has_query());
	if (down & KEY_A) {
		home_item item = has_query() ? focus : HOME_ITEM_SEARCH;
		press_begin(item_press(item));
		press_begin(&press_glyph[GLYPH_A]);
		queue(item_action(item), item_press(item));
	} else if (down & KEY_X) {
		press_begin(&press_glyph[GLYPH_X]);
		queue(HOME_ACTION_RECONNECT, &press_glyph[GLYPH_X]);
	} else if (down & KEY_Y) {
		press_begin(&press_glyph[GLYPH_Y]);
		queue(HOME_ACTION_PAIR, &press_glyph[GLYPH_Y]);
	} else if (down & KEY_SELECT) {
		/* No control on screen: a bubble with the result (6.5). */
		bubble_frames = 0;
		return HOME_ACTION_LANGUAGE;
	}
	return HOME_ACTION_NONE;
}

bool view_home_dirty(void)
{
	if (stale || parsed_lang != lang_get() || !drawn_valid)
		return true;
	home_look look = current_look();
	return !same_look(&look, &drawn);
}

/* --- Drawing ---------------------------------------------------------- */

void view_home_draw_top(void)
{
	ensure_parsed();
	/* A single message: centred (3.3). */
	ui_text_centered(&brand_text, SCREEN_TOP_W / 2, SCREEN_H / 2,
	                 TXT_TITULO, COL_TEXTO);
}

static void draw_button_bar(const home_look *look)
{
	ui_hint hints[GLYPH_COUNT];
	for (int i = 0; i < GLYPH_COUNT; i++) {
		hints[i] = (ui_hint){ &letter_text[i], &hint_text[i],
		                      glyph_colors[i], look->glyph[i] };
	}
	ui_button_bar(home_hints_rect(), hints, GLYPH_COUNT);
}

void view_home_draw_bottom(void)
{
	ensure_parsed();
	home_look look = current_look();

	ui_rect header = home_header_rect();
	ui_panel(header);
	ui_text(&title_text, SP_2, header.y + header.h / 2, TXT_CABEZA,
	        COL_TEXTO);

	C2D_Image icon = icon_image(ICON_SEARCH);
	ui_control_look search = {
		.pressed = look.search,
		.focused = focus == HOME_ITEM_SEARCH,
	};
	ui_button(home_search_rect(search_content_width), UI_BUTTON_PRIMARY,
	          search, &search_text, &icon);

	if (has_query()) {
		ui_rect label = home_label_rect();
		ui_text(&last_label_text, label.x, label.y + label.h / 2, TXT_META,
		        COL_TEXTO_2);
		ui_control_look repeat = {
			.pressed = look.repeat,
			.focused = focus == HOME_ITEM_REPEAT,
		};
		ui_button(home_repeat_rect(repeat_text_width), UI_BUTTON_SECONDARY,
		          repeat, &repeat_text, NULL);
	}

	draw_button_bar(&look);

	if (look.bubble) {
		C2D_Image globe = icon_image(ICON_LANGUAGE);
		ui_bubble(home_bubble_rect(ui_bubble_content_width(&bubble_text)),
		          &globe, &bubble_text);
	}

	drawn = look;
	drawn_valid = true;
}
