/* The strip of kinds on the top screen while Search shows its results
 * (docs/STYLES.md 5.17): one segment per kind the search offers, with its
 * icon and how many were found, the kind on screen highlighted. Not a
 * control: nothing on the top screen is touched, and left/right on the
 * bottom one switch the kind. It sits above the focused result's card
 * (7.12), which starts at y 40.
 *
 * Per STYLES.md 9 it asks for a frame only when what it shows changes:
 * the active kind, a count, or the nudge starting and ending. Main thread
 * only. */
#ifndef TOP_STRIP_H
#define TOP_STRIP_H

#include "results_logic.h"

#include <stdbool.h>

/* After ui_init; false when there is no memory for the numbers. */
bool top_strip_init(void);
void top_strip_exit(void);

/* Forces a repaint, for when the strip comes into view. */
void top_strip_invalidate(void);

bool top_strip_dirty(const results_strip *s);
void top_strip_draw(const results_strip *s);

#endif
