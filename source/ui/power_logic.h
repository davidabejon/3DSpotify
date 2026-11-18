#ifndef POWER_LOGIC_H
#define POWER_LOGIC_H

/* What a closed lid does, decided without console headers so the native
 * tests can check it. While music is due, closing the lid only darkens the
 * screens: sleep would cut the audio and drop the Wi-Fi under the download.
 * Otherwise the console sleeps as usual. */

#include "player_logic.h"

#include <stdbool.h>

/* A track loading counts: the user pressed play and expects to hear it.
 * So does a finished one while the queue goes on (`follows`): the next
 * track starts a frame later, and letting go for that frame would sleep
 * the console and drop the Wi-Fi it needs. Only through headphones,
 * though: the system mutes the speakers while the lid is shut, so without
 * them staying awake would play to nobody. */
static inline bool power_keep_awake(player_phase phase, bool paused,
                                    bool follows, bool headphones)
{
	return headphones &&
	       (phase == PLAYER_PHASE_LOADING ||
	        (phase == PLAYER_PHASE_PLAYING && !paused) ||
	        (phase == PLAYER_PHASE_FINISHED && follows));
}

/* The backlights stay on unless the lid is shut over music. When the music
 * stops with the lid shut they come back on, just before the console is let
 * to sleep, so it wakes with lit screens. */
static inline bool power_screens_lit(bool lid_open, bool keep_awake)
{
	return lid_open || !keep_awake;
}

/* Sleep drops the Wi-Fi, and after waking the console takes a few seconds
 * to join it again. Missing Wi-Fi only counts as a lost connection once
 * this long has passed since the wake; before, it is the sleep's. */
#define POWER_WAKE_GRACE_MS 30000

static inline bool power_wifi_settling(bool woke, unsigned long long now,
                                       unsigned long long woke_at)
{
	return woke && now - woke_at < POWER_WAKE_GRACE_MS;
}

#endif
