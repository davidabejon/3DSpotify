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

static inline ui_rect list_hints_rect(void)
{
	ui_rect r = { 0, SCREEN_H - BAR_H, SCREEN_BOTTOM_W, BAR_H };
	return r;
}

/* The row in screen slot `slot` (0..RESULTS_PER_PAGE-1). */
static inline ui_rect list_row_rect(int slot)
{
	ui_rect r = { SP_2, list_top() + SP_1 + slot * (ROW_H + SP_1),
	              SCREEN_BOTTOM_W - 2 * SP_2, ROW_H };
	return r;
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
static inline int list_max_scroll(int count)
{
	return count > RESULTS_PER_PAGE
	       ? (count - RESULTS_PER_PAGE) * LIST_PITCH : 0;
}

/* Row `index` of the list scrolled by `scroll` px. It may lie partly or
 * wholly outside the list area; the bars are drawn over it. */
static inline ui_rect list_row_at(int index, int scroll)
{
	ui_rect r = list_row_rect(0);
	r.y += index * LIST_PITCH - scroll;
	return r;
}

/* Whether (x, y) is inside the list area, between the bars: where a drag
 * may start. */
static inline bool list_area_contains(int x, int y)
{
	(void)x;
	return y >= list_top() && y < SCREEN_H - BAR_H;
}

/* The track under (x, y), or -1: inside the list area, rows only, not the
 * gaps between them. */
static inline int list_hit_row(int x, int y, int scroll, int count)
{
	if (y < list_top() || y >= SCREEN_H - BAR_H)
		return -1;
	int offset = y - list_row_rect(0).y + scroll;
	if (offset < 0)
		return -1;
	int index = offset / LIST_PITCH;
	if (index >= count)
		return -1;
	return ui_rect_contains(list_row_at(index, scroll), x, y) ? index : -1;
}

/* The whole row nearest to `scroll`. */
static inline int list_snap(int scroll, int count)
{
	int first = (scroll + LIST_PITCH / 2) / LIST_PITCH;
	int max_first = list_max_scroll(count) / LIST_PITCH;
	if (first > max_first)
		first = max_first;
	return first < 0 ? 0 : first;
}

/* The rows wholly inside the list area at `scroll`. With the list resting
 * on whole rows that is RESULTS_PER_PAGE of them; cut at both ends, one
 * fewer. */
static inline void list_full_rows(int scroll, int count, int *from, int *to)
{
	int top = list_top();
	int bottom = SCREEN_H - BAR_H;
	int r0 = list_row_rect(0).y;
	/* Row i is whole when r0 + i*P - scroll >= top and its bottom edge
	 * stays above `bottom`. */
	int low = scroll + top - r0;
	*from = low <= 0 ? 0 : (low + LIST_PITCH - 1) / LIST_PITCH;
	*to = (bottom - ROW_H - r0 + scroll) / LIST_PITCH;
	if (*to > count - 1)
		*to = count - 1;
}

/* Whether row `index` is wholly on screen. A drag can leave the focused
 * row cut or out of view: it keeps the focus anyway (6.9). */
static inline bool list_row_whole(int index, int scroll, int count)
{
	int from, to;
	list_full_rows(scroll, count, &from, &to);
	return index >= from && index <= to;
}

/* The buttons work on whole rows (6.6): from a list left cut by a drag,
 * the first whole row that shows `selected` with the least movement. */
static inline int list_align(int scroll, int selected, int count)
{
	return list_scroll(list_snap(scroll, count), selected, count,
	                   RESULTS_PER_PAGE);
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
static inline int list_drag_scroll(const list_drag *d, int y, int count)
{
	int scroll = d->start_scroll - (y - d->start_y);
	int max = list_max_scroll(count);
	if (scroll > max)
		scroll = max;
	return scroll < 0 ? 0 : scroll;
}

/* The scroll indicator (5.11): a track against the right edge of the
 * screen, between the bars, and a thumb sized and placed in proportion to
 * the visible part of the list. */
static inline ui_rect list_scroll_track(void)
{
	ui_rect r = { SCREEN_BOTTOM_W - SCROLL_W, list_top(), SCROLL_W,
	              SCREEN_H - list_top() - BAR_H };
	return r;
}

static inline ui_rect list_scroll_thumb(int scroll, int count)
{
	ui_rect track = list_scroll_track();
	if (count <= RESULTS_PER_PAGE)
		return track;
	int h = track.h * RESULTS_PER_PAGE / count;
	if (h < SCROLL_MIN_H)
		h = SCROLL_MIN_H;
	int max = list_max_scroll(count);
	if (scroll < 0)
		scroll = 0;
	if (scroll > max)
		scroll = max;
	int travel = track.h - h;
	ui_rect r = { track.x, track.y + (travel * scroll + max / 2) / max,
	              track.w, h };
	return r;
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

/* "m:ss" for a duration; negative durations show as 0:00. */
static inline void list_format_duration(int duration_ms, char *out,
                                        size_t outlen)
{
	int seconds = duration_ms > 0 ? duration_ms / 1000 : 0;
	snprintf(out, outlen, "%d:%02d", seconds / 60, seconds % 60);
}

#endif
