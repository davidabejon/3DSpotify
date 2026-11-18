/* What the top screen shows with the tabs (STYLES.md 7.4, 7.5, 7.8,
 * 7.12): the logo while no music plays, else the "now playing" card, or
 * the big cover on the Playing tab; on Settings, always the account and
 * the credits; the card of a detail, or of a preview, over any of them;
 * and on Search's results, the strip of kinds above that card. Pure C
 * with no 3DS headers, tested in tests/native_logic_test.c. */
#ifndef TOP_LOGIC_H
#define TOP_LOGIC_H

#include "player_logic.h"
#include "tabs_logic.h"

#include <stdbool.h>

typedef enum {
	TOP_LOGO,
	TOP_NOW_CARD,
	TOP_ART_BIG,
	/* The detail on top of the active tab, or the focused tile or result
	 * previewed: its card (STYLES.md 7.12). */
	TOP_DETAIL,
	/* Settings: the account, the credits and the licence over the
	 * darkened logo (top_about.h, STYLES.md 7.8). */
	TOP_ABOUT,
	/* Settings with the downloads manager open: its card
	 * (view_downloads.h). */
	TOP_DOWNLOADS
} top_mode;

/* Music counts as playing while loading, playing (paused too) and once
 * finished, which keeps the last cover and a full bar (7.6). Idle and
 * failed show the logo. */
static inline bool top_music_on(player_phase phase)
{
	return phase == PLAYER_PHASE_LOADING ||
	       phase == PLAYER_PHASE_PLAYING ||
	       phase == PLAYER_PHASE_FINISHED;
}

/* `detail_on`: the active tab shows a detail, which takes the top screen
 * whatever plays. */
static inline top_mode top_mode_for(app_tab tab, player_phase phase,
                                    bool detail_on)
{
	if (detail_on)
		return TOP_DETAIL;
	if (tab == TAB_SETTINGS)
		return TOP_ABOUT;
	if (!top_music_on(phase))
		return TOP_LOGO;
	return tab == TAB_PLAYER ? TOP_ART_BIG : TOP_NOW_CARD;
}

/* The downloads manager is Settings in another mode: its top screen is
 * the manager's card instead of the account's. */
static inline top_mode top_mode_with_manager(top_mode mode, bool manager)
{
	return mode == TOP_ABOUT && manager ? TOP_DOWNLOADS : mode;
}

/* The strip of kinds (STYLES.md 5.17): on Search, with the list of
 * results on screen and no detail open over it. Searching, failed, empty
 * or idle, the top screen is the usual one. */
static inline bool top_shows_strip(app_tab tab, bool detail_open,
                                   bool results_list)
{
	return tab == TAB_SEARCH && !detail_open && results_list;
}

/* The spinner, centred over whatever the top screen shows, on any tab:
 * while a track loads or plays and nothing of its audio has downloaded
 * yet. A finished track counts as fully downloaded. */
static inline bool top_shows_spinner(player_phase phase,
                                     unsigned download_pct)
{
	return (phase == PLAYER_PHASE_LOADING ||
	        phase == PLAYER_PHASE_PLAYING) && download_pct == 0;
}

#endif
