/* The tab bar of the bottom screen (docs/STYLES.md 5.12): Home, Search,
 * Playing and Settings, an icon each, across the top TAB_H px.
 *
 * Input follows STYLES.md 6: touching a tab shows it pressed and switches
 * on lifting the stylus inside it (6.4); L/R and ZL/ZR, used by no view,
 * switch at once and show the new tab pressed for at least
 * ANIM_PRESS_MIN frames (6.5). Main thread only. */
#ifndef TABBAR_H
#define TABBAR_H

#include "tabs_logic.h"

#include <3ds.h>

#include <stdbool.h>

/* Shows `tab` as the active one, with no press in progress. */
void tabbar_reset(app_tab tab);
void tabbar_set_active(app_tab tab);
app_tab tabbar_active(void);

/* Once per frame, after hidScanInput. Returns the tab to switch to, or -1.
 */
int tabbar_update(u32 down, u32 held, u32 up);

/* True while the stylus is on a tab: the views ignore that touch. */
bool tabbar_touching(void);

bool tabbar_dirty(void);
/* Draws the bar into the current scene (the bottom screen), over the
 * view, so dragged rows slide under it. */
void tabbar_draw(void);

#endif
