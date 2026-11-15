/* A line of text too wide for its box rolls instead of being cut with an
 * ellipsis: it waits MARQUEE_PAUSE_MS at its start, then slides left at
 * MARQUEE_SPEED px/s, followed by a second copy MARQUEE_GAP px behind it,
 * until that copy sits where the first began; then it waits again. The
 * wrap is seamless, as the copy is the same text in the same place.
 * Pure C with no 3DS headers, tested in tests/native_logic_test.c. */
#ifndef MARQUEE_LOGIC_H
#define MARQUEE_LOGIC_H

#include "theme.h"

#include <stdbool.h>

enum {
	MARQUEE_PAUSE_MS = 1500,
	MARQUEE_SPEED = 30,
	MARQUEE_GAP = SP_6 * 2,
};

static inline bool marquee_rolls(int text_w, int box_w)
{
	return text_w > box_w;
}

/* How far a full cycle slides: one text and the gap. */
static inline int marquee_span(int text_w)
{
	return text_w + MARQUEE_GAP;
}

/* The whole cycle, pause included, in ms. */
static inline unsigned marquee_period_ms(int text_w)
{
	return MARQUEE_PAUSE_MS +
	       (unsigned)marquee_span(text_w) * 1000u / MARQUEE_SPEED;
}

/* How many px the text has slid left `ms` after it first showed: 0 for
 * a text that fits, and always in 0..marquee_span(text_w) - 1. */
static inline int marquee_offset(unsigned ms, int text_w, int box_w)
{
	if (!marquee_rolls(text_w, box_w))
		return 0;
	unsigned t = ms % marquee_period_ms(text_w);
	if (t < MARQUEE_PAUSE_MS)
		return 0;
	int offset = (int)((t - MARQUEE_PAUSE_MS) * MARQUEE_SPEED / 1000u);
	return offset < marquee_span(text_w) ? offset : 0;
}

#endif
