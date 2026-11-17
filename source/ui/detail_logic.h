/* Layout, hit testing and focus of the track detail (docs/plan_vistas_
 * detalle.md 4.1). Pure C with no 3DS headers, tested in
 * tests/native_logic_test.c.
 *
 * The same geometry as the Settings tab (STYLES.md 7.8), so the two screens
 * of buttons line up:
 *   tabs        0..32    the tab bar (tabs_logic.h)
 *   play       44..76    primary button
 *   download   88..120   secondary button: the track's download, as it is
 *   artist    132..164   secondary button: "Artist: <name>"
 *   album     176..208   secondary button: "Album: <name>"
 *   hints     216..240   button bar
 * Artist and album are disabled (6.1) when the track does not name them,
 * the download while it is queued or under way: the D-pad skips them and
 * touching them does nothing. */
#ifndef DETAIL_LOGIC_H
#define DETAIL_LOGIC_H

#include "settings_logic.h"

#include <stdbool.h>

typedef enum {
	DETAIL_BUTTON_NONE = -1,
	DETAIL_BUTTON_PLAY,
	DETAIL_BUTTON_DOWNLOAD,
	DETAIL_BUTTON_ARTIST,
	DETAIL_BUTTON_ALBUM,
	DETAIL_BUTTON_COUNT
} detail_button;

/* Whether a paged list wants its next page: the last row on screen
 * (`first` + `visible` - 1) is within `ahead` rows of the last loaded, or
 * the focus is. */
static inline bool detail_wants_more(int first, int selected, int visible,
                                     int count, int ahead)
{
	int last_shown = first + visible - 1;
	if (selected > last_shown)
		last_shown = selected;
	return count <= 0 || last_shown >= count - 1 - ahead;
}

static inline ui_rect detail_button_rect(int button)
{
	return settings_item_rect(button);
}

static inline ui_rect detail_hints_rect(void)
{
	return settings_hints_rect();
}

/* The enabled button under (x, y), or DETAIL_BUTTON_NONE. */
static inline detail_button detail_hit_test(int x, int y,
                                            const bool enabled[DETAIL_BUTTON_COUNT])
{
	for (int i = 0; i < DETAIL_BUTTON_COUNT; i++)
		if (enabled[i] && ui_rect_contains(detail_button_rect(i), x, y))
			return (detail_button)i;
	return DETAIL_BUTTON_NONE;
}

/* D-pad focus: `delta` -1 is up, +1 down, to the next enabled button that
 * way, staying put when there is none. A focus that is not an enabled
 * button falls back to Play, which always is. */
static inline detail_button detail_focus_move(detail_button focus, int delta,
                                              const bool enabled[DETAIL_BUTTON_COUNT])
{
	if (focus < 0 || focus >= DETAIL_BUTTON_COUNT || !enabled[focus])
		return DETAIL_BUTTON_PLAY;
	if (delta == 0)
		return focus;
	for (int i = (int)focus + delta; i >= 0 && i < DETAIL_BUTTON_COUNT;
	     i += delta)
		if (enabled[i])
			return (detail_button)i;
	return focus;
}

/* The D-pad between an album's or playlist's list and the button over it
 * (its download): up from the first row reaches the button, down from the
 * button goes back to the list, where the focus was. Returns whether the
 * button has the focus after `delta` (-1 up, +1 down, 0 to check), with
 * `selected` the focused row. A disabled button (6.1) is never focused:
 * the focus goes back to the list when it becomes one. */
static inline bool detail_header_focus(bool on_header, int selected,
                                       int delta, bool header_enabled)
{
	if (!header_enabled)
		return false;
	if (on_header)
		return delta <= 0;
	return delta < 0 && selected <= 0;
}

#endif
