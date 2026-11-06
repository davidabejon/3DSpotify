/* The home view: what the app shows once the session is ready. One primary
 * action, search, plus a secondary button that runs the last search again
 * without the keyboard. Drawn with the Bisagra system (docs/STYLES.md).
 *
 * Input follows STYLES.md 6: every press shows the pressed state for at
 * least ANIM_PRESS_MIN frames, touch acts when the stylus is lifted inside
 * the control, and A inverts its glyph in the button bar. Actions that take
 * the screens over (the keyboard) are returned once the pressed state has
 * been seen, so the feedback is never skipped. The tab bar (tabbar.h) sits
 * where the header was and is drawn by the caller.
 *
 * Parsed text is cached and only rebuilt when the query or the language
 * changes; GPU frames are only needed while something animates. Main
 * thread only, like everything that touches citro2d. */
#ifndef VIEW_HOME_H
#define VIEW_HOME_H

#include <3ds.h>

#include <stdbool.h>

typedef enum {
	HOME_ACTION_NONE,
	/* Open the keyboard for a new search. */
	HOME_ACTION_SEARCH,
	/* Run the last search again, skipping the keyboard. */
	HOME_ACTION_REPEAT,
	/* SELECT: switch the language. The view shows the bubble with the new
	 * language's name by itself. */
	HOME_ACTION_LANGUAGE
} home_action;

/* After ui_init and icons_init. */
bool view_home_init(void);
void view_home_exit(void);

/* The last search ("" for none), shown on the repeat button. Call on
 * entering the view (switching to its tab too): it resets the animations
 * and forces a repaint. */
void view_home_enter(const char *last_query);

/* Once per frame, after hidScanInput, with that frame's new key presses.
 * Handles A, SELECT, the D-pad and touch; START and the tabs stay with
 * the caller. */
home_action view_home_update(u32 down);

/* True when a GPU frame is needed: the content, focus, language or an
 * animation changed since the last draw. */
bool view_home_dirty(void);

/* Draw into the current scene (the bottom screen; the top one is top.h's). */
void view_home_draw_bottom(void);

#endif
