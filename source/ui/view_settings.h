/* The Settings tab: the language, reconnecting and pairing another
 * account, which used to be SELECT, X and Y on the home view.
 *
 * Three secondary buttons, focusable with the D-pad and touchable. Input
 * follows STYLES.md 6 like the other views: pressed states for at least
 * ANIM_PRESS_MIN frames, touch acts on lifting the stylus, A inverts its
 * glyph, and the actions that take the screens over (reconnecting,
 * pairing) are returned once the pressed state has been seen. SELECT still
 * switches the language from any tab; here the language button shows it
 * pressed (6.5). Main thread only. */
#ifndef VIEW_SETTINGS_H
#define VIEW_SETTINGS_H

#include <3ds.h>

#include <stdbool.h>

typedef enum {
	SETTINGS_ACTION_NONE,
	/* The language button or SELECT: switch the language. The button
	 * shows the new one by itself. */
	SETTINGS_ACTION_LANGUAGE,
	/* Reconnect the session with the saved credential. */
	SETTINGS_ACTION_RECONNECT,
	/* Pair another account. */
	SETTINGS_ACTION_PAIR,
	/* B: back to the Home tab. */
	SETTINGS_ACTION_BACK
} settings_action;

/* After ui_init and icons_init. */
bool view_settings_init(void);
void view_settings_exit(void);

/* Switching to the tab: resets the animations and forces a repaint. */
void view_settings_enter(void);

/* Once per frame, after hidScanInput. START and the tabs stay with the
 * caller. */
settings_action view_settings_update(u32 down);

bool view_settings_dirty(void);

/* The bottom screen; the top one is top.h's. */
void view_settings_draw_bottom(void);

#endif
