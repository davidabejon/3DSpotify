/* Which result thumbnail to fetch next (docs/STYLES.md 9). Pure C, no 3DS
 * headers, tested in tests/native_logic_test.c. */
#ifndef THUMB_QUEUE_H
#define THUMB_QUEUE_H

#include <stdbool.h>

typedef enum {
	THUMB_NONE,     /* the track has no cover */
	THUMB_WAITING,  /* not fetched yet */
	THUMB_LOADING,  /* the worker is on it */
	THUMB_DECODED,  /* texels ready, waiting for the main thread's upload */
	THUMB_READY,    /* in its texture */
	THUMB_FAILED    /* download or decode failed: keep the placeholder */
} thumb_state;

/* The next thumbnail to fetch: the first waiting one among the `visible`
 * rows from `first`, then among the `visible` rows after them. Nothing
 * further down is fetched until the list scrolls there. -1 when there is
 * nothing to do. */
static inline int thumb_pick(const thumb_state *states, int count, int first,
                             int visible)
{
	if (first < 0)
		first = 0;
	int end = first + 2 * visible;
	if (end > count)
		end = count;
	for (int i = first; i < end; i++)
		if (states[i] == THUMB_WAITING)
			return i;
	return -1;
}

/* Whether a list update can keep what slot `i` already holds: the same
 * cover, already downloaded (or with nothing to download). A failed one is
 * tried again: the Wi-Fi may be back. */
static inline bool thumb_keep(thumb_state state, bool same_url)
{
	return same_url && (state == THUMB_READY || state == THUMB_NONE ||
	                    state == THUMB_DECODED);
}

#endif
