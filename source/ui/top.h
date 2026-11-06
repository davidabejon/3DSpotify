/* The top screen with the tabs (feature_plan.md): the logo while no music
 * plays, else the "now playing" card (top_now.h), or the big cover on the
 * Playing tab. top_logic.h decides which. The text screens (start-up,
 * pairing, errors) keep artwork_draw.
 *
 * Per STYLES.md 9 it only asks for a frame when what it shows changes.
 * Main thread only. */
#ifndef TOP_H
#define TOP_H

#include "artwork.h"
#include "tabs_logic.h"
#include "view_player.h"

#include <stdbool.h>

/* After ui_init; false when there is no memory for the card's text. */
bool top_init(void);
void top_exit(void);

/* Once per frame with the tabs, with the same snapshot as
 * view_player_set. */
void top_set(const player_info *info);

/* Whether `tab`'s top screen needs a frame. Reads artwork_dirty only while
 * the cover is on screen, since only drawing it clears the flag. */
bool top_dirty(app_tab tab, const artwork_state *art);

void top_draw(app_tab tab, artwork_state *art);

#endif
