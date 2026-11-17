/* Layout, scrolling, dragging and hit testing of the results view
 * (docs/STYLES.md 5.3, 5.11, 6.6, 6.9 and 7.4). Pure C with no 3DS headers, tested in
 * tests/native_logic_test.c. Whole pixels throughout.
 *
 * Bottom screen, 320 x 240:
 *   tabs      0..32   the tab bar (tabs_logic.h); "1-4 of 24" goes to the
 *                     top screen
 *   rows     36..212  4 rows of ROW_H, SP_1 apart, with SP_1 under the
 *                     tab bar and SP_1 over the button bar: exactly the
 *                     184 px between the bars
 *   scroll  316..320  the scroll indicator, against the right edge
 *   hints   216..240  button bar
 * `scroll` is the pixel offset of the list and the first (possibly cut)
 * visible row is scroll / LIST_PITCH. A drag may leave rows cut at either
 * end; the buttons bring the list back to whole rows (6.6, 6.9).
 * The empty and error states put a message, or a notice, and one button
 * in the same space. */
#ifndef LIST_LOGIC_H
#define LIST_LOGIC_H

#include "layout.h"
#include "theme.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Rows on screen; also how far left/right jump (7.4: RESULTS_PER_PAGE). */
#define RESULTS_PER_PAGE 4
/* From one row to the next. */
#define LIST_PITCH (ROW_H + SP_1)

/* Clamps `selected` into 0..count-1 (0 for an empty list). */
static inline int list_clamp(int selected, int count)
{
	if (count <= 0 || selected < 0)
		return 0;
	return selected >= count ? count - 1 : selected;
}

/* The first visible row once `selected` has to be visible: the list moves
 * just enough (continuous scrolling, 7.4), never past either end. */
static inline int list_scroll(int first, int selected, int count,
                              int visible)
{
	int max_first = count > visible ? count - visible : 0;
	if (selected < first)
		first = selected;
	if (selected >= first + visible)
		first = selected - visible + 1;
	if (first > max_first)
		first = max_first;
	if (first < 0)
		first = 0;
	return first;
}

/* Moves the selection by `delta`, stopping at the ends (the list does not
 * wrap, 6.6). Sets `*bumped` when it was already at the end it moved
 * towards, so the view can show the edge nudge. */
static inline int list_move(int selected, int delta, int count,
                            bool *bumped)
{
	*bumped = false;
	if (count <= 0 || delta == 0)
		return 0;
	int next = selected + delta;
	if (next < 0)
		next = 0;
	if (next > count - 1)
		next = count - 1;
	*bumped = next == selected;
	return next;
}

/* "from-to of count", 1-based; 0-0 for an empty list. */
static inline void list_range(int first, int count, int visible, int *from,
                              int *to)
{
	if (count <= 0) {
		*from = *to = 0;
		return;
	}
	*from = first + 1;
	*to = first + visible < count ? first + visible : count;
}

/* Where the list area starts: under the tab bar. */
static inline int list_top(void)
{
	return TAB_H;
}

/* The area a list lives in: from `top` to the button bar, `rows` whole
 * rows of it. The results, and most lists, have it all (list_frame_full);
 * an album or a playlist gives the top of it to a button
 * (list_frame_under). The functions without a frame are the full one's. */
typedef struct {
	int top;
	int rows;
} list_frame;

static inline list_frame list_frame_full(void)
{
	list_frame f = { TAB_H, RESULTS_PER_PAGE };
	return f;
}

/* Under a button of TOUCH_MIN, SP_1 below the tab bar and SP_1 above the
 * list area (whose first row is SP_1 lower still): the rows that fit whole
 * above the button bar. */
static inline list_frame list_frame_under_button(void)
{
	list_frame f;
	f.top = TAB_H + SP_1 + TOUCH_MIN + SP_1;
	f.rows = (SCREEN_H - BAR_H - f.top) / LIST_PITCH;
	return f;
}

/* The button over such a list. */
static inline ui_rect list_header_button_rect(void)
{
	ui_rect r = { SP_2, TAB_H + SP_1, SCREEN_BOTTOM_W - 2 * SP_2, TOUCH_MIN };
	return r;
}

static inline ui_rect list_hints_rect(void)
{
	ui_rect r = { 0, SCREEN_H - BAR_H, SCREEN_BOTTOM_W, BAR_H };
	return r;
}

/* The row in screen slot `slot` (0..f.rows-1). */
static inline ui_rect list_row_rect_in(list_frame f, int slot)
{
	ui_rect r = { SP_2, f.top + SP_1 + slot * (ROW_H + SP_1),
	              SCREEN_BOTTOM_W - 2 * SP_2, ROW_H };
	return r;
}

static inline ui_rect list_row_rect(int slot)
{
	return list_row_rect_in(list_frame_full(), slot);
}

/* The thumbnail square of a row, SP_1 from its left, top and bottom. */
static inline ui_rect list_thumb_rect(ui_rect row)
{
	ui_rect r = { row.x + SP_1, row.y + SP_1, THUMB_SIZE, THUMB_SIZE };
	return r;
}

/* Where a row's text starts: SP_2 right of the thumbnail. */
static inline int list_text_x(ui_rect row)
{
	return row.x + SP_1 + THUMB_SIZE + SP_2;
}

/* The furthest the list scrolls: its last row at the bottom. */
static inline int list_max_scroll_in(list_frame f, int count)
{
	return count > f.rows ? (count - f.rows) * LIST_PITCH : 0;
}

static inline int list_max_scroll(int count)
{
	return list_max_scroll_in(list_frame_full(), count);
}

/* Row `index` of the list scrolled by `scroll` px. It may lie partly or
 * wholly outside the list area; the bars are drawn over it. */
static inline ui_rect list_row_at_in(list_frame f, int index, int scroll)
{
	ui_rect r = list_row_rect_in(f, 0);
	r.y += index * LIST_PITCH - scroll;
	return r;
}

static inline ui_rect list_row_at(int index, int scroll)
{
	return list_row_at_in(list_frame_full(), index, scroll);
}

/* Whether (x, y) is inside the list area, between the bars: where a drag
 * may start. */
static inline bool list_area_contains_in(list_frame f, int x, int y)
{
	(void)x;
	return y >= f.top && y < SCREEN_H - BAR_H;
}

static inline bool list_area_contains(int x, int y)
{
	return list_area_contains_in(list_frame_full(), x, y);
}

/* The track under (x, y), or -1: inside the list area, rows only, not the
 * gaps between them. */
static inline int list_hit_row_in(list_frame f, int x, int y, int scroll,
                                  int count)
{
	if (y < f.top || y >= SCREEN_H - BAR_H)
		return -1;
	int offset = y - list_row_rect_in(f, 0).y + scroll;
	if (offset < 0)
		return -1;
	int index = offset / LIST_PITCH;
	if (index >= count)
		return -1;
	return ui_rect_contains(list_row_at_in(f, index, scroll), x, y) ? index
	                                                                : -1;
}

static inline int list_hit_row(int x, int y, int scroll, int count)
{
	return list_hit_row_in(list_frame_full(), x, y, scroll, count);
}

/* The whole row nearest to `scroll`. */
static inline int list_snap_in(list_frame f, int scroll, int count)
{
	int first = (scroll + LIST_PITCH / 2) / LIST_PITCH;
	int max_first = list_max_scroll_in(f, count) / LIST_PITCH;
	if (first > max_first)
		first = max_first;
	return first < 0 ? 0 : first;
}

static inline int list_snap(int scroll, int count)
{
	return list_snap_in(list_frame_full(), scroll, count);
}

/* The rows wholly inside the list area at `scroll`. With the list resting
 * on whole rows that is f.rows of them; cut at both ends, one fewer. */
static inline void list_full_rows_in(list_frame f, int scroll, int count,
                                     int *from, int *to)
{
	int top = f.top;
	int bottom = SCREEN_H - BAR_H;
	int r0 = list_row_rect_in(f, 0).y;
	/* Row i is whole when r0 + i*P - scroll >= top and its bottom edge
	 * stays above `bottom`. */
	int low = scroll + top - r0;
	*from = low <= 0 ? 0 : (low + LIST_PITCH - 1) / LIST_PITCH;
	*to = (bottom - ROW_H - r0 + scroll) / LIST_PITCH;
	if (*to > count - 1)
		*to = count - 1;
}

static inline void list_full_rows(int scroll, int count, int *from, int *to)
{
	list_full_rows_in(list_frame_full(), scroll, count, from, to);
}

/* Whether row `index` is wholly on screen. A drag can leave the focused
 * row cut or out of view: it keeps the focus anyway (6.9). */
static inline bool list_row_whole_in(list_frame f, int index, int scroll,
                                     int count)
{
	int from, to;
	list_full_rows_in(f, scroll, count, &from, &to);
	return index >= from && index <= to;
}

static inline bool list_row_whole(int index, int scroll, int count)
{
	return list_row_whole_in(list_frame_full(), index, scroll, count);
}

/* The buttons work on whole rows (6.6): from a list left cut by a drag,
 * the first whole row that shows `selected` with the least movement. */
static inline int list_align_in(list_frame f, int scroll, int selected,
                                int count)
{
	return list_scroll(list_snap_in(f, scroll, count), selected, count,
	                   f.rows);
}

static inline int list_align(int scroll, int selected, int count)
{
	return list_align_in(list_frame_full(), scroll, selected, count);
}

/* Dragging the list (6.9): a touch becomes a drag once the stylus has moved
 * DRAG_START px vertically; from then on the list follows it pixel for
 * pixel, without passing either end. */
typedef struct {
	bool down;          /* the stylus is on the list */
	bool dragging;      /* ... and has moved far enough */
	int start_y;
	int start_scroll;
} list_drag;

static inline void list_drag_begin(list_drag *d, int y, int scroll)
{
	d->down = true;
	d->dragging = false;
	d->start_y = y;
	d->start_scroll = scroll;
}

/* True once the touch is a drag. */
static inline bool list_drag_update(list_drag *d, int y)
{
	if (d->down && !d->dragging) {
		int moved = y - d->start_y;
		if (moved >= DRAG_START || moved <= -DRAG_START)
			d->dragging = true;
	}
	return d->dragging;
}

/* The scroll for the stylus at `y`: the list moves with it. */
static inline int list_drag_scroll_in(list_frame f, const list_drag *d, int y,
                                      int count)
{
	int scroll = d->start_scroll - (y - d->start_y);
	int max = list_max_scroll_in(f, count);
	if (scroll > max)
		scroll = max;
	return scroll < 0 ? 0 : scroll;
}

static inline int list_drag_scroll(const list_drag *d, int y, int count)
{
	return list_drag_scroll_in(list_frame_full(), d, y, count);
}

/* The scroll indicator (5.11): a track against the right edge of the
 * screen, between the bars, and a thumb sized and placed in proportion to
 * the visible part of the list. */
static inline ui_rect list_scroll_track_in(list_frame f)
{
	ui_rect r = { SCREEN_BOTTOM_W - SCROLL_W, f.top, SCROLL_W,
	              SCREEN_H - f.top - BAR_H };
	return r;
}

static inline ui_rect list_scroll_track(void)
{
	return list_scroll_track_in(list_frame_full());
}

static inline ui_rect list_scroll_thumb_in(list_frame f, int scroll,
                                           int count)
{
	ui_rect track = list_scroll_track_in(f);
	if (count <= f.rows)
		return track;
	int h = track.h * f.rows / count;
	if (h < SCROLL_MIN_H)
		h = SCROLL_MIN_H;
	int max = list_max_scroll_in(f, count);
	if (scroll < 0)
		scroll = 0;
	if (scroll > max)
		scroll = max;
	int travel = track.h - h;
	ui_rect r = { track.x, track.y + (travel * scroll + max / 2) / max,
	              track.w, h };
	return r;
}

static inline ui_rect list_scroll_thumb(int scroll, int count)
{
	return list_scroll_thumb_in(list_frame_full(), scroll, count);
}

/* Empty and error states: a block from the top of the list area and a
 * button SP_4 under it. The notice fits a title and three lines. */
static inline ui_rect list_notice_rect(void)
{
	ui_rect r = { SP_2, list_top() + SP_4, SCREEN_BOTTOM_W - 2 * SP_2,
	              4 * SP_6 };
	return r;
}

/* A button hugging `content_width` plus SP_3 each side, under the notice;
 * centred when `centered` (a single-message screen, 3.3). */
static inline ui_rect list_action_rect(float content_width, bool centered)
{
	ui_rect notice = list_notice_rect();
	int w = ui_px_ceil(content_width) + 2 * SP_3;
	if (w < TOUCH_MIN)
		w = TOUCH_MIN;
	if (w > notice.w)
		w = notice.w;
	ui_rect r = { centered ? (SCREEN_BOTTOM_W - w) / 2 : SP_2,
	              notice.y + notice.h + SP_4, w, TOUCH_MIN };
	return r;
}

/* Nothing searched yet with recent searches to suggest: the search button
 * centred at the top, then a label and one row per recent query, all
 * clear of the button bar. */
static inline ui_rect list_idle_button_rect(float content_width)
{
	ui_rect r = list_action_rect(content_width, true);
	r.y = list_top() + SP_2;
	return r;
}

/* The vertical centre of the "Recent searches" label. */
static inline int list_recent_label_cy(void)
{
	return list_top() + SP_2 + TOUCH_MIN + SP_2 + SP_2;
}

/* Recent query `i` (0 for the newest). */
static inline ui_rect list_recent_rect(int i)
{
	ui_rect r = { SP_2, list_recent_label_cy() + SP_2 + SP_1 +
	                    i * (TOUCH_MIN + SP_1),
	              SCREEN_BOTTOM_W - 2 * SP_2, TOUCH_MIN };
	return r;
}

/* Centred, SP_2 above the button bar (5.9). */
static inline ui_rect list_bubble_rect(float content_width)
{
	int w = ui_px_ceil(content_width) + 2 * SP_3;
	if (w > SCREEN_BOTTOM_W - 2 * SP_2)
		w = SCREEN_BOTTOM_W - 2 * SP_2;
	ui_rect r = { (SCREEN_BOTTOM_W - w) / 2,
	              SCREEN_H - BAR_H - SP_2 - BUBBLE_H, w, BUBBLE_H };
	return r;
}

/* Two bubbles may show at once (5.9): the search's and the language's.
 * Slot 0 is list_bubble_rect; slot 1 stacks SP_2 above it, its shadow
 * included. */
static inline ui_rect list_bubble_slot_rect(float content_width, int slot)
{
	ui_rect r = list_bubble_rect(content_width);
	r.y -= slot * (BUBBLE_H + SHADOW_OFF + SP_2);
	return r;
}

/* Long lists keep only a window of `window` rows ready (their text and
 * covers), from `base`. The window moves when the rows on screen, from
 * `first` (the peek row of a drag included), come within LIST_WINDOW_MARGIN
 * of either end of it, and it then centres on them; a list that fits is
 * always the window from 0. Returns the (possibly unchanged) base. */
#define LIST_WINDOW_MARGIN 2

static inline int list_window_base(int base, int first, int count,
                                   int window)
{
	if (count <= window)
		return 0;
	int shown = RESULTS_PER_PAGE + 1;
	int max_base = count - window;
	if (base > max_base)
		base = max_base;
	if (base < 0)
		base = 0;
	bool low = base > 0 && first < base + LIST_WINDOW_MARGIN;
	bool high = base < max_base &&
	            first + shown > base + window - LIST_WINDOW_MARGIN;
	if (!low && !high && first >= base && first + shown <= base + window)
		return base;
	int centred = first - (window - shown) / 2;
	return centred < 0 ? 0 : centred > max_base ? max_base : centred;
}

/* "m:ss" for a duration; negative durations show as 0:00. */
static inline void list_format_duration(int duration_ms, char *out,
                                        size_t outlen)
{
	int seconds = duration_ms > 0 ? duration_ms / 1000 : 0;
	snprintf(out, outlen, "%d:%02d", seconds / 60, seconds % 60);
}

#endif
