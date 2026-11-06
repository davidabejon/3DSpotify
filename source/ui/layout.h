/* Rectangles for layout and hit testing, in whole pixels (docs/STYLES.md
 * 4.3: no fractional coordinates). Pure C, no 3DS headers. */
#ifndef LAYOUT_H
#define LAYOUT_H

#include <stdbool.h>

typedef struct {
	int x, y, w, h;
} ui_rect;

static inline bool ui_rect_contains(ui_rect r, int px, int py)
{
	return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

/* Shrinks every side by `d`; a negative `d` grows it. */
static inline ui_rect ui_rect_inset(ui_rect r, int d)
{
	ui_rect out = { r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d };
	return out;
}

static inline ui_rect ui_rect_offset(ui_rect r, int dx, int dy)
{
	ui_rect out = { r.x + dx, r.y + dy, r.w, r.h };
	return out;
}

/* The smallest whole number of pixels that holds `width`. */
static inline int ui_px_ceil(float width)
{
	int whole = (int)width;
	return whole + ((float)whole < width);
}

#endif
