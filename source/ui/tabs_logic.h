/* The tab bar (docs/STYLES.md 5.12): layout, hit testing and which tab the
 * buttons move to. Pure C with no 3DS headers, tested in
 * tests/native_logic_test.c.
 *
 * Bottom screen, 320 x 240: the bar takes the top TAB_H px (the minimum
 * touch target, 4.4) across the whole width, four tabs of equal width with
 * an icon each and no label. It replaces the 24 px header of the views. */
#ifndef TABS_LOGIC_H
#define TABS_LOGIC_H

#include "layout.h"
#include "theme.h"

#include <stdbool.h>

/* In the bar's order, left to right. */
typedef enum {
	TAB_HOME,
	TAB_SEARCH,
	TAB_PLAYER,
	TAB_SETTINGS,
	TAB_COUNT
} app_tab;

static inline ui_rect tabs_bar_rect(void)
{
	ui_rect r = { 0, 0, SCREEN_BOTTOM_W, TAB_H };
	return r;
}

/* Tab `tab`: a quarter of the width, the full height of the bar. */
static inline ui_rect tabs_tab_rect(int tab)
{
	int w = SCREEN_BOTTOM_W / TAB_COUNT;
	ui_rect r = { tab * w, 0, w, TAB_H };
	return r;
}

/* The tab under (x, y), or -1 outside the bar. */
static inline int tabs_hit(int x, int y)
{
	if (!ui_rect_contains(tabs_bar_rect(), x, y))
		return -1;
	int tab = x / (SCREEN_BOTTOM_W / TAB_COUNT);
	return tab < TAB_COUNT ? tab : TAB_COUNT - 1;
}

/* The tab `delta` steps from `tab`, stopping at both ends like the lists
 * (6.6): the bar does not wrap. -1 when there is nowhere to go. */
static inline int tabs_step(int tab, int delta)
{
	if (tab < 0 || tab >= TAB_COUNT || delta == 0)
		return -1;
	int next = tab + (delta < 0 ? -1 : 1);
	return next >= 0 && next < TAB_COUNT ? next : -1;
}

/* Which way this frame's buttons move the bar: -1, 0 or +1. L/R, and
 * ZL/ZR on the New 3DS, belong to the bar alone: no view uses them. */
static inline int tabs_key_delta(bool l, bool r, bool zl, bool zr)
{
	int delta = ((r || zr) ? 1 : 0) - ((l || zl) ? 1 : 0);
	return delta;
}

#endif
