/* The mini player (docs/STYLES.md 5.18): what plays, in the bottom right
 * corner of the top screen while it shows something else -- the cover at
 * 32x32, the title, the artist, whether it is paused and a progress band.
 * It is not a control: the top screen has no touch, and pausing or
 * skipping stays on the Playing tab. The layout is in now_layout.h;
 * top_logic.h (top_shows_mini) says when it shows.
 *
 * The cover is the player's cover_art, only ever drawn: the player waits
 * for that image before preparing a track, so asking it for another one
 * (artwork_request, artwork_set_preview) would hold the music up.
 *
 * While the track's download is at 0% the download spinner turns where its
 * state icon goes, instead of at the centre of the screen.
 *
 * Per STYLES.md 9 a repaint is only needed for a phase or track change,
 * the pause, the band moving a whole pixel, the cover arriving or the
 * language; besides the spinner (whose steps top.c counts), nothing in it
 * moves by itself. Main thread only. */
#ifndef TOP_MINI_H
#define TOP_MINI_H

#include "artwork.h"
#include "view_player.h"

#include <stdbool.h>

/* After ui_init; false when there is no memory for its text. */
bool top_mini_init(void);
void top_mini_exit(void);

/* Once per frame, with the snapshot of top_set. `info` is copied; the
 * track must stay valid until the next call. */
void top_mini_set(const player_info *info);

/* Forces a repaint, for when it comes into view. */
void top_mini_invalidate(void);

/* Only while the mini player is on screen: reads artwork_dirty, which only
 * drawing clears. */
bool top_mini_dirty(const artwork_state *art);

/* `art` is the player's cover_art: drawn, never requested. `panel` is
 * where it goes, from now_layout.h (now_mini_card_rect and the rest): its
 * corner follows what it sits by. `spin_step` >= 0: the spinner's step,
 * drawn in place of the icon; -1 for none. */
void top_mini_draw(artwork_state *art, ui_rect panel, int spin_step);

#endif
