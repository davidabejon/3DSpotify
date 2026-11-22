/* The top screen with the tabs: the logo while no music plays, else the
 * "now playing" card (top_now.h), or the big cover on the Playing tab;
 * the account and the credits on Settings (top_about.h); the card of a
 * detail (top_detail.h) whenever the active tab shows one, or of the
 * focused tile or result as a preview; and, on Search's results, the strip
 * of kinds above that card (top_strip.h). With music on and a detail's
 * card, a preview or the downloads manager on screen, the mini player
 * (top_mini.h) in the bottom right corner says what plays. top_logic.h
 * decides which. While a track's download is still at 0%, a spinner
 * (spinner_logic.h) turns in the mini player's icon place when it shows,
 * else at the centre of the screen, over any of them and on any tab.
 * Before the session (start-up, pairing, errors) the top screen is
 * view_login.h's.
 *
 * Per STYLES.md 9 it only asks for a frame when what it shows changes: the
 * spinner adds one every LOADER_STEP frames while it turns, the mini
 * player one when it comes or goes and when its own look changes. Main
 * thread only. */
#ifndef TOP_H
#define TOP_H

#include "artwork.h"
#include "details.h"
#include "results_logic.h"
#include "top_about.h"
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
/* Once per frame with the tabs: the detail on top of the active tab, or
 * NULL, and the image its card shows (never the playing track's). */
void top_set_detail(const detail_doc *doc, artwork_state *art);

/* The account for Settings (top_about.h) and its picture; both stay the
 * caller's and must outlive the tabs. */
void top_set_account(const about_account *account, artwork_state *art);

/* Whether Settings shows the downloads manager, whose card then takes the
 * top screen. Once per frame with the tabs. */
void top_set_manager(bool on);

/* The strip of kinds, once per frame with the tabs: NULL whenever
 * top_shows_strip says no. Copied. */
void top_set_results(const results_strip *strip);

bool top_dirty(app_tab tab, const artwork_state *art);

void top_draw(app_tab tab, artwork_state *art);

#endif
