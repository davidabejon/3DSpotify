/* The home view (docs/STYLES.md 7.7): the account's recent playlists as
 * tiles, six at first and up to HOME_MAX_PLAYLISTS behind "Show more".
 * The playlists come from home_feed.h, which the view reads by itself;
 * while they load it shows skeleton tiles, on failure a notice with
 * "Retry", and with none a message and "Search songs". Y opens the search
 * keyboard from anywhere in the tab.
 *
 * Input follows STYLES.md 6: every press shows the pressed state for at
 * least ANIM_PRESS_MIN frames, touch acts when the stylus is lifted inside
 * the control, and buttons invert their glyph in the button bar. Actions
 * are returned once the pressed state has been seen, so the feedback is
 * never skipped. The tab bar (tabbar.h) is drawn by the caller.
 *
 * Parsed text is cached and only rebuilt when the content or the language
 * changes; GPU frames are only needed while something changes. Main
 * thread only, like everything that touches citro2d. */
#ifndef VIEW_HOME_H
#define VIEW_HOME_H

#include "details.h"

#include <3ds.h>

#include <stdbool.h>

typedef enum {
	HOME_ACTION_NONE,
	/* Open the playlist view_home_selected_uri() names. */
	HOME_ACTION_OPEN,
	/* Open the keyboard for a new search (Y, or the empty state's
	 * button). */
	HOME_ACTION_SEARCH,
	/* Load the playlists again after a failure. */
	HOME_ACTION_RETRY,
	/* SELECT: switch the language. The view shows the bubble with the new
	 * language's name by itself. */
	HOME_ACTION_LANGUAGE
} home_action;

/* After ui_init and icons_init. */
bool view_home_init(void);
void view_home_exit(void);

/* Entering the view (switching to its tab too): no press in progress, and
 * a repaint. "Show more" and the focus are kept. */
void view_home_enter(void);

/* Once per frame, after hidScanInput, with that frame's new key presses.
 * Handles A, B, Y, SELECT, the D-pad and touch; START and the tabs stay
 * with the caller. */
home_action view_home_update(u32 down);

/* The playlist to open after HOME_ACTION_OPEN, and the track to open it
 * at (where it was left; "" for the top). */
const char *view_home_selected_uri(void);
const char *view_home_selected_resume(void);

/* The card the top screen shows for the focused tile (STYLES.md 7.7), or
 * NULL when no tile has the focus. Its cover is only named once the focus
 * has rested HOME_PREVIEW_DELAY frames, so running across the grid
 * downloads nothing. */
#define HOME_PREVIEW_DELAY 18
const detail_doc *view_home_preview(void);

/* True when a GPU frame is needed: the content, focus, language or an
 * animation changed since the last draw. */
bool view_home_dirty(void);

/* Draw into the current scene (the bottom screen; the top one is top.h's). */
void view_home_draw_bottom(void);

#endif
