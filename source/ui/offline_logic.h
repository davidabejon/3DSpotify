/* What the Home tab's Downloads lists, from the downloads index, without
 * the network. Pure C with no 3DS headers, tested in
 * tests/native_logic_test.c.
 *
 * Two views:
 *   Downloads       every album and playlist in the index (whole or still
 *                   downloading), newest first, then the tracks downloaded
 *                   on their own, newest first. A list of tracks and
 *                   collections mixed: each row says which it is
 *                   (track_row), and draws differently (STYLES.md 5.3).
 *   A collection    its tracks in their order; only those on the card play,
 *                   the rest show what they wait for and the D-pad skips
 *                   them.
 * The Home tab's first row is two tiles that do not come from the feed:
 * Liked Songs and Downloads (DL_HOME_URI). */
#ifndef OFFLINE_LOGIC_H
#define OFFLINE_LOGIC_H

#include "downloads_logic.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The Downloads tile's own URI. The app's, never the service's: nothing
 * with it may reach spclient. */
#define DL_HOME_URI "3dspotify:downloads"

/* Tiles pinned at the start of the home grid: Liked Songs, Downloads. */
#define HOME_PINNED 2

typedef enum {
	TRACK_ROW_TRACK,
	TRACK_ROW_ALBUM,
	TRACK_ROW_PLAYLIST,
	TRACK_ROW_ARTIST,      /* a search result only */
	TRACK_ROW_HEADER       /* a section's title: `index` is its str_id */
} track_row_kind;

/* One row of a list that is not only playable tracks: the downloads, the
 * search results of a kind other than tracks, or an artist's sections. */
typedef struct {
	uint8_t kind;      /* track_row_kind */
	uint8_t state;     /* a track's dl_state */
	/* Not on the card, or a section's header: never focused. A track off
	 * shows dimmed. */
	bool    off;
	/* A search result: the second line says what it is ("Album · ..."),
	 * not what is downloaded of it. */
	bool    search;
	/* A release of an artist's discography: the second line says its type
	 * and year ("Album · 2004"). */
	bool    release;
	uint8_t album_type;    /* a release's SP_ALBUM_* */
	/* A header with a square button at its right (an artist's biography):
	 * the button takes the focus and A, so the header is not off. */
	bool    button;
	/* Into idx->tracks or idx->colls; an artist's track or release; a
	 * header's str_id. */
	int16_t index;
	int16_t done, count;   /* a collection's tracks on the card, in all */
} track_row;

typedef struct {
	uint32_t added_at;
	int16_t  index;
} offline_item;

static inline int offline_newest_first(const void *pa, const void *pb)
{
	const offline_item *a = (const offline_item *)pa;
	const offline_item *b = (const offline_item *)pb;
	if (a->added_at != b->added_at)
		return a->added_at < b->added_at ? 1 : -1;
	return a->index - b->index;
}

/* The Downloads view's rows, up to `max`: collections, then the tracks
 * downloaded on their own. `scratch` holds DL_TRACKS_MAX items. Returns
 * how many; `*colls` says how many of them are collections. */
static inline int offline_home_rows(const dl_index *idx, track_row *rows,
                                    int max, offline_item *scratch,
                                    int *colls)
{
	*colls = 0;
	if (!idx)
		return 0;
	int n = 0, k = 0;
	for (int c = 0; c < idx->coll_count; c++)
		scratch[k++] = (offline_item){ idx->colls[c].added_at, (int16_t)c };
	qsort(scratch, (size_t)k, sizeof(scratch[0]), offline_newest_first);
	for (int i = 0; i < k && n < max; i++) {
		track_row *r = &rows[n++];
		int c = scratch[i].index;
		int done, count;
		dl_collection_progress(idx, c, &done, &count);
		r->kind = idx->colls[c].kind == DL_COLL_PLAYLIST ? TRACK_ROW_PLAYLIST
		                                                 : TRACK_ROW_ALBUM;
		r->state = DL_DONE;
		r->off = false;
		r->search = false;
		r->release = false;
		r->album_type = 0;
		r->button = false;
		r->index = (int16_t)c;
		r->done = (int16_t)done;
		r->count = (int16_t)count;
	}
	*colls = n;
	k = 0;
	for (int t = 0; t < idx->track_count; t++)
		if (idx->tracks[t].standalone && idx->tracks[t].state == DL_DONE)
			scratch[k++] = (offline_item){ idx->tracks[t].added_at,
			                               (int16_t)t };
	qsort(scratch, (size_t)k, sizeof(scratch[0]), offline_newest_first);
	for (int i = 0; i < k && n < max; i++) {
		track_row *r = &rows[n++];
		r->kind = TRACK_ROW_TRACK;
		r->state = DL_DONE;
		r->off = false;
		r->search = false;
		r->release = false;
		r->album_type = 0;
		r->button = false;
		r->index = scratch[i].index;
		r->done = r->count = 0;
	}
	return n;
}

/* Collection `c`'s tracks, in its order, up to `max`. */
static inline int offline_collection_rows(const dl_index *idx, int c,
                                          track_row *rows, int max)
{
	if (!idx || c < 0 || c >= idx->coll_count)
		return 0;
	int n = 0;
	for (int i = 0; i < idx->ref_count && n < max; i++) {
		if (idx->refs[i].coll != c)
			continue;
		int t = idx->refs[i].track;
		track_row *r = &rows[n++];
		r->kind = TRACK_ROW_TRACK;
		r->state = (uint8_t)idx->tracks[t].state;
		r->off = idx->tracks[t].state != DL_DONE;
		r->search = false;
		r->release = false;
		r->album_type = 0;
		r->button = false;
		r->index = (int16_t)t;
		r->done = r->count = 0;
	}
	return n;
}

/* Whether row `i` can take the focus (and play): any row of a list
 * without row data. */
static inline bool track_row_on(const track_row *rows, int i)
{
	return !rows || !rows[i].off;
}

/* The D-pad over rows that may be off: the next row `delta` away (a page
 * for |delta| > 1) that is on, stopping at the ends. `*bumped` when it
 * could not move at all. */
static inline int track_row_move(const track_row *rows, int count,
                                 int selected, int delta, bool *bumped)
{
	*bumped = false;
	if (count <= 0 || delta == 0)
		return selected;
	int step = delta < 0 ? -1 : 1;
	int left = delta < 0 ? -delta : delta;
	int at = selected, last_on = selected;
	while (left > 0) {
		int next = at + step;
		if (next < 0 || next >= count)
			break;
		at = next;
		if (track_row_on(rows, at)) {
			last_on = at;
			left--;
		}
	}
	*bumped = last_on == selected;
	return last_on;
}

/* The first row that is on at or after `from`, else before it; -1 for
 * none. */
static inline int track_row_first_on(const track_row *rows, int count,
                                     int from)
{
	if (from < 0)
		from = 0;
	for (int i = from; i < count; i++)
		if (track_row_on(rows, i))
			return i;
	for (int i = from - 1; i >= 0; i--)
		if (track_row_on(rows, i))
			return i;
	return -1;
}

/* The first visible row once the focus is on `selected`: a section's
 * first row brings its header into view with it (STYLES.md 6.6), so a
 * header just above the focus is never left out at the top. Any other
 * `first` stays. */
static inline int track_row_show_header(const track_row *rows, int count,
                                        int selected, int first)
{
	if (!rows || selected <= 0 || selected >= count)
		return first;
	int above = selected - 1;
	if (rows[above].kind == TRACK_ROW_HEADER && first > above)
		return above;
	return first;
}

/* The playable span of a list with rows: the first run of track rows,
 * whether on the card or not (the collections before the Downloads'
 * tracks, the header and the discography around an artist's). Returns
 * how many, with `*first` where they start; 0 for none. */
static inline int track_row_track_span(const track_row *rows, int count,
                                       int *first)
{
	*first = 0;
	int i = 0;
	while (i < count && rows[i].kind != TRACK_ROW_TRACK)
		i++;
	int n = 0;
	while (i + n < count && rows[i + n].kind == TRACK_ROW_TRACK)
		n++;
	*first = n ? i : 0;
	return n;
}

/* The home grid: feed item `i` (0 is Liked Songs) goes to tile `i` for
 * the first, and after the pinned Downloads tile for the rest. */
static inline int home_tile_of_feed(int i)
{
	return i < 1 ? i : i + 1;
}

/* Which tile is Downloads. */
static inline int home_downloads_tile(void)
{
	return 1;
}

#endif
