#include "view_results.h"

#include "draw.h"
#include "icons.h"
#include "list_logic.h"
#include "press_logic.h"
#include "results_logic.h"
#include "tabbar.h"
#include "theme.h"
#include "track_list.h"
#include "ui_font.h"

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

/* The buttons this view uses (2.5); the D-pad's glyph is an icon. */
enum { GLYPH_A, GLYPH_B, GLYPH_X, GLYPH_Y, GLYPH_DPAD, GLYPH_COUNT };
static const char *const glyph_letters[GLYPH_COUNT] = {
	"A", "B", "X", "Y", ""
};
static const u32 glyph_colors[GLYPH_COUNT] = {
	COL_BTN_A, COL_BTN_B, COL_BTN_X, COL_BTN_Y, 0
};

/* Content. The rows, their focus, scroll and input are the list's: one
 * list, loaded with the kind on screen (results_logic.h keeps where the
 * others were left). The kinds other than tracks draw as rows of their
 * kind, which are the view's. */
static results_mode mode = MODE_IDLE;
static track_list list;
static results_content content;
static results_kinds kinds;
static track_row kind_rows[RESULT_KINDS][SEARCH_ITEMS_MAX];
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
                hint_retry, hint_open, hint_kind;

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
	result_kind kind;
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

/* Loads the list with the active kind, where it was left. The covers of
 * a kind left are asked again on coming back to it: a few KB, against
 * keeping four lists. */
static void load_kind(void)
{
	result_kind k = kinds.active;
	track_list_set(&list, content.items[k], kinds.count[k],
	               kinds.selected[k]);
	if (k != RESULT_TRACKS)
		track_list_set_rows(&list, kind_rows[k]);
	if (kinds.first[k] >= 0)
		track_list_scroll_to(&list, kinds.first[k]);
	stale = true;
}

void view_results_show(const results_content *c)
{
	static const uint8_t ROW_KIND[RESULT_KINDS] = {
		TRACK_ROW_TRACK, TRACK_ROW_ARTIST, TRACK_ROW_ALBUM,
		TRACK_ROW_PLAYLIST,
	};
	content = *c;
	int count[RESULT_KINDS];
	for (int k = 0; k < RESULT_KINDS; k++) {
		int max = k == RESULT_TRACKS ? TRACK_LIST_WINDOW : SEARCH_ITEMS_MAX;
		count[k] = !content.items[k] || content.count[k] < 0 ? 0
		         : content.count[k] > max ? max : content.count[k];
		content.count[k] = count[k];
		for (int i = 0; k != RESULT_TRACKS && i < count[k]; i++)
			kind_rows[k][i] = (track_row){
				.kind = ROW_KIND[k], .state = DL_DONE, .search = true,
				.index = (int16_t)i,
			};
	}
	bool any = results_kinds_init(&kinds, content.offered, count,
	                              content.preferred_kind,
	                              content.preferred_index);
	mode = any ? MODE_LIST : MODE_EMPTY;
	if (any)
		load_kind();
	else
		track_list_set(&list, NULL, 0, 0);
	reset_interaction();
	/* Covers now, even before the tab is on screen, unless a list on
	 * screen has them; on screen, the list claims them anyway. */
	track_list_offer_thumbs(&list);
}

result_kind view_results_kind(void)
{
	return kinds.active;
}

/* Left/right: the next kind with results that way, the one left kept
 * where it was. False at an end. */
static bool step_kind(int dir)
{
	results_kind_keep(&kinds, list.selected, list.first);
	if (!results_kind_step(&kinds, dir))
		return false;
	load_kind();
	return true;
}

/* More than one kind to switch between: the D-pad has a use. */
static bool kinds_switch(void)
{
	return mode == MODE_LIST && results_kinds_enabled(&kinds) > 1;
}

/* The tracks are what X opens a detail of. */
static bool on_tracks(void)
{
	return mode == MODE_LIST && kinds.active == RESULT_TRACKS;
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
	ui_parse(text, buf, string);
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
	parse(&hint_open, static_buf, tr(STR_HINT_OPEN));
	parse(&hint_kind, static_buf, tr(STR_HINT_KIND));
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
		.kind = kinds.active,
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
	if (a->mode != b->mode || a->kind != b->kind ||
	    !track_list_same_look(&a->list, &b->list) ||
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
	if (up & (KEY_LEFT | KEY_RIGHT))
		press_end(&press_glyph[GLYPH_DPAD]);
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

	u32 confirm = KEY_A | (on_tracks() ? KEY_X : 0);
	if (mode == MODE_LIST && track_list_realign(&list, down, confirm)) {
		/* 6.6: with the focus dragged out of view, the first button press
		 * only brings the list back to it, on whole rows -- left/right
		 * too, before they switch the kind. A and X still show their
		 * glyph pressed, but act on nothing they cannot see. */
		if (down & KEY_A)
			press_begin(&press_glyph[GLYPH_A]);
		if (down & confirm & KEY_X)
			press_begin(&press_glyph[GLYPH_X]);
		if (down & (KEY_LEFT | KEY_RIGHT))
			press_begin(&press_glyph[GLYPH_DPAD]);
		return RESULTS_ACTION_NONE;
	}

	if (mode == MODE_LIST && (down & (KEY_LEFT | KEY_RIGHT))) {
		/* The kind, without wrapping: at an end nothing moves and the
		 * glyph held is all the answer (STYLES.md 6.6, 7.4). */
		press_begin(&press_glyph[GLYPH_DPAD]);
		step_kind((down & KEY_LEFT) ? -1 : 1);
		return RESULTS_ACTION_NONE;
	}
	if (mode == MODE_LIST)
		track_list_navigate(&list, down & (KEY_UP | KEY_DOWN));
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
	} else if ((down & KEY_X) && on_tracks()) {
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

/* At most four actions (5.6). On the tracks, with kinds to switch:
 * Play, Detail, Search, then Type against the right margin -- B still
 * goes back, as on the home grid, which does not list it either. On the
 * other kinds: Open, Search, Back, Type. Type keeps its place whatever
 * the kind. */
static void draw_bar(const results_look *look)
{
	ui_hint hints[4];
	int n = 0;
	const C2D_Text *a_label = NULL;
	bool tracks = mode == MODE_LIST && look->kind == RESULT_TRACKS;
	bool switches = kinds_switch();
	C2D_Image dpad = icon_image(ICON_DPAD_LR);
	switch (mode) {
	case MODE_SEARCHING:
		break;
	case MODE_IDLE:
		a_label = &hint_search;
		break;
	case MODE_LIST:
		a_label = tracks ? &hint_play : &hint_open;
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
		                        look->glyph[GLYPH_A], NULL };
	if (tracks)
		hints[n++] = (ui_hint){ &letter_text[GLYPH_X], &hint_detail,
		                        glyph_colors[GLYPH_X],
		                        look->glyph[GLYPH_X], NULL };
	if (mode == MODE_LIST || mode == MODE_ERROR)
		hints[n++] = (ui_hint){ &letter_text[GLYPH_Y], &hint_search,
		                        glyph_colors[GLYPH_Y],
		                        look->glyph[GLYPH_Y], NULL };
	if (n < (switches ? 3 : 4))
		hints[n++] = (ui_hint){ &letter_text[GLYPH_B],
		                        mode == MODE_SEARCHING ? &hint_cancel
		                                               : &hint_back,
		                        glyph_colors[GLYPH_B],
		                        look->glyph[GLYPH_B], NULL };
	/* Last, against the right margin: the same place on every kind. */
	if (switches)
		hints[n++] = (ui_hint){ &letter_text[GLYPH_DPAD], &hint_kind,
		                        glyph_colors[GLYPH_DPAD],
		                        look->glyph[GLYPH_DPAD], &dpad };
	ui_button_bar_split(list_hints_rect(), hints, n, switches ? 1 : 0);
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
		track_list_draw_skeleton(list_frame_full());
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
