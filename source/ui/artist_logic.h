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

/* "Discography" and `releases` releases (types[i], an SP_ALBUM_*), at most
 * `max` rows; nothing for no releases. A release's `index` is its position
 * among the releases. Returns how many rows. Also how the discography is
 * appended once it is named, after the popular tracks. */
static inline void artist_header(track_row *r, int title, bool button)
{
	/* A header with the biography's button takes the focus: the button. */
	artist_row(r, TRACK_ROW_HEADER, title, !button);
	r->button = button;
}

static inline int artist_discography_rows(int releases, const int *types,
                                          track_row *rows, int max,
                                          bool button)
{
	int n = 0;
	if (releases <= 0 || max <= 0)
		return 0;
	artist_header(&rows[n++], STR_ARTIST_DISCOGRAPHY, button);
	for (int i = 0; i < releases && n < max; i++) {
		track_row *r = &rows[n++];
		artist_row(r, TRACK_ROW_ALBUM, i, false);
		r->release = true;
		r->album_type = (uint8_t)(types ? types[i] : SP_ALBUM_ALBUM);
	}
	return n;
}

/* The rows of an artist's detail: a header and `top` tracks, then the
 * discography (artist_discography_rows); a section with nothing is left
 * out, header and all. At most `max` rows. Returns how many; `*top_first`
 * is the row of the first popular track (-1 for none), so the tracks are
 * rows[top_first .. top_first + top). A track's `index` is its position
 * among the tracks. `bio_button`: the first header carries the button
 * that shows the biography ("Popular tracks", unless there are none). */
static inline int artist_rows(int top, int releases, const int *types,
                              track_row *rows, int max, int *top_first,
                              bool bio_button)
{
	int n = 0;
	*top_first = -1;
	if (top > 0 && n < max) {
		artist_header(&rows[n++], STR_ARTIST_TOP_TRACKS, bio_button);
		bio_button = false;
		*top_first = n < max ? n : -1;
		for (int i = 0; i < top && n < max; i++)
			artist_row(&rows[n++], TRACK_ROW_TRACK, i, false);
	}
	return n + artist_discography_rows(releases, types, rows + n, max - n,
	                                   bio_button);
}

/* Where the focus starts: the first track or release, past a header's
 * button; else whatever takes it (track_row_first_on); -1 for none. */
static inline int artist_first_item(const track_row *rows, int n)
{
	for (int i = 0; i < n; i++)
		if (rows[i].kind != TRACK_ROW_HEADER && !rows[i].off)
			return i;
	return track_row_first_on(rows, n, 0);
}

/* `n` with `sep` between groups of three digits ("12.345.678" with '.',
 * "12,345,678" with ','), into `out`; "" for a negative `n` or no room. */
static inline void artist_format_count(long n, char sep, char *out,
                                       size_t len)
{
	if (!out || len == 0)
		return;
	out[0] = '\0';
	if (n < 0)
		return;
	char digits[24];
	int nd = 0;
	do {
		digits[nd++] = (char)('0' + n % 10);
		n /= 10;
	} while (n > 0 && nd < (int)sizeof(digits));
	size_t need = (size_t)nd + (size_t)(nd - 1) / 3 + 1;
	if (need > len)
		return;
	size_t o = 0;
	for (int i = nd - 1; i >= 0; i--) {
		out[o++] = digits[i];
		if (i > 0 && i % 3 == 0)
			out[o++] = sep;
	}
	out[o] = '\0';
}

/* Naming the releases: `n` of them in batches of `size` (SP_ALBUM_BATCH).
 * How many batches, and how long batch `b` is. */
static inline int artist_batches(int n, int size)
{
	return n > 0 && size > 0 ? (n + size - 1) / size : 0;
}

static inline int artist_batch_len(int n, int size, int b)
{
	int left = n - b * size;
	return left < 0 ? 0 : left < size ? left : size;
}

/* Drops the releases that came back without a name (their batch failed,
 * or the service left them out): no row without a title. The rest close
 * ranks in their order. Returns how many are left. */
static inline int artist_keep_named(sp_release *r, int n)
{
	int kept = 0;
	for (int i = 0; i < n; i++) {
		if (!r[i].name[0])
			continue;
		if (kept != i)
			r[kept] = r[i];
		kept++;
	}
	return kept;
}

#endif
