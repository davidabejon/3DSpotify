#include "view_results.h"

#include "draw.h"
#include "icons.h"
#include "list_logic.h"
#include "press_logic.h"
#include "tabbar.h"
#include "theme.h"
#include "track_list.h"

#include <citro2d.h>

#include <stdio.h>
#include <string.h>

#define STATIC_GLYPHS 1024
#define SCRATCH_GLYPHS UI_FIELD_SIZE

/* Control id for touch tracking: the rows are the list's own. */
#define CONTROL_BUTTON 1000

typedef enum {
	MODE_IDLE,
	MODE_SEARCHING,
	MODE_LIST,
	MODE_EMPTY,
	MODE_ERROR
} results_mode;

/* The buttons this view uses (2.5). */
enum { GLYPH_A, GLYPH_B, GLYPH_X, GLYPH_Y, GLYPH_COUNT };
static const char *const glyph_letters[GLYPH_COUNT] = { "A", "B", "X", "Y" };
static const u32 glyph_colors[GLYPH_COUNT] = {
	COL_BTN_A, COL_BTN_B, COL_BTN_X, COL_BTN_Y
};

/* Content. The rows, their focus, scroll and input are the list's. */
static results_mode mode = MODE_IDLE;
static track_list list;
static str_id advice = STR_SEARCH_CHECK_WIFI;
static bool cancelling;
/* Nothing searched yet: the recent searches, and the focus among the
 * search button (0) and them (1 + their index). */
static recent_list recent;
static int idle_focus;
static int recent_choice;

/* Parsed text. `stale`: content or language changed. */
static C2D_TextBuf static_buf, scratch_buf;
static bool stale = true;
static lang_id parsed_lang = LANG_COUNT;

static C2D_Text status_text;        /* the waiting bubble */
static C2D_Text empty_text;
static C2D_Text button_text;        /* empty: new search; error: retry */
static C2D_Text notice_title_text;
static C2D_Text notice_body_text;
static C2D_Text bubble_text;
static C2D_Text recent_label_text;
static C2D_Text recent_text[RECENT_MAX];
static C2D_Text letter_text[GLYPH_COUNT];
static C2D_Text hint_play, hint_detail, hint_back, hint_search, hint_cancel,
                hint_retry;

/* Interaction (STYLES.md 6). `touch` is the button's, outside the list. */
static press_state press_button;
static press_state press_recent[RECENT_MAX];
static press_state press_glyph[GLYPH_COUNT];
static touch_track touch = { -1 };
static int bubble_frames = -1;
static int loader_frames;
static results_action pending = RESULTS_ACTION_NONE;
static const press_state *pending_press;

typedef struct {
	results_mode mode;
	track_list_look list;
	bool button;
	int idle_focus;
	bool recent[RECENT_MAX];
	bool glyph[GLYPH_COUNT];
	bool bubble;
	int loader_step;
	bool cancelling;
} results_look;
static results_look drawn;
static bool drawn_valid;

/* --- Lifetime ---------------------------------------------------------- */

bool view_results_init(void)
{
	static_buf = C2D_TextBufNew(STATIC_GLYPHS);
	scratch_buf = C2D_TextBufNew(SCRATCH_GLYPHS);
	if (!static_buf || !scratch_buf ||
	    !track_list_init(&list, TRACK_LIST_THUMBS)) {
		view_results_exit();
		return false;
	}
	return true;
}

void view_results_exit(void)
{
	C2D_TextBuf *bufs[] = { &static_buf, &scratch_buf };
	for (size_t i = 0; i < sizeof(bufs) / sizeof(bufs[0]); i++) {
		if (*bufs[i])
			C2D_TextBufDelete(*bufs[i]);
		*bufs[i] = NULL;
	}
	track_list_exit(&list);
}

static void reset_interaction(void)
{
	track_list_reset_input(&list);
	press_reset(&press_button);
	for (int i = 0; i < RECENT_MAX; i++)
		press_reset(&press_recent[i]);
	for (int i = 0; i < GLYPH_COUNT; i++)
		press_reset(&press_glyph[i]);
	touch.target = -1;
	bubble_frames = -1;
	loader_frames = 0;
	pending = RESULTS_ACTION_NONE;
	pending_press = NULL;
	stale = true;
	drawn_valid = false;
}

/* For every list with thumbnails, the details' too: the results are where
 * the caller has always asked. */
void view_results_allow_thumbs(bool allow)
{
	track_list_allow_thumbs(allow);
}

void view_results_idle(void)
{
	mode = MODE_IDLE;
	track_list_set(&list, NULL, 0, 0);
	cancelling = false;
	idle_focus = 0;
	reset_interaction();
}

void view_results_set_recent(const recent_list *r)
{
	recent = *r;
	if (recent.count < 0 || recent.count > RECENT_MAX)
		recent.count = 0;
	if (idle_focus > recent.count)
		idle_focus = recent.count;
	stale = true;
}

const char *view_results_recent_query(void)
{
	return recent_choice >= 0 && recent_choice < recent.count
	       ? recent.items[recent_choice] : "";
}

void view_results_enter(void)
{
	reset_interaction();
}

void view_results_searching(void)
{
	mode = MODE_SEARCHING;
	track_list_set(&list, NULL, 0, 0);
	cancelling = false;
	reset_interaction();
}

void view_results_cancelling(void)
{
	if (!cancelling) {
		cancelling = true;
		stale = true;
	}
}

void view_results_show(const sp_track *tracks, int n, int focus)
{
	track_list_set(&list, tracks, n, focus);
	mode = list.count > 0 ? MODE_LIST : MODE_EMPTY;
	reset_interaction();
	/* Covers now, even before the tab is on screen, unless a list on
	 * screen has them; on screen, the list claims them anyway. */
	track_list_offer_thumbs(&list);
}

void view_results_error(str_id help)
{
	mode = MODE_ERROR;
	track_list_set(&list, NULL, 0, 0);
	advice = help;
	reset_interaction();
}

int view_results_selected(void)
{
	return list.selected;
}

/* --- Text -------------------------------------------------------------- */

static void parse(C2D_Text *text, C2D_TextBuf buf, const char *string)
{
	C2D_TextParse(text, buf, string);
	C2D_TextOptimize(text);
}

/* Parses `string` cut with "..." to `width` px at `scale` (3.3). */
static void parse_fitted(C2D_Text *text, C2D_TextBuf buf, const char *string,
                         float scale, int width)
{
	ui_parse_fitted(text, buf, scratch_buf, string, scale, width);
}

static void parse_static(void)
{
	C2D_TextBufClear(static_buf);
	parse(&status_text, static_buf,
	      tr(cancelling ? STR_SEARCH_CANCELLING : STR_RESULTS_SEARCHING));
	parse(&empty_text, static_buf,
	      tr(mode == MODE_IDLE ? STR_RESULTS_IDLE : STR_RESULTS_EMPTY));
	parse(&button_text, static_buf,
	      tr(mode == MODE_ERROR ? STR_RESULTS_RETRY
	         : mode == MODE_IDLE ? STR_HOME_SEARCH
	                             : STR_RESULTS_NEW_SEARCH));
	ui_rect notice = list_notice_rect();
	parse_fitted(&notice_title_text, static_buf, tr(STR_SEARCH_FAILED_TITLE),
	             TXT_CABEZA, notice.w - 2 * SP_3 - ICON_S - SP_1);
	parse(&notice_body_text, static_buf, tr(advice));
	parse(&bubble_text, static_buf, tr(STR_LANG_NAME));
	parse(&recent_label_text, static_buf, tr(STR_RESULTS_RECENT));
	for (int i = 0; i < recent.count; i++) {
		ui_rect row = list_recent_rect(i);
		parse_fitted(&recent_text[i], static_buf, recent.items[i],
		             TXT_CUERPO, row.w - 3 * SP_2 - ICON_S);
	}
	for (int i = 0; i < GLYPH_COUNT; i++)
		parse(&letter_text[i], static_buf, glyph_letters[i]);
	parse(&hint_play, static_buf, tr(STR_HINT_PLAY));
	parse(&hint_detail, static_buf, tr(STR_HINT_DETAIL));
	parse(&hint_back, static_buf, tr(STR_HINT_BACK));
	parse(&hint_search, static_buf, tr(STR_HINT_SEARCH));
	parse(&hint_cancel, static_buf, tr(STR_HINT_CANCEL));
	parse(&hint_retry, static_buf, tr(STR_HINT_RETRY));
}

static void ensure_parsed(void)
{
	if (stale || parsed_lang != lang_get()) {
		parse_static();
		parsed_lang = lang_get();
		stale = false;
	}
	track_list_ensure_parsed(&list);
}

/* --- State ------------------------------------------------------------- */

static results_look current_look(void)
{
	results_look look = {
		.mode = mode,
		.list = track_list_get_look(&list),
		.button = press_down(&press_button),
		.idle_focus = idle_focus,
		.bubble = bubble_frames >= 0,
		.loader_step = mode == MODE_SEARCHING
		               ? loader_frames / LOADER_STEP : 0,
		.cancelling = cancelling,
	};
	for (int i = 0; i < RECENT_MAX; i++)
		look.recent[i] = press_down(&press_recent[i]);
	for (int i = 0; i < GLYPH_COUNT; i++)
		look.glyph[i] = press_down(&press_glyph[i]);
	return look;
}

static bool same_look(const results_look *a, const results_look *b)
{
	if (a->mode != b->mode || !track_list_same_look(&a->list, &b->list) ||
	    a->button != b->button || a->idle_focus != b->idle_focus ||
	    a->bubble != b->bubble || a->loader_step != b->loader_step ||
	    a->cancelling != b->cancelling)
		return false;
	for (int i = 0; i < RECENT_MAX; i++)
		if (a->recent[i] != b->recent[i])
			return false;
	for (int i = 0; i < GLYPH_COUNT; i++)
		if (a->glyph[i] != b->glyph[i])
			return false;
	return true;
}

static void tick(void)
{
	track_list_tick(&list);
	press_tick(&press_button);
	for (int i = 0; i < RECENT_MAX; i++)
		press_tick(&press_recent[i]);
	for (int i = 0; i < GLYPH_COUNT; i++)
		press_tick(&press_glyph[i]);
	if (bubble_frames >= 0 && ++bubble_frames >= BUBBLE_SHOW)
		bubble_frames = -1;
	if (mode == MODE_SEARCHING)
		loader_frames++;
}

/* --- Input ------------------------------------------------------------- */

static void queue(results_action action, const press_state *press)
{
	pending = action;
	pending_press = press;
}

static bool has_button(void)
{
	return mode == MODE_IDLE || mode == MODE_EMPTY || mode == MODE_ERROR;
}

static results_action button_action(void)
{
	return mode == MODE_ERROR ? RESULTS_ACTION_RETRY
	                          : RESULTS_ACTION_NEW_SEARCH;
}

/* Nothing searched yet, with searches to suggest. */
static bool has_recent(void)
{
	return mode == MODE_IDLE && recent.count > 0;
}

static ui_rect button_rect(void)
{
	float width = ui_text_width(&button_text, TXT_CUERPO);
	if (has_recent())
		return list_idle_button_rect(width);
	return list_action_rect(width, mode == MODE_EMPTY || mode == MODE_IDLE);
}

/* What B does: leave the results for the screen before them, or, from
 * that screen, leave the tab. */
static results_action back_action(void)
{
	return mode == MODE_IDLE ? RESULTS_ACTION_BACK : RESULTS_ACTION_CLOSE;
}

static void release_keys(u32 up)
{
	if (up & KEY_A) {
		press_end(&press_glyph[GLYPH_A]);
		track_list_release(&list);
		if (touch.target < 0) {
			press_end(&press_button);
			for (int i = 0; i < RECENT_MAX; i++)
				press_end(&press_recent[i]);
		}
	}
	if (up & KEY_B)
		press_end(&press_glyph[GLYPH_B]);
	if (up & KEY_X)
		press_end(&press_glyph[GLYPH_X]);
	if (up & KEY_Y)
		press_end(&press_glyph[GLYPH_Y]);
}

/* Returns true while the stylus owns the input: the list's rows, or the
 * button of the other modes. */
static bool handle_touch(u32 down, u32 held, u32 up)
{
	if (mode == MODE_LIST) {
		track_list_touch event = track_list_update_touch(&list, down, held,
		                                                 up);
		if (event == TRACK_LIST_ACTIVATE)
			queue(RESULTS_ACTION_OPEN, &list.press);  /* the focused row */
		return event != TRACK_LIST_IDLE;
	}

	int hit = -1;
	if (held & KEY_TOUCH) {
		touchPosition pos;
		hidTouchRead(&pos);
		if (has_button() && ui_rect_contains(button_rect(), pos.px, pos.py))
			hit = CONTROL_BUTTON;
		for (int i = 0; has_recent() && i < recent.count; i++)
			if (ui_rect_contains(list_recent_rect(i), pos.px, pos.py))
				hit = i;
	}
	int control;
	touch_event event = touch_track_update(&touch, (down & KEY_TOUCH) != 0,
	                                       (held & KEY_TOUCH) != 0,
	                                       (up & KEY_TOUCH) != 0, hit,
	                                       &control);
	press_state *press = control >= 0 && control < RECENT_MAX
	                     ? &press_recent[control] : &press_button;
	switch (event) {
	case TOUCH_PRESS:
		press_begin(press);
		break;
	case TOUCH_CANCEL:
		press_cancel(press);
		break;
	case TOUCH_ACTIVATE:
		press_end(press);
		if (press == &press_button) {
			idle_focus = 0;
			queue(button_action(), press);
		} else {
			idle_focus = control + 1;
			recent_choice = control;
			queue(RESULTS_ACTION_RECENT, press);
		}
		break;
	case TOUCH_NONE:
		break;
	}
	return touch.target >= 0;
}

/* The A, B, X and Y glyphs plus what each does in this mode. */
static void press_key(int glyph, results_action action,
                      press_state *control)
{
	press_begin(&press_glyph[glyph]);
	if (control)
		press_begin(control);
	queue(action, control ? control : &press_glyph[glyph]);
}

results_action view_results_update(u32 down)
{
	u32 held = hidKeysHeld();
	u32 up = hidKeysUp();
	ensure_parsed();
	tick();
	release_keys(up);
	/* Uploads happen here, on the main thread, never while drawing. */
	track_list_thumbs_frame(&list);

	if (pending != RESULTS_ACTION_NONE) {
		if (!press_shown(pending_press))
			return RESULTS_ACTION_NONE;
		results_action action = pending;
		pending = RESULTS_ACTION_NONE;
		pending_press = NULL;
		return action;
	}

	if (mode == MODE_SEARCHING) {
		/* Cancelling is the caller's; the glyph still answers. */
		if (down & KEY_B)
			press_begin(&press_glyph[GLYPH_B]);
		return RESULTS_ACTION_NONE;
	}

	if (handle_touch(down, held, up) || pending != RESULTS_ACTION_NONE)
		return RESULTS_ACTION_NONE;

	if (mode == MODE_LIST &&
	    track_list_realign(&list, down, KEY_A | KEY_X)) {
		/* 6.6: with the focus dragged out of view, the first button press
		 * only brings the list back to it, on whole rows. A and X still
		 * show their glyph pressed, but act on nothing they cannot see. */
		if (down & KEY_A)
			press_begin(&press_glyph[GLYPH_A]);
		if (down & KEY_X)
			press_begin(&press_glyph[GLYPH_X]);
		return RESULTS_ACTION_NONE;
	}

	if (mode == MODE_LIST)
		track_list_navigate(&list, down);
	if (has_recent()) {
		if ((down & KEY_UP) && idle_focus > 0)
			idle_focus--;
		else if ((down & KEY_DOWN) && idle_focus < recent.count)
			idle_focus++;
	}

	if (down & KEY_A) {
		if (mode == MODE_LIST) {
			press_key(GLYPH_A, RESULTS_ACTION_OPEN,
			          track_list_focused_press(&list));
		} else if (has_recent() && idle_focus > 0) {
			recent_choice = idle_focus - 1;
			press_key(GLYPH_A, RESULTS_ACTION_RECENT,
			          &press_recent[recent_choice]);
		} else {
			press_key(GLYPH_A, button_action(), &press_button);
		}
	} else if ((down & KEY_X) && mode == MODE_LIST) {
		press_key(GLYPH_X, RESULTS_ACTION_DETAIL, NULL);
	} else if (down & KEY_B) {
		press_key(GLYPH_B, back_action(), NULL);
	} else if (down & KEY_Y) {
		press_key(GLYPH_Y, RESULTS_ACTION_NEW_SEARCH, NULL);
	} else if (down & KEY_SELECT) {
		/* No control on screen: a bubble with the result (6.5). */
		bubble_frames = 0;
		return RESULTS_ACTION_LANGUAGE;
	}
	return RESULTS_ACTION_NONE;
}

bool view_results_dirty(void)
{
	if (stale || parsed_lang != lang_get() || !drawn_valid ||
	    track_list_text_stale(&list))
		return true;
	results_look look = current_look();
	return !same_look(&look, &drawn);
}

/* --- Drawing ----------------------------------------------------------- */

static void draw_bar(const results_look *look)
{
	ui_hint hints[4];
	int n = 0;
	const C2D_Text *a_label = NULL;
	switch (mode) {
	case MODE_SEARCHING:
		break;
	case MODE_IDLE:
		a_label = &hint_search;
		break;
	case MODE_LIST:
		a_label = &hint_play;
		break;
	case MODE_EMPTY:
		a_label = &hint_search;
		break;
	case MODE_ERROR:
		a_label = &hint_retry;
		break;
	}
	if (a_label)
		hints[n++] = (ui_hint){ &letter_text[GLYPH_A], a_label,
		                        glyph_colors[GLYPH_A],
		                        look->glyph[GLYPH_A] };
	if (mode == MODE_LIST)
		hints[n++] = (ui_hint){ &letter_text[GLYPH_X], &hint_detail,
		                        glyph_colors[GLYPH_X],
		                        look->glyph[GLYPH_X] };
	if (mode == MODE_LIST || mode == MODE_ERROR)
		hints[n++] = (ui_hint){ &letter_text[GLYPH_Y], &hint_search,
		                        glyph_colors[GLYPH_Y],
		                        look->glyph[GLYPH_Y] };
	hints[n++] = (ui_hint){ &letter_text[GLYPH_B],
	                        mode == MODE_SEARCHING ? &hint_cancel
	                                               : &hint_back,
	                        glyph_colors[GLYPH_B],
	                        look->glyph[GLYPH_B] };
	ui_button_bar(list_hints_rect(), hints, n);
}

/* The recent searches under the button: a label, then one row each with
 * the search icon, the focused one in the list's focused style. */
static void draw_recent(const results_look *look)
{
	ui_text(&recent_label_text, SP_2 + SP_1, list_recent_label_cy(),
	        TXT_META, COL_TEXTO_2);
	C2D_Image icon = icon_image(ICON_SEARCH);
	for (int i = 0; i < recent.count; i++) {
		u32 ink, meta_ink;
		ui_rect row = ui_item_frame(list_recent_rect(i),
		                            look->idle_focus == i + 1,
		                            look->recent[i], &ink, &meta_ink);
		int cy = row.y + row.h / 2;
		ui_icon(&icon, row.x + SP_2, cy - ICON_S / 2, meta_ink);
		ui_text(&recent_text[i], row.x + 2 * SP_2 + ICON_S, cy, TXT_CUERPO,
		        ink);
	}
}

void view_results_draw_bottom(void)
{
	ensure_parsed();
	results_look look = current_look();

	switch (mode) {
	case MODE_SEARCHING:
		/* Skeleton rows: the layout the results will fill. */
		track_list_draw_skeleton();
		break;
	case MODE_LIST:
		track_list_draw(&list, &look.list);
		break;
	case MODE_IDLE:
		if (has_recent()) {
			draw_recent(&look);
			break;
		}
		/* fall through */
	case MODE_EMPTY: {
		/* A single message: centred (3.3). */
		ui_rect notice = list_notice_rect();
		ui_text_centered(&empty_text, SCREEN_BOTTOM_W / 2,
		                 notice.y + notice.h / 2, TXT_CUERPO, COL_TEXTO);
		break;
	}
	case MODE_ERROR: {
		C2D_Image icon = icon_image(ICON_ERROR);
		ui_notice(list_notice_rect(), true, &icon, &notice_title_text,
		          &notice_body_text);
		break;
	}
	}

	if (has_button()) {
		ui_control_look button = {
			.pressed = look.button,
			.focused = !has_recent() || look.idle_focus == 0
		};
		ui_button(button_rect(), UI_BUTTON_PRIMARY, button, &button_text,
		          NULL);
	}

	/* The bar after the rows: nudged and dragged rows slide under it, and
	 * under the tab bar, drawn by the caller. */
	draw_bar(&look);

	/* While searching, the waiting bubble (5.9 with the indicator of 5.7)
	 * sits over the skeleton rows; the language bubble, if it shows too,
	 * stacks above it. */
	int slot = 0;
	if (look.mode == MODE_SEARCHING) {
		ui_bubble_loading(list_bubble_rect(
			ui_bubble_loading_content_width(&status_text)),
			look.loader_step, &status_text);
		slot = 1;
	}
	if (look.bubble) {
		C2D_Image globe = icon_image(ICON_LANGUAGE);
		ui_bubble(list_bubble_slot_rect(ui_bubble_content_width(&bubble_text),
		                                slot),
		          &globe, &bubble_text);
	}

	drawn = look;
	drawn_valid = true;
}
