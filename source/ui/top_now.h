/* The top screen while music plays (docs/STYLES.md 7.5): the "now
 * playing" card (cover, title, artist, album, times and progress) and the
 * big cover. The layout is in now_layout.h; the bar, the times and the
 * texts are the bottom player's (ui_progress, player_logic.h).
 *
 * Per 9, a repaint is only needed for a new second, the sampled download,
 * a phase or track change, the cover arriving or the language; the big
 * cover only for the cover itself. Main thread only. */
#ifndef TOP_NOW_H
#define TOP_NOW_H

#include "artwork.h"
#include "view_player.h"

#include <stdbool.h>

bool top_now_init(void);
void top_now_exit(void);

/* What the player is doing now, once per frame (the same snapshot as
 * view_player_set). `info` is copied; the track must stay valid until the
 * next call. */
void top_now_set(const player_info *info);

/* Forces a repaint, for when the card or the cover comes into view. */
void top_now_invalidate(void);

/* Whether the card (`big` false) or the big cover (`big` true) needs a
 * repaint. Only call it while that one is on screen: it reads
 * artwork_dirty, which only drawing clears. */
bool top_now_dirty(bool big, const artwork_state *art);

void top_now_draw_card(artwork_state *art);
void top_now_draw_big(artwork_state *art);

#endif
