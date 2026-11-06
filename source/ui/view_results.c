#include "view_results.h"

#include "draw.h"
#include "icons.h"
#include "list_logic.h"
#include "press_logic.h"
#include "text_fit.h"
#include "thumbs.h"
#include "theme.h"

#include <citro2d.h>

#include <stdio.h>
#include <string.h>

#define MAX_TRACKS 24
#define QUERY_SIZE 96
#define ERROR_SIZE 192
/* Fitted text: at most the source plus "...". */
#define FIELD_SIZE 136

#define STATIC_GLYPHS 1024
#define ROW_GLYPHS    3072
#define DETAIL_GLYPHS 640
#define RANGE_GLYPHS  32
#define SCRATCH_GLYPHS FIELD_SIZE

/* Control ids for touch tracking: rows are their track index. */
#define CONTROL_BUTTON 1000

typedef enum {
	MODE_SEARCHING,
	MODE_LIST,
	MODE_EMPTY,
	MODE_ERROR
} results_mode;

/* The buttons this view uses (2.5). */
enum { GLYPH_A, GLYPH_B, GLYPH_Y, GLYPH_COUNT };
static const char *const glyph_letters[GLYPH_COUNT] = { "A", "B", "Y" };
static const u32 glyph_colors[GLYPH_COUNT] = {
	COL_BTN_A, COL_BTN_B, COL_BTN_Y
};

/* Content. */
static results_mode mode = MODE_SEARCHING;
static char query[QUERY_SIZE];
static const sp_track *tracks;
static int count;
static int selected;
static int first;       /* first visible row: scroll / LIST_PITCH */
static int scroll;      /* px; whole rows except while dragging */
static char error_detail[ERROR_SIZE];
static str_id advice = STR_SEARCH_CHECK_WIFI;
static bool cancelling;
/* Bumped whenever a thumbnail arrives, so the list repaints. */
static int thumbs_serial;

/* Parsed text. `stale`: content or language changed. */
static C2D_TextBuf static_buf, row_buf, detail_buf, range_buf, scratch_buf;
static bool stale = true;
static lang_id parsed_lang = LANG_COUNT;
static int parsed_first = -1;
static int parsed_selected = -1;

static C2D_Text query_title_text;   /* top: Results: "query" */
static C2D_Text header_text;        /* bottom header */
static C2D_Text status_text;        /* top, while searching */
static C2D_Text empty_text;
static C2D_Text button_text;        /* empty: new search; error: retry */
static C2D_Text notice_title_text;
static C2D_Text notice_body_text;
static C2D_Text error_detail_text;
static C2D_Text bubble_text;
static C2D_Text letter_text[GLYPH_COUNT];
static C2D_Text hint_play, hint_back, hint_search, hint_cancel, hint_retry;
static C2D_Text range_text;
static C2D_Text row_title[MAX_TRACKS];
static C2D_Text row_artist[MAX_TRACKS];
static C2D_Text row_duration[MAX_TRACKS];
static float row_duration_width[MAX_TRACKS];
static C2D_Text detail_title, detail_artist, detail_album, detail_duration;
static bool detail_has_album;

/* Interaction (STYLES.md 6). */
static press_state press_row;
static int press_row_index = -1;
static press_state press_button;
static press_state press_glyph[GLYPH_COUNT];
static touch_track touch = { -1 };
static list_drag drag;
static int edge_frames;
static int edge_dir;
static int bubble_frames = -1;
static int loader_frames;
static results_action pending = RESULTS_ACTION_NONE;
static const press_state *pending_press;

typedef struct {
	results_mode mode;
	int selected, first, scroll;
	int pressed_row;      /* -1: none */
	bool button;
	bool glyph[GLYPH_COUNT];
	int edge;             /* the focused row's nudge, px */
	bool bubble;
	int loader_step;
	bool cancelling;
	int thumbs;
} results_look;
static results_look drawn;
static bool drawn_valid;

/* --- Lifetime ---------------------------------------------------------- */

bool view_results_init(void)
{
	static_buf = C2D_TextBufNew(STATIC_GLYPHS);
	row_buf = C2D_TextBufNew(ROW_GLYPHS);
	detail_buf = C2D_TextBufNew(DETAIL_GLYPHS);
	range_buf = C2D_TextBufNew(RANGE_GLYPHS);
	scratch_buf = C2D_TextBufNew(SCRATCH_GLYPHS);
	if (!static_buf || !row_buf || !detail_buf || !range_buf ||
	    !scratch_buf) {
		view_results_exit();
		return false;
	}
	return true;
}

void view_results_exit(void)
{
	C2D_TextBuf *bufs[] = {
		&static_buf, &row_buf, &detail_buf, &range_buf, &scratch_buf
	};
	for (size_t i = 0; i < sizeof(bufs) / sizeof(bufs[0]); i++) {
		if (*bufs[i])
			C2D_TextBufDelete(*bufs[i]);
		*bufs[i] = NULL;
	}
}

static void reset_interaction(void)
{
	press_reset(&press_row);
	press_row_index = -1;
	press_reset(&press_button);
	for (int i = 0; i < GLYPH_COUNT; i++)
		press_reset(&press_glyph[i]);
	touch.target = -1;
	drag.down = drag.dragging = false;
	edge_frames = 0;
	bubble_frames = -1;
	loader_frames = 0;
	pending = RESULTS_ACTION_NONE;
	pending_press = NULL;
	stale = true;
	drawn_valid = false;
}

static void set_query(const char *text)
{
	snprintf(query, sizeof(query), "%s", text ? text : "");
}

void view_results_searching(const char *text)
{
	set_query(text);
	mode = MODE_SEARCHING;
	tracks = NULL;
	count = selected = first = scroll = 0;
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

void view_results_show(const char *text, const sp_track *list, int n,
                       int focus)
{
	set_query(text);
	tracks = list;
	count = n < 0 ? 0 : n > MAX_TRACKS ? MAX_TRACKS : n;
	mode = count > 0 ? MODE_LIST : MODE_EMPTY;
	selected = list_clamp(focus, count);
	first = list_scroll(0, selected, count, RESULTS_PER_PAGE);
	scroll = first * LIST_PITCH;
	reset_interaction();
	/* Covers: visible rows first (STYLES.md 9). */
	thumbs_set_tracks(list, count);
	thumbs_set_window(first);
}

void view_results_error(const char *text, const char *error, str_id help)
{
	set_query(text);
	mode = MODE_ERROR;
	tracks = NULL;
	count = selected = first = scroll = 0;
	snprintf(error_detail, sizeof(error_detail), "%s", error ? error : "");
	advice = help;
	reset_interaction();
}

int view_results_selected(void)
{
	return selected;
}

/* --- Text -------------------------------------------------------------- */

static void parse(C2D_Text *text, C2D_TextBuf buf, const char *string)
{
	C2D_TextParse(text, buf, string);
	C2D_TextOptimize(text);
}

static float measure(const char *string, void *ctx)
{
	float scale = *(const float *)ctx;
	C2D_Text text;
	C2D_TextBufClear(scratch_buf);
	C2D_TextParse(&text, scratch_buf, string);
	return ui_text_width(&text, scale);
}

/* Parses `string` cut with "..." to `width` px at `scale` (3.3). */
static void parse_fitted(C2D_Text *text, C2D_TextBuf buf, const char *string,
                         float scale, int width)
{
	char fitted[FIELD_SIZE];
	text_fit(string, (float)width, measure, &scale, fitted, sizeof(fitted));
	parse(text, buf, fitted);
}

static const char *track_name(const sp_track *t)
{
	return t->name[0] ? t->name : tr(STR_UNTITLED);
}

static const char *track_artist(const sp_track *t)
{
	return t->artist[0] ? t->artist : tr(STR_UNKNOWN_ARTIST);
}

/* Width of a row's text column. */
static int row_text_width(void)
{
	ui_rect row = list_row_rect(0);
	return row.x + row.w - SP_2 - list_text_x(row);
}

/* The top screen's content width. */
static int top_width(void)
{
	return SCREEN_TOP_W - 2 * SP_3;
}

static void parse_static(void)
{
	C2D_TextBufClear(static_buf);
	char line[QUERY_SIZE + 32];
	snprintf(line, sizeof(line), tr(STR_RESULTS_QUERY_FMT), query);
	parse_fitted(&query_title_text, static_buf, line, TXT_CABEZA,
	             top_width());
	parse(&header_text, static_buf,
	      tr(mode == MODE_SEARCHING ? STR_RESULTS_SEARCHING
	                                : STR_RESULTS_HEADER));
	parse(&status_text, static_buf,
	      tr(cancelling ? STR_SEARCH_CANCELLING : STR_RESULTS_SEARCHING));
	parse(&empty_text, static_buf, tr(STR_RESULTS_EMPTY));
	parse(&button_text, static_buf,
	      tr(mode == MODE_ERROR ? STR_RESULTS_RETRY
	                            : STR_RESULTS_NEW_SEARCH));
	ui_rect notice = list_notice_rect();
	parse_fitted(&notice_title_text, static_buf, tr(STR_SEARCH_FAILED_TITLE),
	             TXT_CABEZA, notice.w - 2 * SP_3 - ICON_S - SP_1);
	parse(&notice_body_text, static_buf, tr(advice));
	parse(&error_detail_text, static_buf, error_detail);
	parse(&bubble_text, static_buf, tr(STR_LANG_NAME));
	for (int i = 0; i < GLYPH_COUNT; i++)
		parse(&letter_text[i], static_buf, glyph_letters[i]);
	parse(&hint_play, static_buf, tr(STR_HINT_PLAY));
	parse(&hint_back, static_buf, tr(STR_HINT_BACK));
	parse(&hint_search, static_buf, tr(STR_HINT_SEARCH));
	parse(&hint_cancel, static_buf, tr(STR_HINT_CANCEL));
	parse(&hint_retry, static_buf, tr(STR_HINT_RETRY));
}

static void parse_rows(void)
{
	C2D_TextBufClear(row_buf);
	int width = row_text_width();
	for (int i = 0; i < count; i++) {
		char duration[16];
		list_format_duration(tracks[i].duration_ms, duration,
		                     sizeof(duration));
		parse(&row_duration[i], row_buf, duration);
		row_duration_width[i] = ui_text_width(&row_duration[i], TXT_META);
		/* The duration is never cut (5.3): the title makes room. */
		int title_width = width - ui_px_ceil(row_duration_width[i]) - SP_2;
		parse_fitted(&row_title[i], row_buf, track_name(&tracks[i]),
		             TXT_CABEZA, title_width);
		parse_fitted(&row_artist[i], row_buf, track_artist(&tracks[i]),
		             TXT_CUERPO, width);
	}
}

static void parse_range(void)
{
	C2D_TextBufClear(range_buf);
	int from, to;
	list_range(first, count, RESULTS_PER_PAGE, &from, &to);
	char range[32];
	snprintf(range, sizeof(range), tr(STR_RESULTS_RANGE_FMT), from, to,
	         count);
	parse(&range_text, range_buf, range);
}

/* Inside the detail panel on the top screen. */
static int detail_width(void)
{
	return top_width() - 2 * SP_3;
}

static void parse_detail(void)
{
	C2D_TextBufClear(detail_buf);
	detail_has_album = false;
	if (mode != MODE_LIST)
		return;
	const sp_track *t = &tracks[selected];
	parse_fitted(&detail_title, detail_buf, track_name(t), TXT_CABEZA,
	             detail_width());
	parse_fitted(&detail_artist, detail_buf, track_artist(t), TXT_CUERPO,
	             detail_width());
	detail_has_album = t->album[0] != '\0';
	if (detail_has_album)
		parse_fitted(&detail_album, detail_buf, t->album, TXT_CUERPO,
		             detail_width());
	char duration[16];
	list_format_duration(t->duration_ms, duration, sizeof(duration));
	parse(&detail_duration, detail_buf, duration);
}

static void ensure_parsed(void)
{
	if (stale || parsed_lang != lang_get()) {
		parse_static();
		parse_rows();
		parsed_lang = lang_get();
		parsed_first = parsed_selected = -1;
		stale = false;
	}
	if (parsed_first != first) {
		parse_range();
		parsed_first = first;
	}
	if (parsed_selected != selected) {
		parse_detail();
		parsed_selected = selected;
	}
}

/* --- State ------------------------------------------------------------- */

static results_look current_look(void)
{
	results_look look = {
		.mode = mode,
		.selected = selected,
		.first = first,
		.scroll = scroll,
		.pressed_row = press_down(&press_row) ? press_row_index : -1,
		.button = press_down(&press_button),
		.edge = edge_frames > 0 ? edge_dir * EDGE_SHIFT : 0,
		.bubble = bubble_frames >= 0,
		.loader_step = mode == MODE_SEARCHING
		               ? loader_frames / LOADER_STEP : 0,
		.cancelling = cancelling,
		.thumbs = thumbs_serial,
	};
	for (int i = 0; i < GLYPH_COUNT; i++)
		look.glyph[i] = press_down(&press_glyph[i]);
	return look;
}

static bool same_look(const results_look *a, const results_look *b)
{
	if (a->mode != b->mode || a->selected != b->selected ||
	    a->first != b->first || a->scroll != b->scroll ||
	    a->pressed_row != b->pressed_row ||
	    a->button != b->button || a->edge != b->edge ||
	    a->bubble != b->bubble || a->loader_step != b->loader_step ||
	    a->cancelling != b->cancelling || a->thumbs != b->thumbs)
		return false;
	for (int i = 0; i < GLYPH_COUNT; i++)
		if (a->glyph[i] != b->glyph[i])
			return false;
	return true;
}

static void tick(void)
{
	press_tick(&press_row);
	if (!press_down(&press_row))
		press_row_index = -1;
	press_tick(&press_button);
	for (int i = 0; i < GLYPH_COUNT; i++)
		press_tick(&press_glyph[i]);
	if (edge_frames > 0)
		edge_frames--;
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

/* The buttons: focus `index` and bring the list to whole rows with it in
 * view (6.6), even when a drag left it cut. */
static void focus_row(int index)
{
	selected = list_clamp(index, count);
	first = list_align(scroll, selected, count);
	scroll = first * LIST_PITCH;
	thumbs_set_window(first);
}

/* 6.9: the list follows the stylus pixel for pixel. */
static void drag_to(int y)
{
	scroll = list_drag_scroll(&drag, y, count);
	int now_first = scroll / LIST_PITCH;
	if (now_first != first) {
		first = now_first;
		thumbs_set_window(first);
	}
}

/* 6.9: on lifting the stylus the list stays where it is, rows cut or not,
 * and the focus stays on its row even if that is now out of view. */
static void drag_end(void)
{
	drag.down = drag.dragging = false;
}

/* 6.6: the focus moves in the same frame; at an end, the focused row
 * nudges towards it instead. */
static void move_focus(int delta)
{
	bool bumped;
	int next = list_move(selected, delta, count, &bumped);
	/* Whole rows again, bumped or not. */
	focus_row(next);
	if (bumped) {
		edge_dir = delta < 0 ? -1 : 1;
		edge_frames = ANIM_EDGE;
	}
}

static bool has_button(void)
{
	return mode == MODE_EMPTY || mode == MODE_ERROR;
}

static results_action button_action(void)
{
	return mode == MODE_ERROR ? RESULTS_ACTION_RETRY
	                          : RESULTS_ACTION_NEW_SEARCH;
}

static ui_rect button_rect(void)
{
	return list_action_rect(ui_text_width(&button_text, TXT_CUERPO),
	                        mode == MODE_EMPTY);
}

static void release_keys(u32 up)
{
	if (up & KEY_A) {
		press_end(&press_glyph[GLYPH_A]);
		if (touch.target < 0) {
			press_end(&press_row);
			press_end(&press_button);
		}
	}
	if (up & KEY_B)
		press_end(&press_glyph[GLYPH_B]);
	if (up & KEY_Y)
		press_end(&press_glyph[GLYPH_Y]);
}

/* Returns true while the stylus owns the input. */
static bool handle_touch(u32 down, u32 held, u32 up)
{
	int hit = -1;
	if (mode == MODE_LIST) {
		if (up & KEY_TOUCH) {
			bool was_drag = drag.dragging;
			drag.down = false;
			if (was_drag) {
				drag_end();
				return true;
			}
		} else if (held & KEY_TOUCH) {
			touchPosition pos;
			hidTouchRead(&pos);
			if (down & KEY_TOUCH) {
				list_drag_begin(&drag, pos.py, scroll);
			} else if (list_drag_update(&drag, pos.py)) {
				/* A drag is not a tap: the row returns to rest without
				 * acting (6.4, 6.9). */
				if (touch.target >= 0) {
					press_cancel(&press_row);
					touch.target = -1;
				}
				drag_to(pos.py);
				return true;
			}
		}
	}
	if (held & KEY_TOUCH) {
		touchPosition pos;
		hidTouchRead(&pos);
		if (mode == MODE_LIST)
			hit = list_hit_row(pos.px, pos.py, scroll, count);
		else if (has_button() &&
		         ui_rect_contains(button_rect(), pos.px, pos.py))
			hit = CONTROL_BUTTON;
	}
	int control;
	touch_event event = touch_track_update(&touch, (down & KEY_TOUCH) != 0,
	                                       (held & KEY_TOUCH) != 0,
	                                       (up & KEY_TOUCH) != 0, hit,
	                                       &control);
	press_state *press = control == CONTROL_BUTTON ? &press_button
	                                               : &press_row;
	switch (event) {
	case TOUCH_PRESS:
		press_begin(press);
		if (control != CONTROL_BUTTON)
			press_row_index = control;
		break;
	case TOUCH_CANCEL:
		press_cancel(press);
		break;
	case TOUCH_ACTIVATE:
		press_end(press);
		if (control == CONTROL_BUTTON)
			queue(button_action(), press);
		else if (control == selected)
			queue(RESULTS_ACTION_OPEN, press);  /* tap the focused row */
		else
			selected = control;  /* tap another: focus, list stays put */
		break;
	case TOUCH_NONE:
		break;
	}
	return touch.target >= 0;
}

/* The A, B and Y glyphs plus what each does in this mode. */
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
	if (thumbs_poll())
		thumbs_serial++;

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
	    (down & (KEY_UP | KEY_DOWN | KEY_L | KEY_R | KEY_A)) &&
	    !list_row_whole(selected, scroll, count)) {
		/* 6.6: with the focus dragged out of view, the first button press
		 * only brings the list back to it, on whole rows. A still shows
		 * its glyph pressed, but plays nothing it cannot see. */
		focus_row(selected);
		if (down & KEY_A)
			press_begin(&press_glyph[GLYPH_A]);
		return RESULTS_ACTION_NONE;
	}

	if (mode == MODE_LIST) {
		if (down & KEY_UP)
			move_focus(-1);
		else if (down & KEY_DOWN)
			move_focus(1);
		else if (down & KEY_L)
			move_focus(-RESULTS_PER_PAGE);
		else if (down & KEY_R)
			move_focus(RESULTS_PER_PAGE);
	}

	if (down & KEY_A) {
		if (mode == MODE_LIST) {
			press_row_index = selected;
			press_key(GLYPH_A, RESULTS_ACTION_OPEN, &press_row);
		} else {
			press_key(GLYPH_A, button_action(), &press_button);
		}
	} else if (down & KEY_B) {
		press_key(GLYPH_B, RESULTS_ACTION_BACK, NULL);
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
	if (stale || parsed_lang != lang_get() || !drawn_valid)
		return true;
	results_look look = current_look();
	return !same_look(&look, &drawn);
}

/* --- Drawing ----------------------------------------------------------- */

/* 5.3: the cover inside a 1 px `tinta` outline, or, while it loads or when
 * there is none, `capa_alta` with the note icon. Same size either way, so
 * nothing moves when the image arrives. `index` < 0: a skeleton row. */
static void draw_thumb(ui_rect row, int index)
{
	ui_rect thumb = list_thumb_rect(row);
	ui_fill(thumb, COL_TINTA);
	ui_rect inside = ui_rect_inset(thumb, OUTLINE_W);
	C2D_Image image;
	if (index >= 0 && thumbs_image(index, &image)) {
		/* Fitted into the square, centred on whole pixels. */
		int w = image.subtex->width, h = image.subtex->height;
		int longest = w > h ? w : h;
		float scale = (float)inside.w / (float)longest;
		int shown_w = ui_px_ceil(w * scale), shown_h = ui_px_ceil(h * scale);
		if (shown_w < inside.w || shown_h < inside.h)
			ui_fill(inside, COL_CAPA_ALTA);
		C2D_DrawImageAt(image, inside.x + (inside.w - shown_w) / 2,
		                inside.y + (inside.h - shown_h) / 2, 0.0f, NULL,
		                scale, scale);
		return;
	}
	ui_fill(inside, COL_CAPA_ALTA);
	C2D_Image note = icon_image(ICON_NOTE);
	ui_icon(&note, thumb.x + (thumb.w - ICON_S) / 2,
	        thumb.y + (thumb.h - ICON_S) / 2, COL_TEXTO_2);
}

/* 5.3. `index` < 0 draws an empty skeleton row (searching). */
static void draw_row(ui_rect row, int index, const results_look *look)
{
	bool focused = index >= 0 && index == look->selected;
	bool pressed = index >= 0 && index == look->pressed_row;
	if (focused)
		row = ui_rect_offset(row, 0, look->edge);
	if (pressed)
		row = ui_rect_offset(row, PRESS_SHIFT, PRESS_SHIFT);

	u32 title_ink = COL_TEXTO;
	u32 meta_ink = COL_TEXTO_2;
	if (pressed || focused) {
		/* Focused: `clorofila` with a `musgo` bottom band; pressed: the
		 * dark fill (6.3). Text in `tinta` either way. */
		ui_fill(row, COL_TINTA);
		ui_rect inside = ui_rect_inset(row, OUTLINE_W);
		ui_fill(inside, pressed ? COL_MUSGO : COL_CLOROFILA);
		if (!pressed) {
			ui_rect band = { inside.x, inside.y + inside.h - BEVEL_W,
			                 inside.w, BEVEL_W };
			ui_fill(band, COL_MUSGO);
		}
		title_ink = meta_ink = COL_TINTA;
	} else {
		ui_panel(row);
	}
	draw_thumb(row, index);
	if (index < 0)
		return;

	int x = list_text_x(row);
	int title_cy = row.y + SP_1 + ui_line_height(TXT_CABEZA) / 2;
	int artist_cy = row.y + row.h - SP_1 - ui_line_height(TXT_CUERPO) / 2;
	ui_text(&row_title[index], x, title_cy, TXT_CABEZA, title_ink);
	ui_text(&row_artist[index], x, artist_cy, TXT_CUERPO, meta_ink);
	int duration_x = row.x + row.w - SP_2 -
	                 ui_px_ceil(row_duration_width[index]);
	ui_text(&row_duration[index], duration_x, title_cy, TXT_META, meta_ink);
}

static void draw_header(void)
{
	ui_rect header = list_header_rect();
	ui_panel(header);
	int cy = header.y + header.h / 2;
	ui_text(&header_text, SP_2, cy, TXT_CABEZA, COL_TEXTO);
	if (mode == MODE_LIST || mode == MODE_EMPTY) {
		int w = ui_px_ceil(ui_text_width(&range_text, TXT_META));
		ui_text(&range_text, header.x + header.w - SP_2 - w, cy, TXT_META,
		        COL_TEXTO_2);
	}
}

static void draw_bar(const results_look *look)
{
	ui_hint hints[3];
	int n = 0;
	const C2D_Text *a_label = NULL;
	switch (mode) {
	case MODE_SEARCHING:
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

void view_results_draw_bottom(void)
{
	ensure_parsed();
	results_look look = current_look();

	switch (mode) {
	case MODE_SEARCHING:
		/* Skeleton rows: the layout the results will fill. */
		for (int slot = 0; slot < RESULTS_PER_PAGE; slot++)
			draw_row(list_row_rect(slot), -1, &look);
		break;
	case MODE_LIST: {
		/* Mid-drag one more row peeks in at the bottom. The focused row
		 * goes last, so its nudge draws over its neighbours. */
		int last = first + RESULTS_PER_PAGE;
		if (last > count - 1)
			last = count - 1;
		for (int index = first; index <= last; index++)
			if (index != selected)
				draw_row(list_row_at(index, scroll), index, &look);
		if (selected >= first && selected <= last)
			draw_row(list_row_at(selected, scroll), selected, &look);
		/* 5.11: only when the list does not fit. */
		if (count > RESULTS_PER_PAGE) {
			ui_fill(list_scroll_track(), COL_CAPA);
			ui_fill(list_scroll_thumb(scroll, count), COL_CLOROFILA);
		}
		break;
	}
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
		ui_control_look button = { .pressed = look.button, .focused = true };
		ui_button(button_rect(), UI_BUTTON_PRIMARY, button, &button_text,
		          NULL);
	}

	/* Bars after the rows: nudged and dragged rows slide under them. */
	draw_header();
	draw_bar(&look);

	if (look.bubble) {
		C2D_Image globe = icon_image(ICON_LANGUAGE);
		ui_bubble(list_bubble_rect(ui_bubble_content_width(&bubble_text)),
		          &globe, &bubble_text);
	}

	drawn = look;
	drawn_valid = true;
}

static void draw_detail(int y)
{
	int line = ui_line_height(TXT_CUERPO);
	int title_line = ui_line_height(TXT_CABEZA);
	int lines = detail_has_album ? 3 : 2;
	ui_rect panel = { SP_3, y, top_width(),
	                  2 * SP_3 + title_line + lines * (line + SP_1) };
	ui_panel(panel);
	int x = panel.x + SP_3;
	int cy = panel.y + SP_3 + title_line / 2;
	ui_text(&detail_title, x, cy, TXT_CABEZA, COL_TEXTO);
	cy += title_line / 2 + SP_1 + line / 2;
	ui_text(&detail_artist, x, cy, TXT_CUERPO, COL_TEXTO_2);
	if (detail_has_album) {
		cy += line + SP_1;
		ui_text(&detail_album, x, cy, TXT_CUERPO, COL_TEXTO_2);
	}
	cy += line + SP_1;
	ui_text(&detail_duration, x, cy, TXT_CUERPO, COL_TEXTO_2);
}

void view_results_draw_top(void)
{
	ensure_parsed();
	int title_line = ui_line_height(TXT_CABEZA);
	ui_text(&query_title_text, SP_3, SP_3 + title_line / 2, TXT_CABEZA,
	        COL_TEXTO);
	int below = SP_3 + title_line + SP_2;

	switch (mode) {
	case MODE_SEARCHING: {
		/* A waiting screen: indicator and status, centred (7.2). */
		int cy = SCREEN_H / 2;
		ui_loader(SCREEN_TOP_W / 2, cy, loader_frames / LOADER_STEP);
		ui_text_centered(&status_text, SCREEN_TOP_W / 2,
		                 cy + LOADER_BAR_H + SP_4, TXT_CUERPO, COL_TEXTO);
		break;
	}
	case MODE_LIST:
		draw_detail(below);
		break;
	case MODE_ERROR:
		/* The technical detail, for reporting the problem. */
		ui_text_wrapped(&error_detail_text, SP_3, below, TXT_CUERPO,
		                COL_TEXTO_2, top_width());
		break;
	case MODE_EMPTY:
		break;
	}
}
