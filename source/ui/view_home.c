#include "view_home.h"

#include "art_cache.h"
#include "draw.h"
#include "home_feed.h"
#include "home_logic.h"
#include "icons.h"
#include "lang.h"
#include "list_logic.h"
#include "press_logic.h"
#include "theme.h"
#include "tile_grid.h"
#include "ui_font.h"

#include <citro2d.h>

#include <stdio.h>
#include <string.h>

/* Labels, buttons, the notice and the button bar. */
#define TEXT_GLYPHS 1024
#define SCRATCH_GLYPHS UI_FIELD_SIZE

/* Control id for touch tracking: the tiles are the grids' own. */
#define CONTROL_BUTTON 1000

typedef enum {
	MODE_LOADING,
	MODE_ERROR,
	MODE_EMPTY,
	MODE_SIX,      /* the six recent, and "Show more" under them */
	MODE_MORE      /* every playlist kept, scrolling */
} home_mode;

/* The buttons this view uses (2.5). */
enum { GLYPH_A, GLYPH_B, GLYPH_Y, GLYPH_COUNT };
static const char *const glyph_letters[GLYPH_COUNT] = { "A", "B", "Y" };
static const u32 glyph_colors[GLYPH_COUNT] = { COL_BTN_A, COL_BTN_B,
                                               COL_BTN_Y };

/* Content, from home_feed.h. */
static home_mode mode = MODE_LOADING;
static unsigned synced_generation = (unsigned)-1;
static const sp_playlist_ref *items;
static int item_count;
static tile_grid six, more;
static home_focus focus = HOME_FOCUS_TILES;

/* Parsed text. `stale`: content or language changed. */
static C2D_TextBuf text_buf, scratch_buf;
static bool stale = true;
static lang_id parsed_lang = LANG_COUNT;
static C2D_Text label_text;
static C2D_Text more_text;
static C2D_Text button_text;        /* error: retry; empty: search */
static C2D_Text empty_text;
static C2D_Text notice_title_text;
static C2D_Text notice_body_text;
static C2D_Text loading_text;
static C2D_Text bubble_text;
static C2D_Text letter_text[GLYPH_COUNT];
static C2D_Text hint_open, hint_search, hint_back, hint_retry;

/* Interaction (STYLES.md 6). `touch` is the buttons', outside the grids. */
static press_state press_button;    /* "Show more", "Retry" or "Search" */
static press_state press_glyph[GLYPH_COUNT];
static touch_track touch = { -1 };
static int bubble_frames = -1;      /* -1: no bubble */
static int loader_frames;
static home_action pending = HOME_ACTION_NONE;
static const press_state *pending_press;
/* A change of mode inside the view ("Show more", B), waiting for its
 * press to be seen like the actions. */
static void (*pending_step)(void);
static char selected_uri[SP_URI_MAX];

/* The top screen's card for the focused tile: which item it is about, and
 * how long the focus has rested there. A document of its own, only its
 * header used; its serial changes with the item, so the card re-parses. */
static detail_doc preview;
static int preview_index = -1;
static int preview_still;

typedef struct {
	home_mode mode;
	home_focus focus;
	tile_grid_look tiles;
	bool button;
	bool glyph[GLYPH_COUNT];
	bool bubble;
	int loader_step;
} home_look;
static home_look drawn;
static bool drawn_valid;

/* --- Lifetime ---------------------------------------------------------- */

bool view_home_init(void)
{
	text_buf = C2D_TextBufNew(TEXT_GLYPHS);
	scratch_buf = C2D_TextBufNew(SCRATCH_GLYPHS);
	if (!text_buf || !scratch_buf ||
	    !tile_grid_init(&six, home_six_geom()) ||
	    !tile_grid_init(&more, home_more_geom())) {
		view_home_exit();
		return false;
	}
	return true;
}

void view_home_exit(void)
{
	C2D_TextBuf *bufs[] = { &text_buf, &scratch_buf };
	for (size_t i = 0; i < sizeof(bufs) / sizeof(bufs[0]); i++) {
		if (*bufs[i])
			C2D_TextBufDelete(*bufs[i]);
		*bufs[i] = NULL;
	}
	tile_grid_exit(&six);
	tile_grid_exit(&more);
}

static void reset_interaction(void)
{
	tile_grid_reset_input(&six);
	tile_grid_reset_input(&more);
	press_reset(&press_button);
	for (int i = 0; i < GLYPH_COUNT; i++)
		press_reset(&press_glyph[i]);
	touch.target = -1;
	bubble_frames = -1;
	pending = HOME_ACTION_NONE;
	pending_press = NULL;
	pending_step = NULL;
	stale = true;
	drawn_valid = false;
}

static bool has_more(void)
{
	return home_has_more(item_count);
}

/* The feed changed: the mode and the grids follow it. */
static void sync(void)
{
	unsigned generation = home_feed_generation();
	if (generation == synced_generation)
		return;
	synced_generation = generation;
	switch (home_feed_get_state()) {
	case HOME_FEED_IDLE:
	case HOME_FEED_LOADING:
		mode = MODE_LOADING;
		loader_frames = 0;
		items = NULL;
		item_count = 0;
		break;
	case HOME_FEED_FAILED:
		mode = MODE_ERROR;
		items = NULL;
		item_count = 0;
		break;
	case HOME_FEED_READY:
		items = home_feed_items(&item_count);
		mode = item_count > 0 ? MODE_SIX : MODE_EMPTY;
		break;
	}
	int shown = item_count < HOME_RECENT_SHOWN ? item_count
	                                           : HOME_RECENT_SHOWN;
	tile_grid_set(&six, items, shown, 0);
	tile_grid_set(&more, items, item_count, 0);
	focus = HOME_FOCUS_TILES;
	six.has_focus = true;
	/* Another list: the card follows from scratch. */
	preview_index = -1;
	reset_interaction();
}

void view_home_enter(void)
{
	sync();
	reset_interaction();
}

const char *view_home_selected_uri(void)
{
	return selected_uri;
}

/* The tile with the focus, or -1: on the six or "Show more", not on the
 * button and not while loading. */
static int focused_item(void)
{
	if (mode == MODE_MORE)
		return more.count > 0 ? more.selected : -1;
	if (mode == MODE_SIX && focus == HOME_FOCUS_TILES)
		return six.count > 0 ? six.selected : -1;
	return -1;
}

/* Follows the focus once per frame. */
static void follow_preview(void)
{
	int index = focused_item();
	if (index != preview_index) {
		preview_index = index;
		preview_still = 0;
		if (index >= 0 && index < item_count) {
			const sp_playlist_ref *r = &items[index];
			unsigned serial = preview.serial + 1;
			memset(&preview, 0, sizeof(preview));
			preview.serial = serial;
			preview.kind = DETAIL_PLAYLIST;
			preview.state = DETAIL_READY;
			snprintf(preview.uri, sizeof(preview.uri), "%s", r->uri);
			snprintf(preview.title, sizeof(preview.title), "%s", r->name);
			/* The length the folder reports, as "N tracks". */
			preview.count = preview.total_tracks = r->length;
			snprintf(preview.resume, sizeof(preview.resume), "%s",
			         home_feed_resume_text(index));
			/* Already here: no download to hold back. */
			if (art_cache_has(r->art))
				snprintf(preview.art, sizeof(preview.art), "%s", r->art);
		}
		return;
	}
	if (index >= 0 && index < item_count &&
	    ++preview_still == HOME_PREVIEW_DELAY)
		snprintf(preview.art, sizeof(preview.art), "%s", items[index].art);
}

const detail_doc *view_home_preview(void)
{
	return preview_index >= 0 && preview_index < item_count ? &preview
	                                                       : NULL;
}

/* --- Text -------------------------------------------------------------- */

static void parse(C2D_Text *text, const char *string)
{
	ui_parse(text, text_buf, string);
	C2D_TextOptimize(text);
}

static void ensure_parsed(void)
{
	if (stale || parsed_lang != lang_get()) {
		C2D_TextBufClear(text_buf);
		parse(&label_text, tr(home_feed_is_recent() ? STR_HOME_RECENT
		                                            : STR_HOME_YOUR_PLAYLISTS));
		parse(&more_text, tr(STR_HOME_SHOW_MORE));
		parse(&button_text, tr(mode == MODE_ERROR ? STR_RESULTS_RETRY
		                                          : STR_HOME_SEARCH));
		parse(&empty_text, tr(STR_HOME_EMPTY));
		ui_rect notice = list_notice_rect();
		ui_parse_fitted(&notice_title_text, text_buf, scratch_buf,
		                tr(STR_HOME_FAILED_TITLE), TXT_CABEZA,
		                notice.w - 2 * SP_3 - ICON_S - SP_1);
		const char *detail = home_feed_error();
		parse(&notice_body_text, detail[0] ? detail
		                                   : tr(STR_SEARCH_CHECK_WIFI));
		parse(&loading_text, tr(STR_DETAIL_LOADING));
		parse(&bubble_text, tr(STR_LANG_NAME));
		for (int i = 0; i < GLYPH_COUNT; i++)
			parse(&letter_text[i], glyph_letters[i]);
		parse(&hint_open, tr(STR_HINT_OPEN));
		parse(&hint_search, tr(STR_HINT_SEARCH));
		parse(&hint_back, tr(STR_HINT_BACK));
		parse(&hint_retry, tr(STR_HINT_RETRY));
		parsed_lang = lang_get();
		stale = false;
	}
	tile_grid_ensure_parsed(&six);
	tile_grid_ensure_parsed(&more);
}

/* --- State ------------------------------------------------------------- */

static tile_grid *active_grid(void)
{
	return mode == MODE_MORE ? &more : mode == MODE_SIX ? &six : NULL;
}

static home_look current_look(void)
{
	tile_grid *grid = active_grid();
	home_look look = {
		.mode = mode,
		.focus = focus,
		.button = press_down(&press_button),
		.bubble = bubble_frames >= 0,
		.loader_step = mode == MODE_LOADING ? loader_frames / LOADER_STEP
		                                    : 0,
	};
	if (grid)
		look.tiles = tile_grid_get_look(grid);
	for (int i = 0; i < GLYPH_COUNT; i++)
		look.glyph[i] = press_down(&press_glyph[i]);
	return look;
}

static bool same_look(const home_look *a, const home_look *b)
{
	if (a->mode != b->mode || a->focus != b->focus ||
	    !tile_grid_same_look(&a->tiles, &b->tiles) ||
	    a->button != b->button || a->bubble != b->bubble ||
	    a->loader_step != b->loader_step)
		return false;
	for (int i = 0; i < GLYPH_COUNT; i++)
		if (a->glyph[i] != b->glyph[i])
			return false;
	return true;
}

static void tick(void)
{
	tile_grid_tick(&six);
	tile_grid_tick(&more);
	press_tick(&press_button);
	for (int i = 0; i < GLYPH_COUNT; i++)
		press_tick(&press_glyph[i]);
	/* 5.9: shown BUBBLE_SHOW frames, gone without a transition. */
	if (bubble_frames >= 0 && ++bubble_frames >= BUBBLE_SHOW)
		bubble_frames = -1;
	if (mode == MODE_LOADING)
		loader_frames++;
}

/* --- Input ------------------------------------------------------------- */

static void queue(home_action action, const press_state *press)
{
	pending = action;
	pending_press = press;
}

static void queue_step(void (*step)(void), const press_state *press)
{
	pending_step = step;
	pending_press = press;
}

/* The one button of the mode, if any: "Show more" with the six (when
 * there are more), "Retry" on error, "Search songs" when empty. */
static bool has_button(void)
{
	return (mode == MODE_SIX && has_more()) || mode == MODE_ERROR ||
	       mode == MODE_EMPTY;
}

static ui_rect button_rect(void)
{
	if (mode == MODE_SIX)
		return home_more_rect();
	return list_action_rect(ui_text_width(&button_text, TXT_CUERPO),
	                        mode == MODE_EMPTY);
}

static void set_focus(home_focus f)
{
	focus = f;
	six.has_focus = f == HOME_FOCUS_TILES;
}

/* "Show more": the same tab with every tile, from the first. */
static void enter_more(void)
{
	mode = MODE_MORE;
	tile_grid_set(&more, items, item_count, 0);
	reset_interaction();
}

static void leave_more(void)
{
	mode = MODE_SIX;
	set_focus(has_more() ? HOME_FOCUS_MORE : HOME_FOCUS_TILES);
	reset_interaction();
}

static void open_tile(const tile_grid *grid, int index)
{
	if (index < 0 || index >= item_count)
		return;
	snprintf(selected_uri, sizeof(selected_uri), "%s", grid->items[index].uri);
}

/* What the button does, once its press has been seen. */
static void activate_button(void)
{
	switch (mode) {
	case MODE_SIX:
		queue_step(enter_more, &press_button);
		break;
	case MODE_ERROR:
		queue(HOME_ACTION_RETRY, &press_button);
		break;
	case MODE_EMPTY:
		queue(HOME_ACTION_SEARCH, &press_button);
		break;
	default:
		break;
	}
}

static void release_keys(u32 up)
{
	tile_grid *grid = active_grid();
	if (up & KEY_A) {
		press_end(&press_glyph[GLYPH_A]);
		if (grid)
			tile_grid_release(grid);
		if (touch.target < 0)
			press_end(&press_button);
	}
	if (up & KEY_B)
		press_end(&press_glyph[GLYPH_B]);
	if (up & KEY_Y)
		press_end(&press_glyph[GLYPH_Y]);
}

/* Returns true while the stylus owns the input: a grid's tiles, or the
 * mode's button. */
static bool handle_touch(u32 down, u32 held, u32 up)
{
	tile_grid *grid = active_grid();
	if (grid && touch.target < 0) {
		tile_grid_touch event = tile_grid_update_touch(grid, down, held, up);
		if (event == TILE_GRID_FOCUSED && grid == &six)
			set_focus(HOME_FOCUS_TILES);
		if (event == TILE_GRID_ACTIVATE) {
			open_tile(grid, grid->selected);
			queue(HOME_ACTION_OPEN, &grid->press);
		}
		if (event != TILE_GRID_IDLE)
			return true;
	}

	int hit = -1;
	if (held & KEY_TOUCH) {
		touchPosition pos;
		hidTouchRead(&pos);
		if (has_button() && ui_rect_contains(button_rect(), pos.px, pos.py))
			hit = CONTROL_BUTTON;
	}
	int control;
	touch_event event = touch_track_update(&touch, (down & KEY_TOUCH) != 0,
	                                       (held & KEY_TOUCH) != 0,
	                                       (up & KEY_TOUCH) != 0, hit,
	                                       &control);
	switch (event) {
	case TOUCH_PRESS:
		if (mode == MODE_SIX)
			set_focus(HOME_FOCUS_MORE);
		press_begin(&press_button);
		break;
	case TOUCH_CANCEL:
		press_cancel(&press_button);
		break;
	case TOUCH_ACTIVATE:
		press_end(&press_button);
		activate_button();
		break;
	case TOUCH_NONE:
		break;
	}
	return touch.target >= 0;
}

/* The D-pad in the six: between the tiles and "Show more" (6.6). */
static void navigate_six(u32 down)
{
	if (focus == HOME_FOCUS_TILES && (down & KEY_DOWN)) {
		home_focus next = home_focus_vertical(focus, true,
		                                      tile_grid_can_move(&six,
		                                                         GRID_DOWN),
		                                      has_more());
		if (next == HOME_FOCUS_MORE) {
			set_focus(next);
			return;
		}
	}
	if (focus == HOME_FOCUS_MORE) {
		if (down & KEY_UP)
			set_focus(HOME_FOCUS_TILES);
		return;
	}
	tile_grid_navigate(&six, down);
}

home_action view_home_update(u32 down)
{
	u32 held = hidKeysHeld();
	u32 up = hidKeysUp();
	sync();
	ensure_parsed();
	tick();
	release_keys(up);
	/* Uploads happen here, on the main thread, never while drawing. */
	tile_grid *grid = active_grid();
	if (grid)
		tile_grid_thumbs_frame(grid);

	follow_preview();
	if (pending != HOME_ACTION_NONE || pending_step) {
		if (!press_shown(pending_press))
			return HOME_ACTION_NONE;
		home_action action = pending;
		void (*step)(void) = pending_step;
		pending = HOME_ACTION_NONE;
		pending_step = NULL;
		pending_press = NULL;
		if (step)
			step();
		return action;
	}

	if (handle_touch(down, held, up) || pending != HOME_ACTION_NONE ||
	    pending_step)
		return HOME_ACTION_NONE;

	if (grid && (mode == MODE_MORE || focus == HOME_FOCUS_TILES) &&
	    tile_grid_realign(grid, down, KEY_A)) {
		/* 6.6: the first press only brings the grid back to the focus. */
		if (down & KEY_A)
			press_begin(&press_glyph[GLYPH_A]);
		return HOME_ACTION_NONE;
	}

	if (mode == MODE_SIX)
		navigate_six(down);
	else if (mode == MODE_MORE)
		tile_grid_navigate(&more, down);

	if (down & KEY_A) {
		press_begin(&press_glyph[GLYPH_A]);
		if (grid && (mode == MODE_MORE || focus == HOME_FOCUS_TILES)) {
			open_tile(grid, grid->selected);
			queue(HOME_ACTION_OPEN, tile_grid_focused_press(grid));
			press_begin(&grid->press);
		} else if (has_button()) {
			press_begin(&press_button);
			activate_button();
		}
	} else if ((down & KEY_B) && mode == MODE_MORE) {
		press_begin(&press_glyph[GLYPH_B]);
		queue_step(leave_more, &press_glyph[GLYPH_B]);
	} else if (down & KEY_Y) {
		press_begin(&press_glyph[GLYPH_Y]);
		queue(HOME_ACTION_SEARCH, &press_glyph[GLYPH_Y]);
	} else if (down & KEY_SELECT) {
		/* No control on screen: a bubble with the result (6.5). */
		bubble_frames = 0;
		return HOME_ACTION_LANGUAGE;
	}
	return HOME_ACTION_NONE;
}

bool view_home_dirty(void)
{
	sync();
	if (stale || parsed_lang != lang_get() || !drawn_valid ||
	    tile_grid_text_stale(&six) || tile_grid_text_stale(&more))
		return true;
	home_look look = current_look();
	return !same_look(&look, &drawn);
}

/* --- Drawing ----------------------------------------------------------- */

static void draw_bar(const home_look *look)
{
	ui_hint hints[GLYPH_COUNT];
	int n = 0;
	const C2D_Text *a_label = NULL;
	switch (mode) {
	case MODE_SIX:
	case MODE_MORE:
		a_label = &hint_open;
		break;
	case MODE_ERROR:
		a_label = &hint_retry;
		break;
	case MODE_EMPTY:
		a_label = &hint_search;
		break;
	case MODE_LOADING:
		break;
	}
	if (a_label)
		hints[n++] = (ui_hint){ &letter_text[GLYPH_A], a_label,
		                        glyph_colors[GLYPH_A],
		                        look->glyph[GLYPH_A] };
	if (mode != MODE_EMPTY)
		hints[n++] = (ui_hint){ &letter_text[GLYPH_Y], &hint_search,
		                        glyph_colors[GLYPH_Y],
		                        look->glyph[GLYPH_Y] };
	if (mode == MODE_MORE)
		hints[n++] = (ui_hint){ &letter_text[GLYPH_B], &hint_back,
		                        glyph_colors[GLYPH_B],
		                        look->glyph[GLYPH_B] };
	ui_button_bar(home_hints_rect(), hints, n);
}

static void draw_label(void)
{
	ui_rect label = home_label_rect();
	ui_text(&label_text, label.x, label.y + label.h / 2, TXT_META,
	        COL_TEXTO_2);
}

void view_home_draw_bottom(void)
{
	ensure_parsed();
	home_look look = current_look();

	switch (mode) {
	case MODE_LOADING:
		tile_grid_draw_skeleton(home_six_geom(), HOME_RECENT_SHOWN);
		break;
	case MODE_SIX:
		draw_label();
		tile_grid_draw(&six, &look.tiles);
		break;
	case MODE_MORE:
		tile_grid_draw(&more, &look.tiles);
		break;
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
		bool six_mode = mode == MODE_SIX;
		ui_control_look button = {
			.pressed = look.button,
			.focused = !six_mode || focus == HOME_FOCUS_MORE,
		};
		ui_button(button_rect(),
		          six_mode ? UI_BUTTON_SECONDARY : UI_BUTTON_PRIMARY, button,
		          six_mode ? &more_text : &button_text, NULL);
	}

	/* The bar after the tiles: dragged tiles slide under it, and under
	 * the tab bar, drawn by the caller. */
	draw_bar(&look);

	/* While loading, the waiting bubble (5.9 with 5.7) over the skeleton;
	 * the language bubble, if it shows too, stacks above it. */
	int slot = 0;
	if (look.mode == MODE_LOADING) {
		ui_bubble_loading(list_bubble_rect(
			ui_bubble_loading_content_width(&loading_text)),
			look.loader_step, &loading_text);
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
