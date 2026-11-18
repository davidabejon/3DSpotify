/* Layout, hit testing and focus of the home view (docs/STYLES.md 7.7).
 * Pure C with no 3DS headers, so the host tests in
 * tests/native_logic_test.c can exercise it. Every measure comes from
 * theme.h (STYLES.md 4), in whole pixels.
 *
 * Bottom screen, 320 x 240, 8 px side margins. The six recent playlists:
 *   tabs      0..32   the tab bar (tabs_logic.h)
 *   label    36..52   "Recent" (or "Your playlists")
 *   tiles    56..160  3 rows x 2 columns of tiles (grid_logic.h)
 *   more    176..208  "Show more", a secondary button, when there are more
 *   hints   216..240  button bar
 * "Show more" is the same tab in another mode, not a detail level:
 *   tiles    36..212  5 rows x 2 columns, scrolling, with the indicator
 * Loading, error and empty use the results' notice, button and bubbles
 * (list_logic.h). */
#ifndef HOME_LOGIC_H
#define HOME_LOGIC_H

#include "grid_logic.h"
#include "layout.h"
#include "list_logic.h"
#include "theme.h"

#include <stdbool.h>

/* Tiles in the first mode. */
#define HOME_RECENT_SHOWN 6

/* The six: three rows under the label. */
static inline grid_geom home_six_geom(void)
{
	grid_geom g = { 2, TAB_H + SP_1 + SP_4 + SP_1, 3 };
	return g;
}

/* "Show more": five rows filling the space between the bars. */
static inline grid_geom home_more_geom(void)
{
	grid_geom g = { 2, TAB_H + SP_1, 5 };
	return g;
}

static inline ui_rect home_label_rect(void)
{
	ui_rect r = { SP_2, TAB_H + SP_1, SCREEN_BOTTOM_W - 2 * SP_2, SP_4 };
	return r;
}

/* SP_4 under the six, the width of the grid. */
static inline ui_rect home_more_rect(void)
{
	grid_geom g = home_six_geom();
	ui_rect r = { SP_2, g.top + g.rows * GRID_PITCH - GRID_GAP + SP_4,
	              GRID_WIDTH, TOUCH_MIN };
	return r;
}

static inline ui_rect home_hints_rect(void)
{
	return list_hints_rect();
}

/* Whether "Show more" exists: there are playlists past the six. */
static inline bool home_has_more(int count)
{
	return count > HOME_RECENT_SHOWN;
}

/* Where the focus is in the first mode: on the tiles or on "Show more". */
typedef enum { HOME_FOCUS_TILES, HOME_FOCUS_MORE } home_focus;

/* The D-pad between the tiles and the button (6.6): down from a tile that
 * cannot go further down reaches "Show more" when it exists; up from the
 * button goes back to the tiles. `tile_can_move_down` is the grid's own
 * answer for the focused tile. */
static inline home_focus home_focus_vertical(home_focus focus, bool down,
                                             bool tile_can_move_down,
                                             bool has_more)
{
	if (focus == HOME_FOCUS_TILES)
		return down && !tile_can_move_down && has_more ? HOME_FOCUS_MORE
		                                               : HOME_FOCUS_TILES;
	return down ? HOME_FOCUS_MORE : HOME_FOCUS_TILES;
}

#endif
