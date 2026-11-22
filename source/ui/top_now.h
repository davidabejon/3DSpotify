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

/* The card's head, shared with the detail card (top_detail.h): the cover
 * from `art` at 160x160 with its hard shadow, `title` in t_titulo and up
 * to NOW_LINES `lines` in t_cuerpo under it, right of the cover. The
 * texts must be fitted to now_text_w(). `centred` (the detail card, with
 * no bar under it) centres the cover on the screen's height and the texts
 * beside it (now_detail_art_rect, now_detail_text_dy), or, with `art_y`
 * >= 0, puts the cover's top there (under the strip of kinds). `top`,
 * centred only: over the mini player (STYLES.md 5.18) the texts hang from
 * the cover's top edge instead (now_detail_text_dy_top_at), leaving the
 * bottom corner to it. `rolls`, NULL for none, holds 1 + count entries,
 * the title's first: -1 for a text drawn as it is, otherwise how far it
 * has rolled inside now_text_w() (marquee_logic.h; such a text is the
 * whole string, not fitted). `meta`, centred only (NULL, 0 for none): up
 * to ARTIST_BIO_LINES t_meta lines SP_2 under the rest, already wrapped
 * to now_text_w(), which never roll; a centred block is centred with
 * them. */
void top_now_draw_header(artwork_state *art, bool centred, bool top,
                         int art_y,
                         const C2D_Text *title, const C2D_Text *const *lines,
                         int count, const int *rolls, int icon,
                         const C2D_Text *const *meta, int meta_count);
/* `icon` shows in the cover's place while it loads, or when there is none
 * (centred only: the card of what plays shows a note). */
void top_now_draw_big(artwork_state *art);

#endif
