/* Layout, hit testing and focus of the home view. Pure C with no 3DS
 * headers, so the host tests in tests/native_logic_test.c can exercise it.
 * Every measure comes from theme.h (docs/STYLES.md 4), in whole pixels.
 *
 * Bottom screen, 320 x 240, 8 px side margins:
 *   header    0..24   panel with the view title
 *   search   40..72   the primary button (A, or tap), hugging its content
 *   label    88..104  "Last search", only when there is one
 *   repeat  108..140  secondary button that runs the last search again
 *   bubble  180..208  transient message (SELECT), centred
 *   hints   216..240  button bar */
#ifndef HOME_LOGIC_H
#define HOME_LOGIC_H

#include "layout.h"
#include "theme.h"

#include <stdbool.h>

typedef enum {
	HOME_ITEM_NONE = -1,
	/* Focusable with the D-pad and touchable, in this order. */
	HOME_ITEM_SEARCH,
	HOME_ITEM_REPEAT
} home_item;

static inline int home_content_width(void)
{
	return SCREEN_BOTTOM_W - 2 * SP_2;
}

/* A button hugs its content plus SP_3 each side, at least a square touch
 * target and at most the content width. */
static inline int home_button_width(float content_width)
{
	int w = ui_px_ceil(content_width) + 2 * SP_3;
	if (w < TOUCH_MIN)
		w = TOUCH_MIN;
	if (w > home_content_width())
		w = home_content_width();
	return w;
}

static inline ui_rect home_header_rect(void)
{
	ui_rect r = { 0, 0, SCREEN_BOTTOM_W, BAR_H };
	return r;
}

static inline ui_rect home_hints_rect(void)
{
	ui_rect r = { 0, SCREEN_H - BAR_H, SCREEN_BOTTOM_W, BAR_H };
	return r;
}

/* `content_width`: icon, gap and label of the button. */
static inline ui_rect home_search_rect(float content_width)
{
	ui_rect r = { SP_2, BAR_H + SP_4, home_button_width(content_width),
	              TOUCH_MIN };
	return r;
}

static inline ui_rect home_label_rect(void)
{
	ui_rect search = home_search_rect(0);
	ui_rect r = { SP_2, search.y + search.h + SP_4, home_content_width(),
	              SP_4 };
	return r;
}

static inline ui_rect home_repeat_rect(float text_width)
{
	ui_rect label = home_label_rect();
	ui_rect r = { SP_2, label.y + label.h + SP_1,
	              home_button_width(text_width), TOUCH_MIN };
	return r;
}

/* The widest last-search text the repeat button can show. */
static inline int home_repeat_max_text_width(void)
{
	return home_content_width() - 2 * SP_3;
}

/* Centred, SP_2 above the button bar. */
static inline ui_rect home_bubble_rect(float content_width)
{
	int w = ui_px_ceil(content_width) + 2 * SP_3;
	if (w > home_content_width())
		w = home_content_width();
	ui_rect r = { (SCREEN_BOTTOM_W - w) / 2,
	              SCREEN_H - BAR_H - SP_2 - BUBBLE_H, w, BUBBLE_H };
	return r;
}

/* What a touch at (x, y) lands on. The repeat button only exists with a
 * last search. */
static inline home_item home_hit_test(int x, int y,
                                      float search_content_width,
                                      bool has_repeat,
                                      float repeat_text_width)
{
	if (ui_rect_contains(home_search_rect(search_content_width), x, y))
		return HOME_ITEM_SEARCH;
	if (has_repeat &&
	    ui_rect_contains(home_repeat_rect(repeat_text_width), x, y))
		return HOME_ITEM_REPEAT;
	return HOME_ITEM_NONE;
}

/* D-pad focus: `delta` -1 is up, +1 down. Stops at the ends; anything that
 * is not focusable (or a repeat button that went away) falls back to the
 * search button. */
static inline home_item home_focus_move(home_item focus, int delta,
                                        bool has_repeat)
{
	int last = has_repeat ? HOME_ITEM_REPEAT : HOME_ITEM_SEARCH;
	int next = (int)focus;
	if (next < HOME_ITEM_SEARCH || next > last)
		next = HOME_ITEM_SEARCH;
	next += delta;
	if (next < HOME_ITEM_SEARCH)
		next = HOME_ITEM_SEARCH;
	if (next > last)
		next = last;
	return (home_item)next;
}

#endif
