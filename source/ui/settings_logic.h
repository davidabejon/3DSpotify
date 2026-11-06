/* Layout, hit testing and focus of the Settings tab. Pure C with no 3DS
 * headers, tested in tests/native_logic_test.c. Every measure comes from
 * theme.h (docs/STYLES.md 4), in whole pixels.
 *
 * Bottom screen, 320 x 240, 8 px side margins:
 *   tabs        0..32   the tab bar (tabs_logic.h)
 *   language   48..80   secondary button: the current language
 *   reconnect  96..128  secondary button
 *   pair      144..176  secondary button: pair another account
 *   hints     216..240  button bar
 * One button per row, the content width each, SP_4 apart: room for the
 * focus border and the hard shadow between them (4.4). */
#ifndef SETTINGS_LOGIC_H
#define SETTINGS_LOGIC_H

#include "layout.h"
#include "theme.h"

typedef enum {
	SETTINGS_ITEM_NONE = -1,
	/* Focusable with the D-pad and touchable, top to bottom. */
	SETTINGS_ITEM_LANGUAGE,
	SETTINGS_ITEM_RECONNECT,
	SETTINGS_ITEM_PAIR,
	SETTINGS_ITEM_COUNT
} settings_item;

static inline ui_rect settings_item_rect(int item)
{
	ui_rect r = { SP_2, TAB_H + SP_4 + item * (TOUCH_MIN + SP_4),
	              SCREEN_BOTTOM_W - 2 * SP_2, TOUCH_MIN };
	return r;
}

static inline ui_rect settings_hints_rect(void)
{
	ui_rect r = { 0, SCREEN_H - BAR_H, SCREEN_BOTTOM_W, BAR_H };
	return r;
}

/* The button under (x, y), or SETTINGS_ITEM_NONE. */
static inline settings_item settings_hit_test(int x, int y)
{
	for (int i = 0; i < SETTINGS_ITEM_COUNT; i++)
		if (ui_rect_contains(settings_item_rect(i), x, y))
			return (settings_item)i;
	return SETTINGS_ITEM_NONE;
}

/* D-pad focus: `delta` -1 is up, +1 down, stopping at the ends. Anything
 * that is not an item falls back to the first one. */
static inline settings_item settings_focus_move(settings_item focus,
                                                int delta)
{
	int next = (int)focus;
	if (next < 0 || next >= SETTINGS_ITEM_COUNT)
		next = 0;
	next += delta;
	if (next < 0)
		next = 0;
	if (next >= SETTINGS_ITEM_COUNT)
		next = SETTINGS_ITEM_COUNT - 1;
	return (settings_item)next;
}

#endif
