/* What the top screen shows with the tabs (feature_plan.md, phase 0): the
 * logo while no music plays, else the "now playing" card, or the big
 * cover on the Playing tab. Pure C with no 3DS headers, tested in
 * tests/native_logic_test.c. */
#ifndef TOP_LOGIC_H
#define TOP_LOGIC_H

#include "player_logic.h"
#include "tabs_logic.h"

#include <stdbool.h>

typedef enum {
	TOP_LOGO,
	TOP_NOW_CARD,
	TOP_ART_BIG
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

static inline top_mode top_mode_for(app_tab tab, player_phase phase)
{
	if (!top_music_on(phase))
		return TOP_LOGO;
	return tab == TAB_PLAYER ? TOP_ART_BIG : TOP_NOW_CARD;
}

#endif
