/* Layout, scrolling and hit testing of the results view (docs/STYLES.md
 * 5.3, 6.6 and 7.4). Pure C with no 3DS headers, tested in
 * tests/native_logic_test.c. Whole pixels throughout.
 *
 * Bottom screen, 320 x 240:
 *   header    0..24   panel: title on the left, "1-4 of 24" on the right
 *   rows     28..216  4 rows of ROW_H, SP_1 apart (and SP_1 under the
 *                     header): exactly the 192 px between the bars
 *   hints   216..240  button bar
 * The empty and error states put a message, or a notice, and one button
 * in the same space. */
#ifndef LIST_LOGIC_H
#define LIST_LOGIC_H

#include "layout.h"
#include "theme.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Rows on screen; also how far L/R jump (7.4: RESULTS_PER_PAGE). */
#define RESULTS_PER_PAGE 4

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

static inline ui_rect list_header_rect(void)
{
	ui_rect r = { 0, 0, SCREEN_BOTTOM_W, BAR_H };
	return r;
}

static inline ui_rect list_hints_rect(void)
{
	ui_rect r = { 0, SCREEN_H - BAR_H, SCREEN_BOTTOM_W, BAR_H };
	return r;
}

/* The row in screen slot `slot` (0..RESULTS_PER_PAGE-1). */
static inline ui_rect list_row_rect(int slot)
{
	ui_rect r = { SP_2, BAR_H + SP_1 + slot * (ROW_H + SP_1),
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

/* The track under (x, y), or -1: rows only, not the gaps between them. */
static inline int list_hit_row(int x, int y, int first, int count)
{
	for (int slot = 0; slot < RESULTS_PER_PAGE; slot++) {
		if (first + slot >= count)
			break;
		if (ui_rect_contains(list_row_rect(slot), x, y))
			return first + slot;
	}
	return -1;
}

/* Empty and error states: a block from the top of the list area and a
 * button SP_4 under it. The notice fits a title and three lines. */
static inline ui_rect list_notice_rect(void)
{
	ui_rect r = { SP_2, BAR_H + SP_4, SCREEN_BOTTOM_W - 2 * SP_2,
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
