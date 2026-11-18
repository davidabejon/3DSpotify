/* Which playlists the home tab shows, in what order (STYLES.md 7.7).
 * Pure C with no 3DS headers, tested in tests/native_logic_test.c.
 *
 * The play history names playlists by URI only and in the order they were
 * played; the rootlist names the account's own folder with names, covers
 * and lengths. The home list is the history's playlists first, each taking
 * its name and cover from the rootlist when it is there, then the rest of
 * the rootlist in its own order, without repeats. A recent playlist that
 * is not in the folder (someone else's, or one removed from it) is marked
 * `missing`: the loader asks for its header to name it. */
#ifndef HOME_FEED_LOGIC_H
#define HOME_FEED_LOGIC_H

#include "spclient.h"

#include <stdbool.h>
#include <string.h>

static inline int home_find(const sp_playlist_ref *list, int n,
                            const char *uri)
{
	for (int i = 0; i < n; i++)
		if (strcmp(list[i].uri, uri) == 0)
			return i;
	return -1;
}

/* Fills out[0..max) and missing[0..max); returns how many. `total` (may be
 * NULL) gets how many distinct playlists there are in all, `root_total`
 * counting the folder entries that were not fetched (the rootlist's
 * length beyond `nroot`). */
static inline int home_merge(const char (*recent)[SP_URI_MAX], int nrecent,
                             const sp_playlist_ref *root, int nroot,
                             int root_total, sp_playlist_ref *out,
                             bool *missing, int max, int *total)
{
	int n = 0, distinct = 0;
	for (int i = 0; i < nrecent; i++) {
		bool repeat = false;
		for (int k = 0; k < i && !repeat; k++)
			repeat = strcmp(recent[k], recent[i]) == 0;
		if (repeat)
			continue;
		distinct++;
		if (n >= max)
			continue;
		int at = home_find(root, nroot, recent[i]);
		if (at >= 0) {
			out[n] = root[at];
			missing[n] = false;
		} else {
			memset(&out[n], 0, sizeof(out[n]));
			memcpy(out[n].uri, recent[i], SP_URI_MAX);
			out[n].uri[SP_URI_MAX - 1] = '\0';
			missing[n] = true;
		}
		n++;
	}
	for (int i = 0; i < nroot; i++) {
		bool seen = false;
		for (int k = 0; k < nrecent && !seen; k++)
			seen = strcmp(recent[k], root[i].uri) == 0;
		for (int k = 0; k < i && !seen; k++)
			seen = strcmp(root[k].uri, root[i].uri) == 0;
		if (seen)
			continue;
		distinct++;
		if (n < max) {
			out[n] = root[i];
			missing[n] = false;
			n++;
		}
	}
	if (total)
		*total = distinct + (root_total > nroot ? root_total - nroot : 0);
	return n;
}

/* Drops entry `index` (a missing playlist whose header did not come),
 * keeping the order of the rest. Returns the new count. */
static inline int home_drop(sp_playlist_ref *out, bool *missing, int n,
                            int index)
{
	if (index < 0 || index >= n)
		return n;
	for (int i = index; i < n - 1; i++) {
		out[i] = out[i + 1];
		missing[i] = missing[i + 1];
	}
	return n - 1;
}

#endif
