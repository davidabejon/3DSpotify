/* Which result thumbnail to fetch next (docs/STYLES.md 9). Pure C, no 3DS
 * headers, tested in tests/native_logic_test.c. */
#ifndef THUMB_QUEUE_H
#define THUMB_QUEUE_H

#include <stdbool.h>
#include <string.h>

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

/* Where each slot's texture comes from when the list changes: new slot i
 * takes old slot src[i]'s. A slot keeps its own where thumb_keep allows;
 * otherwise a READY slot elsewhere with the same URL moves there, which is
 * what a long list's window does when it scrolls (the covers on screen
 * stay, nothing is downloaded again); the remaining slots take the
 * leftover textures and are fetched. `kept[i]` says whether slot i holds
 * a cover kept for it. src is a permutation of 0..slots-1. Only READY
 * slots move: a DECODED one waits in its own staging buffer. */
#define THUMB_PLAN_MAX 64

static inline void thumb_reuse_plan(const char *const *old_urls,
                                    const thumb_state *old_states,
                                    int old_count, const char *const *urls,
                                    int n, int slots, int *src, bool *kept)
{
	bool used[THUMB_PLAN_MAX] = { false };
	if (slots > THUMB_PLAN_MAX)
		slots = THUMB_PLAN_MAX;
	for (int i = 0; i < slots; i++) {
		src[i] = -1;
		kept[i] = false;
		bool same = i < n && i < old_count && strcmp(old_urls[i], urls[i]) == 0;
		if (i < n && thumb_keep(old_states[i], same)) {
			src[i] = i;
			kept[i] = true;
			used[i] = true;
		}
	}
	for (int i = 0; i < n && i < slots; i++) {
		if (kept[i] || !urls[i][0])
			continue;
		for (int j = 0; j < old_count && j < slots; j++)
			if (!used[j] && old_states[j] == THUMB_READY &&
			    strcmp(old_urls[j], urls[i]) == 0) {
				src[i] = j;
				kept[i] = true;
				used[j] = true;
				break;
			}
	}
	int next = 0;
	for (int i = 0; i < slots; i++) {
		if (src[i] >= 0)
			continue;
		while (used[next])
			next++;
		src[i] = next;
		used[next] = true;
	}
}

#endif
