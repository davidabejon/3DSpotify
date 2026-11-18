/* An artist's detail as a list in sections (STYLES.md 7.11): "Popular
 * tracks" and its tracks, then "Discography" and its releases, each under
 * a header row that takes no focus (offline_logic.h). Pure C with no 3DS
 * headers, tested in tests/native_logic_test.c. */
#ifndef ARTIST_LOGIC_H
#define ARTIST_LOGIC_H

#include "lang.h"
#include "offline_logic.h"
#include "spclient.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* What a release is, for its second line: the type the Album says, or,
 * when it says none, the one its group implies. A list that mixes kinds
 * always says which each row is. */
static inline int artist_release_type(int type, int group)
{
	if (type >= SP_ALBUM_ALBUM && type <= SP_ALBUM_EP)
		return type;
	switch (group) {
	case SP_GROUP_SINGLE:
		return SP_ALBUM_SINGLE;
	case SP_GROUP_COMPILATION:
		return SP_ALBUM_COMPILATION;
	default:
		return SP_ALBUM_ALBUM;
	}
}

/* The str_id of an SP_ALBUM_* type. */
static inline str_id artist_type_label(int type)
{
	switch (type) {
	case SP_ALBUM_SINGLE:
		return STR_ALBUM_TYPE_SINGLE;
	case SP_ALBUM_COMPILATION:
		return STR_ALBUM_TYPE_COMPILATION;
	case SP_ALBUM_EP:
		return STR_ALBUM_TYPE_EP;
	default:
		return STR_ALBUM_TYPE_ALBUM;
	}
}

/* A header keeps its str_id in track_row.index. */
_Static_assert(STR_COUNT <= INT16_MAX, "a str_id must fit track_row.index");

static inline void artist_row(track_row *r, track_row_kind kind, int index,
                              bool off)
{
	memset(r, 0, sizeof(*r));
	r->kind = (uint8_t)kind;
	r->off = off;
	r->index = (int16_t)index;
}

/* The rows of an artist's detail: a header and `top` tracks, then a
 * header and `releases` releases (types[i], an SP_ALBUM_*); a section
 * with nothing is left out, header and all. At most `max` rows. Returns
 * how many; `*top_first` is the row of the first popular track (-1 for
 * none), so the tracks are rows[top_first .. top_first + top). A track's
 * `index` is its position among the tracks, a release's among the
 * releases. */
static inline int artist_rows(int top, int releases, const int *types,
                              track_row *rows, int max, int *top_first)
{
	int n = 0;
	*top_first = -1;
	if (top > 0 && n < max) {
		artist_row(&rows[n++], TRACK_ROW_HEADER, STR_ARTIST_TOP_TRACKS, true);
		*top_first = n < max ? n : -1;
		for (int i = 0; i < top && n < max; i++)
			artist_row(&rows[n++], TRACK_ROW_TRACK, i, false);
	}
	if (releases > 0 && n < max) {
		artist_row(&rows[n++], TRACK_ROW_HEADER, STR_ARTIST_DISCOGRAPHY,
		           true);
		for (int i = 0; i < releases && n < max; i++) {
			track_row *r = &rows[n++];
			artist_row(r, TRACK_ROW_ALBUM, i, false);
			r->release = true;
			r->album_type = (uint8_t)(types ? types[i] : SP_ALBUM_ALBUM);
		}
	}
	return n;
}

#endif
