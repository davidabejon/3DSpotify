/* The kinds of the search results (search_results.h) as the results view
 * shows them: one kind at a time, switched with left/right, in the fixed
 * order tracks, artists, albums, playlists. A kind with nothing found is
 * passed over, and the ends do not wrap. Each kind keeps where its list
 * was left, so coming back to it finds the focus where it was.
 *
 * Pure C with no 3DS headers, tested in tests/native_logic_test.c. */
#ifndef RESULTS_LOGIC_H
#define RESULTS_LOGIC_H

#include "search_results.h"
#include "theme.h"

#include <stdbool.h>
#include <string.h>

typedef struct {
	bool offered[RESULT_KINDS];   /* the source gives the kind at all */
	int  count[RESULT_KINDS];
	/* Where each kind's list was left: its focus, and its first visible
	 * row (-1 until it has been shown, so the list places the focus). */
	int  selected[RESULT_KINDS], first[RESULT_KINDS];
	result_kind active;
} results_kinds;

static inline bool results_kind_enabled(const results_kinds *k,
                                        result_kind kind)
{
	return (unsigned)kind < RESULT_KINDS && k->offered[kind] &&
	       k->count[kind] > 0;
}

/* How many kinds have something to show. */
static inline int results_kinds_enabled(const results_kinds *k)
{
	int n = 0;
	for (int i = 0; i < RESULT_KINDS; i++)
		n += results_kind_enabled(k, (result_kind)i);
	return n;
}

/* `preferred` (or -1): the kind to start on, with `preferred_index`
 * focused, if it has results; else the first kind with results, focused
 * on its first row. False when no kind has any (active is then tracks). */
static inline bool results_kinds_init(results_kinds *k, const bool offered[],
                                      const int count[], int preferred,
                                      int preferred_index)
{
	memset(k, 0, sizeof(*k));
	for (int i = 0; i < RESULT_KINDS; i++) {
		k->offered[i] = offered[i];
		k->count[i] = count[i] < 0 ? 0 : count[i];
		k->first[i] = -1;
	}
	k->active = RESULT_TRACKS;
	if (preferred >= 0 && results_kind_enabled(k, (result_kind)preferred)) {
		k->active = (result_kind)preferred;
		if (preferred_index > 0 && preferred_index < k->count[preferred])
			k->selected[preferred] = preferred_index;
		return true;
	}
	for (int i = 0; i < RESULT_KINDS; i++)
		if (results_kind_enabled(k, (result_kind)i)) {
			k->active = (result_kind)i;
			return true;
		}
	return false;
}

/* -1/+1: to the next kind with results that way. False at the end, where
 * nothing wraps: the caller shows the edge. */
static inline bool results_kind_step(results_kinds *k, int dir)
{
	if (dir == 0)
		return false;
	dir = dir < 0 ? -1 : 1;
	for (int i = (int)k->active + dir; i >= 0 && i < RESULT_KINDS; i += dir)
		if (results_kind_enabled(k, (result_kind)i)) {
			k->active = (result_kind)i;
			return true;
		}
	return false;
}

/* The active kind's list as it is being left. */
static inline void results_kind_keep(results_kinds *k, int selected,
                                     int first)
{
	k->selected[k->active] = selected;
	k->first[k->active] = first;
}

/* What the top screen's strip of kinds shows (STYLES.md 5.17): one
 * segment per kind offered, its count, the active one, and the nudge when
 * left/right found no kind that way (edge_dir -1/+1 for edge_frames). */
typedef struct {
	bool offered[RESULT_KINDS];
	int  count[RESULT_KINDS];
	result_kind active;
	int  edge_dir, edge_frames;
} results_strip;

/* The strip's span: the top screen inside e3. */
#define RESULTS_STRIP_X0 SP_3
#define RESULTS_STRIP_W  (SCREEN_TOP_W - 2 * SP_3)

/* `n` segments across the strip, GRID_GAP apart; the remainder of the
 * division goes to the first ones, one pixel each, as on the home grid.
 * 4: 91 each; 3: 123, 123, 122. */
static inline void results_strip_layout(int n, int x[], int w[])
{
	if (n <= 0)
		return;
	int room = RESULTS_STRIP_W - (n - 1) * GRID_GAP;
	int base = room / n, extra = room % n;
	int at = RESULTS_STRIP_X0;
	for (int i = 0; i < n; i++) {
		w[i] = base + (i < extra ? 1 : 0);
		x[i] = at;
		at += w[i] + GRID_GAP;
	}
}

/* The strip as it looks: its segment of the active kind nudged or not. */
static inline bool results_strip_same(const results_strip *a,
                                      const results_strip *b)
{
	if (a->active != b->active ||
	    (a->edge_frames > 0 ? a->edge_dir : 0) !=
	    (b->edge_frames > 0 ? b->edge_dir : 0))
		return false;
	for (int i = 0; i < RESULT_KINDS; i++)
		if (a->offered[i] != b->offered[i] || a->count[i] != b->count[i])
			return false;
	return true;
}

#endif
