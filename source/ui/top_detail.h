/* The top screen while a detail is open (STYLES.md 7.12): the card of the
 * track, album, artist or playlist on top of the active tab -- or of the
 * focused home tile or search result, as a preview --
 * laid out like the "now playing" card (STYLES.md 7.5, top_now.h) with a
 * different source -- the cover or portrait at 160x160, the name in
 * t_titulo and up to NOW_LINES lines of t_cuerpo:
 *
 *   track   artist / album / "3:42 - track 2 - disc 2" / year
 *   album   artist / "Album - 2004" / "11 tracks" or "24 of 31 tracks" /
 *           label
 *   artist  "Popular tracks" ("Artist" for a search result's preview)
 *
 * The image is the caller's `detail_art`, never the playing track's
 * cover_art: the player waits for that one before preparing a track, and
 * asking it for another image would hold the music up.
 *
 * A text wider than the column is not cut with "...": it rolls
 * (marquee_logic.h), clipped to the column.
 *
 * Per STYLES.md 9 a repaint is only needed when the document on screen
 * changes (another level, or the same one loaded), the image arrives, the
 * language switches or a rolling text moves a pixel. Main thread only. */
#ifndef TOP_DETAIL_H
#define TOP_DETAIL_H

#include "artwork.h"
#include "details.h"

#include <stdbool.h>

bool top_detail_init(void);
void top_detail_exit(void);

/* Forces a repaint, for when the card comes into view. */
void top_detail_invalidate(void);

/* Only while the card is on screen: reads artwork_dirty, which only
 * drawing clears. */
bool top_detail_dirty(const detail_doc *doc, const artwork_state *art);

/* `art_y`: the cover's top, -1 for the card alone (centred on the
 * screen's height); RESULTS_CARD_Y under the strip of kinds. */
void top_detail_draw(const detail_doc *doc, artwork_state *art, int art_y);

/* An artist with `bio_open` shows its whole biography instead of the card,
 * word-wrapped to the screen and scrolled down by `bio_scroll` lines, with
 * a scroll indicator when it does not fit. For the view that scrolls it:
 * the furthest `bio_scroll` goes (0 when it all fits; wrapping it the
 * first time, after ui_init), and how many lines a page shows. */
int top_detail_bio_max_scroll(const detail_doc *d);
int top_detail_bio_page_lines(void);

#endif
