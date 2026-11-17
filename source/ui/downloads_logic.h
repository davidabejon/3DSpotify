/* The downloads index: which tracks are on the SD card, which albums and
 * playlists they came with, and where each one is in the download queue.
 * Pure C with no 3DS headers, tested in tests/native_logic_test.c; the
 * files themselves are dl_store.c's.
 *
 * On disk it is text, one entry per line, fields joined by DL_SEP (which no
 * Spotify name carries, and dl_put_field turns into a space anyway, like
 * any other control character that would break a line):
 *
 *   3dspotify-dl SEP 1        first line: format and version
 *   O SEP <username>          whose downloads these are
 *   S SEP <salt, hex>         the salt the stored keys are wrapped with
 *   T SEP <track fields...>   a track, see dl_put_track
 *   C SEP <collection...>     an album or a playlist
 *   R SEP <coll uri> SEP <track uri>   one track of one collection, in order
 *   E                         last line: the file was written whole
 *
 * A line that is not understood is skipped, so a damaged line costs that
 * entry and not the index. Another version is refused whole instead: a
 * newer build's index is not this build's to rewrite.
 *
 * Tracks and collections refer to each other by URI on disk and by
 * position in memory. Removing a track or a collection keeps the positions
 * in the refs right (dl_remove_track, dl_remove_collection). */
#ifndef DOWNLOADS_LOGIC_H
#define DOWNLOADS_LOGIC_H

#include "spotify_id.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DL_SEP '\x1f'
#define DL_MAGIC "3dspotify-dl"
#define DL_VERSION 1

/* Room for 1000 tracks, which at ~900 bytes each is under 1 MB of heap. */
#define DL_TRACKS_MAX 1000
#define DL_COLLS_MAX 100
/* A track may belong to several collections, so there are more refs than
 * tracks; a playlist is downloaded with up to 500. */
#define DL_REFS_MAX 3000
/* The longest line the index writes: a track with every field full. */
#define DL_LINE_MAX 1536

#define DL_SALT_LEN 16
/* The audio key as dl_store.c keeps it: nonce, the key encrypted, tag. */
#define DL_NONCE_LEN 12
#define DL_KEY_LEN 16
#define DL_TAG_LEN 16
#define DL_WRAPPED_LEN (DL_NONCE_LEN + DL_KEY_LEN + DL_TAG_LEN)

typedef enum {
	DL_QUEUED, DL_ACTIVE, DL_FAILED, DL_DONE
} dl_state;

typedef enum { DL_COLL_ALBUM, DL_COLL_PLAYLIST } dl_coll_kind;

/* One track: what local playback and the lists need, without the network. */
typedef struct {
	char     uri[SP_URI_MAX];
	char     name[128], artist[128], album[128];
	char     art_small[80], art_large[80];
	char     album_uri[SP_URI_MAX], artist_uri[SP_URI_MAX];
	int      duration_ms, track_number, disc_number, year;
	bool     is_explicit;
	int      format;               /* SP_FMT_OGG_* downloaded */
	uint32_t size;                 /* bytes of the whole file */
	uint8_t  file_id[20];
	bool     has_key;
	uint8_t  wrapped_key[DL_WRAPPED_LEN];
	dl_state state;
	char     error[96];            /* DL_FAILED: why */
	bool     standalone;           /* downloaded as a song of its own */
	uint32_t added_at;             /* for the history, newest first */
	uint32_t seq;                  /* queue order: lowest goes first */
} dl_track;

/* An album or a playlist; its tracks are the refs that name it. */
typedef struct {
	char         uri[SP_URI_MAX];
	dl_coll_kind kind;
	char         name[128], subtitle[128], art[80];
	int          total;            /* what the service says it holds */
	uint32_t     added_at;
} dl_collection;

typedef struct {
	uint16_t coll;
	uint16_t track;
} dl_ref;

typedef struct {
	char          owner[128];
	bool          has_salt;
	uint8_t       salt[DL_SALT_LEN];
	uint32_t      next_seq;
	int           track_count, coll_count, ref_count;
	dl_track      tracks[DL_TRACKS_MAX];
	dl_collection colls[DL_COLLS_MAX];
	dl_ref        refs[DL_REFS_MAX];
} dl_index;

static inline void dl_index_clear(dl_index *idx)
{
	memset(idx, 0, sizeof(*idx));
	idx->next_seq = 1;
}

/* ------------------------------------------------------------ lookups */

static inline int dl_find_track(const dl_index *idx, const char *uri)
{
	for (int i = 0; uri && i < idx->track_count; i++)
		if (strcmp(idx->tracks[i].uri, uri) == 0)
			return i;
	return -1;
}

static inline int dl_find_collection(const dl_index *idx, const char *uri)
{
	for (int i = 0; uri && i < idx->coll_count; i++)
		if (strcmp(idx->colls[i].uri, uri) == 0)
			return i;
	return -1;
}

/* How many collections hold track `t`. */
static inline int dl_track_refs(const dl_index *idx, int t)
{
	int n = 0;
	for (int i = 0; i < idx->ref_count; i++)
		if (idx->refs[i].track == t)
			n++;
	return n;
}

/* Track `t` is wanted by someone: a collection, or the user on its own. */
static inline bool dl_track_wanted(const dl_index *idx, int t)
{
	return idx->tracks[t].standalone || dl_track_refs(idx, t) > 0;
}

/* Collection `c`'s tracks, in their order, as positions in `out` (up to
 * `max`). Returns how many it holds, which may be more than `max`. */
static inline int dl_collection_tracks(const dl_index *idx, int c, int *out,
                                       int max)
{
	int n = 0;
	for (int i = 0; i < idx->ref_count; i++) {
		if (idx->refs[i].coll != c)
			continue;
		if (n < max)
			out[n] = idx->refs[i].track;
		n++;
	}
	return n;
}

/* "8 of 12": how many of collection `c`'s tracks are on the card, and how
 * many it has in the index. */
static inline void dl_collection_progress(const dl_index *idx, int c,
                                          int *done, int *count)
{
	*done = *count = 0;
	for (int i = 0; i < idx->ref_count; i++) {
		if (idx->refs[i].coll != c)
			continue;
		(*count)++;
		if (idx->tracks[idx->refs[i].track].state == DL_DONE)
			(*done)++;
	}
}

/* The next track to download: the queued one with the lowest seq, which is
 * the order things were asked for (and, within a collection, its order,
 * since its tracks are queued in it). Failed and active ones are skipped.
 * -1 when the queue is empty. */
static inline int dl_next_queued(const dl_index *idx)
{
	int best = -1;
	for (int i = 0; i < idx->track_count; i++) {
		const dl_track *t = &idx->tracks[i];
		if (t->state == DL_QUEUED &&
		    (best < 0 || t->seq < idx->tracks[best].seq))
			best = i;
	}
	return best;
}

/* The index belongs to the account that downloaded it. An index with no
 * owner yet (nothing downloaded) is anyone's; no username matches nothing. */
static inline bool dl_owner_matches(const char *index_owner,
                                    const char *username)
{
	if (!index_owner || !index_owner[0])
		return true;
	if (!username || !username[0])
		return false;
	return strcmp(index_owner, username) == 0;
}

/* ------------------------------------------------------------ changes */

/* Adds track `t` (its metadata; state and order are set here), queued.
 * One that is already there is left as it is, except that `standalone`
 * sticks once asked for. Returns its position, or -1 when full. */
static inline int dl_add_track(dl_index *idx, const dl_track *t,
                               bool standalone, uint32_t now)
{
	int at = dl_find_track(idx, t->uri);
	if (at >= 0) {
		if (standalone)
			idx->tracks[at].standalone = true;
		return at;
	}
	if (idx->track_count >= DL_TRACKS_MAX || !t->uri[0])
		return -1;
	at = idx->track_count++;
	dl_track *d = &idx->tracks[at];
	*d = *t;
	d->state = DL_QUEUED;
	d->error[0] = '\0';
	d->has_key = false;
	d->standalone = standalone;
	d->added_at = now;
	d->seq = idx->next_seq++;
	return at;
}

/* Adds collection `c` (no tracks yet). One that is already there keeps its
 * tracks and takes the new header. Returns its position, or -1 when full. */
static inline int dl_add_collection(dl_index *idx, const dl_collection *c,
                                    uint32_t now)
{
	int at = dl_find_collection(idx, c->uri);
	if (at < 0) {
		if (idx->coll_count >= DL_COLLS_MAX || !c->uri[0])
			return -1;
		at = idx->coll_count++;
		idx->colls[at] = *c;
		idx->colls[at].added_at = now;
		return at;
	}
	uint32_t added = idx->colls[at].added_at;
	idx->colls[at] = *c;
	idx->colls[at].added_at = added;
	return at;
}

/* Puts track `t` at the end of collection `c`, once. False when full. */
static inline bool dl_add_ref(dl_index *idx, int c, int t)
{
	for (int i = 0; i < idx->ref_count; i++)
		if (idx->refs[i].coll == c && idx->refs[i].track == t)
			return true;
	if (idx->ref_count >= DL_REFS_MAX)
		return false;
	idx->refs[idx->ref_count].coll = (uint16_t)c;
	idx->refs[idx->ref_count].track = (uint16_t)t;
	idx->ref_count++;
	return true;
}

/* Back in the queue, at its end: a retry, or a download cut short. */
static inline void dl_requeue(dl_index *idx, int t)
{
	idx->tracks[t].state = DL_QUEUED;
	idx->tracks[t].error[0] = '\0';
	idx->tracks[t].seq = idx->next_seq++;
}

/* Takes track `t` out of the index, with its refs; the positions after it
 * move down one and the refs follow. Its files are the caller's. */
static inline void dl_remove_track(dl_index *idx, int t)
{
	if (t < 0 || t >= idx->track_count)
		return;
	int w = 0;
	for (int i = 0; i < idx->ref_count; i++) {
		dl_ref r = idx->refs[i];
		if (r.track == t)
			continue;
		if (r.track > t)
			r.track--;
		idx->refs[w++] = r;
	}
	idx->ref_count = w;
	memmove(&idx->tracks[t], &idx->tracks[t + 1],
	        (size_t)(idx->track_count - t - 1) * sizeof(idx->tracks[0]));
	idx->track_count--;
}

/* Takes collection `c` out, and with it every track no one else wants:
 * not in another collection and not downloaded on its own. Their URIs go
 * to `orphans` (up to `max`) so the caller can delete their files; returns
 * how many there were. */
static inline int dl_remove_collection(dl_index *idx, int c,
                                       char (*orphans)[SP_URI_MAX], int max)
{
	if (c < 0 || c >= idx->coll_count)
		return 0;
	int w = 0;
	for (int i = 0; i < idx->ref_count; i++) {
		dl_ref r = idx->refs[i];
		if (r.coll == c)
			continue;
		if (r.coll > c)
			r.coll--;
		idx->refs[w++] = r;
	}
	idx->ref_count = w;
	memmove(&idx->colls[c], &idx->colls[c + 1],
	        (size_t)(idx->coll_count - c - 1) * sizeof(idx->colls[0]));
	idx->coll_count--;

	int n = 0;
	for (int t = idx->track_count - 1; t >= 0; t--) {
		if (dl_track_wanted(idx, t))
			continue;
		if (orphans && n < max)
			snprintf(orphans[n], SP_URI_MAX, "%s", idx->tracks[t].uri);
		n++;
		dl_remove_track(idx, t);
	}
	return n;
}

/* ---------------------------------------------------------- the file */

static inline void dl_hex(const uint8_t *p, size_t n, char *out)
{
	static const char HEX[] = "0123456789abcdef";
	for (size_t i = 0; i < n; i++) {
		out[i * 2] = HEX[p[i] >> 4];
		out[i * 2 + 1] = HEX[p[i] & 15];
	}
	out[n * 2] = '\0';
}

static inline int dl_hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Exactly `n` bytes of hex, or false. */
static inline bool dl_unhex(const char *s, uint8_t *out, size_t n)
{
	if (strlen(s) != n * 2)
		return false;
	for (size_t i = 0; i < n; i++) {
		int hi = dl_hex_digit(s[i * 2]), lo = dl_hex_digit(s[i * 2 + 1]);
		if (hi < 0 || lo < 0)
			return false;
		out[i] = (uint8_t)(hi << 4 | lo);
	}
	return true;
}

/* Appends DL_SEP and `s` to `line` (length `*n`, capacity `cap`), control
 * characters as spaces. False when it does not fit. */
static inline bool dl_put_field(char *line, size_t *n, size_t cap,
                                const char *s)
{
	if (*n + 1 >= cap)
		return false;
	line[(*n)++] = DL_SEP;
	for (; s && *s; s++) {
		if (*n + 1 >= cap)
			return false;
		unsigned char c = (unsigned char)*s;
		line[(*n)++] = c < 0x20 || c == 0x7f ? ' ' : (char)c;
	}
	line[*n] = '\0';
	return true;
}

static inline bool dl_put_uint(char *line, size_t *n, size_t cap,
                               unsigned long v)
{
	char num[24];
	snprintf(num, sizeof(num), "%lu", v);
	return dl_put_field(line, n, cap, num);
}

/* The fields of a track line, in order. Adding one goes at the end, so an
 * older line still reads (the missing ones stay zero). */
enum {
	DLT_URI = 1, DLT_NAME, DLT_ARTIST, DLT_ALBUM, DLT_ART_SMALL,
	DLT_ART_LARGE, DLT_ALBUM_URI, DLT_ARTIST_URI, DLT_DURATION, DLT_TRACK_NO,
	DLT_DISC, DLT_YEAR, DLT_EXPLICIT, DLT_FORMAT, DLT_SIZE, DLT_FILE_ID,
	DLT_KEY, DLT_STATE, DLT_STANDALONE, DLT_ADDED, DLT_SEQ, DLT_ERROR,
	DLT_FIELDS
};

static inline bool dl_put_track(const dl_track *t, char *line, size_t cap)
{
	size_t n = 0;
	char file_id[41], key[DL_WRAPPED_LEN * 2 + 1];
	dl_hex(t->file_id, sizeof(t->file_id), file_id);
	if (t->has_key)
		dl_hex(t->wrapped_key, sizeof(t->wrapped_key), key);
	else
		key[0] = '\0';
	if (cap < 2)
		return false;
	line[n++] = 'T';
	line[n] = '\0';
	return dl_put_field(line, &n, cap, t->uri) &&
	       dl_put_field(line, &n, cap, t->name) &&
	       dl_put_field(line, &n, cap, t->artist) &&
	       dl_put_field(line, &n, cap, t->album) &&
	       dl_put_field(line, &n, cap, t->art_small) &&
	       dl_put_field(line, &n, cap, t->art_large) &&
	       dl_put_field(line, &n, cap, t->album_uri) &&
	       dl_put_field(line, &n, cap, t->artist_uri) &&
	       dl_put_uint(line, &n, cap, (unsigned long)t->duration_ms) &&
	       dl_put_uint(line, &n, cap, (unsigned long)t->track_number) &&
	       dl_put_uint(line, &n, cap, (unsigned long)t->disc_number) &&
	       dl_put_uint(line, &n, cap, (unsigned long)t->year) &&
	       dl_put_uint(line, &n, cap, t->is_explicit) &&
	       dl_put_uint(line, &n, cap, (unsigned long)t->format) &&
	       dl_put_uint(line, &n, cap, t->size) &&
	       dl_put_field(line, &n, cap, file_id) &&
	       dl_put_field(line, &n, cap, key) &&
	       dl_put_uint(line, &n, cap, (unsigned long)t->state) &&
	       dl_put_uint(line, &n, cap, t->standalone) &&
	       dl_put_uint(line, &n, cap, t->added_at) &&
	       dl_put_uint(line, &n, cap, t->seq) &&
	       dl_put_field(line, &n, cap, t->error);
}

static inline bool dl_put_collection(const dl_collection *c, char *line,
                                     size_t cap)
{
	size_t n = 0;
	if (cap < 2)
		return false;
	line[n++] = 'C';
	line[n] = '\0';
	return dl_put_field(line, &n, cap, c->uri) &&
	       dl_put_field(line, &n, cap,
	                    c->kind == DL_COLL_PLAYLIST ? "p" : "a") &&
	       dl_put_field(line, &n, cap, c->name) &&
	       dl_put_field(line, &n, cap, c->subtitle) &&
	       dl_put_field(line, &n, cap, c->art) &&
	       dl_put_uint(line, &n, cap, (unsigned long)c->total) &&
	       dl_put_uint(line, &n, cap, c->added_at);
}

/* Writes the index a line at a time: `*cursor` starts at 0, and each call
 * puts the next line (no newline) in `line` and returns true, until there
 * are none left. A line that does not fit `line` is skipped. The writer
 * needs no buffer the size of the whole index. */
static inline bool dl_index_line(const dl_index *idx, int *cursor, char *line,
                                 size_t cap)
{
	for (;;) {
		int c = (*cursor)++;
		line[0] = '\0';
		if (c == 0) {
			snprintf(line, cap, "%s%c%d", DL_MAGIC, DL_SEP, DL_VERSION);
			return true;
		}
		if (c == 1) {
			snprintf(line, cap, "O");
			size_t n = 1;
			if (dl_put_field(line, &n, cap, idx->owner))
				return true;
			continue;
		}
		if (c == 2) {
			if (!idx->has_salt)
				continue;
			char hex[DL_SALT_LEN * 2 + 1];
			dl_hex(idx->salt, DL_SALT_LEN, hex);
			snprintf(line, cap, "S%c%s", DL_SEP, hex);
			return true;
		}
		c -= 3;
		if (c < idx->track_count) {
			if (dl_put_track(&idx->tracks[c], line, cap))
				return true;
			continue;
		}
		c -= idx->track_count;
		if (c < idx->coll_count) {
			if (dl_put_collection(&idx->colls[c], line, cap))
				return true;
			continue;
		}
		c -= idx->coll_count;
		if (c < idx->ref_count) {
			const dl_ref *r = &idx->refs[c];
			snprintf(line, cap, "R%c%s%c%s", DL_SEP, idx->colls[r->coll].uri,
			         DL_SEP, idx->tracks[r->track].uri);
			return true;
		}
		c -= idx->ref_count;
		if (c == 0) {
			snprintf(line, cap, "E");
			return true;
		}
		return false;
	}
}

/* Splits `line` (changed in place) at DL_SEP into up to `max` fields. The
 * first is the line's kind. Returns how many. */
static inline int dl_split(char *line, char **fields, int max)
{
	int n = 0;
	char *p = line;
	while (n < max) {
		fields[n++] = p;
		char *sep = strchr(p, DL_SEP);
		if (!sep)
			break;
		*sep = '\0';
		p = sep + 1;
	}
	return n;
}

static inline void dl_copy(char *dst, size_t cap, const char *src)
{
	snprintf(dst, cap, "%s", src ? src : "");
}

static inline unsigned long dl_num(const char *s)
{
	return s && *s ? strtoul(s, NULL, 10) : 0;
}

/* Where reading a file has got to: dl_index_parse_line keeps it. */
typedef struct {
	bool header;    /* the first line was this format and version */
	bool refused;   /* the first line was not: nothing else is read */
	bool ended;     /* the E line was there: the file is whole */
} dl_parse_state;

/* Reads one line (changed in place; no newline needed) into `idx`, which
 * starts dl_index_clear'ed. A download that was running when the file was
 * written goes back to the queue: whatever ran it is gone. */
static inline void dl_index_parse_line(dl_index *idx, dl_parse_state *st,
                                       char *line)
{
	line[strcspn(line, "\r\n")] = '\0';
	if (st->refused || st->ended)
		return;
	char *f[DLT_FIELDS + 1];
	int n = dl_split(line, f, DLT_FIELDS + 1);

	if (!st->header) {
		if (n >= 2 && strcmp(f[0], DL_MAGIC) == 0 &&
		    dl_num(f[1]) == DL_VERSION)
			st->header = true;
		else
			st->refused = true;
		return;
	}
	if (strcmp(f[0], "E") == 0) {
		st->ended = true;
	} else if (strcmp(f[0], "O") == 0 && n >= 2) {
		dl_copy(idx->owner, sizeof(idx->owner), f[1]);
	} else if (strcmp(f[0], "S") == 0 && n >= 2) {
		idx->has_salt = dl_unhex(f[1], idx->salt, DL_SALT_LEN);
	} else if (strcmp(f[0], "T") == 0 && n > DLT_URI && f[DLT_URI][0]) {
		if (idx->track_count >= DL_TRACKS_MAX ||
		    dl_find_track(idx, f[DLT_URI]) >= 0)
			return;
		const char *v[DLT_FIELDS];
		for (int i = 0; i < DLT_FIELDS; i++)
			v[i] = i < n ? f[i] : "";
		dl_track *t = &idx->tracks[idx->track_count];
		memset(t, 0, sizeof(*t));
		dl_copy(t->uri, sizeof(t->uri), v[DLT_URI]);
		dl_copy(t->name, sizeof(t->name), v[DLT_NAME]);
		dl_copy(t->artist, sizeof(t->artist), v[DLT_ARTIST]);
		dl_copy(t->album, sizeof(t->album), v[DLT_ALBUM]);
		dl_copy(t->art_small, sizeof(t->art_small), v[DLT_ART_SMALL]);
		dl_copy(t->art_large, sizeof(t->art_large), v[DLT_ART_LARGE]);
		dl_copy(t->album_uri, sizeof(t->album_uri), v[DLT_ALBUM_URI]);
		dl_copy(t->artist_uri, sizeof(t->artist_uri), v[DLT_ARTIST_URI]);
		t->duration_ms = (int)dl_num(v[DLT_DURATION]);
		t->track_number = (int)dl_num(v[DLT_TRACK_NO]);
		t->disc_number = (int)dl_num(v[DLT_DISC]);
		t->year = (int)dl_num(v[DLT_YEAR]);
		t->is_explicit = dl_num(v[DLT_EXPLICIT]) != 0;
		t->format = (int)dl_num(v[DLT_FORMAT]);
		t->size = (uint32_t)dl_num(v[DLT_SIZE]);
		dl_unhex(v[DLT_FILE_ID], t->file_id, sizeof(t->file_id));
		t->has_key = dl_unhex(v[DLT_KEY], t->wrapped_key,
		                      sizeof(t->wrapped_key));
		unsigned long state = dl_num(v[DLT_STATE]);
		t->state = state <= DL_DONE ? (dl_state)state : DL_QUEUED;
		if (t->state == DL_ACTIVE)
			t->state = DL_QUEUED;
		/* Done without a key is a file that cannot play: fetch again. */
		if (t->state == DL_DONE && !t->has_key)
			t->state = DL_QUEUED;
		t->standalone = dl_num(v[DLT_STANDALONE]) != 0;
		t->added_at = (uint32_t)dl_num(v[DLT_ADDED]);
		t->seq = (uint32_t)dl_num(v[DLT_SEQ]);
		dl_copy(t->error, sizeof(t->error), v[DLT_ERROR]);
		if (t->seq >= idx->next_seq)
			idx->next_seq = t->seq + 1;
		idx->track_count++;
	} else if (strcmp(f[0], "C") == 0 && n >= 3 && f[1][0]) {
		if (idx->coll_count >= DL_COLLS_MAX ||
		    dl_find_collection(idx, f[1]) >= 0)
			return;
		dl_collection *c = &idx->colls[idx->coll_count];
		memset(c, 0, sizeof(*c));
		dl_copy(c->uri, sizeof(c->uri), f[1]);
		c->kind = strcmp(f[2], "p") == 0 ? DL_COLL_PLAYLIST : DL_COLL_ALBUM;
		dl_copy(c->name, sizeof(c->name), n > 3 ? f[3] : "");
		dl_copy(c->subtitle, sizeof(c->subtitle), n > 4 ? f[4] : "");
		dl_copy(c->art, sizeof(c->art), n > 5 ? f[5] : "");
		c->total = (int)dl_num(n > 6 ? f[6] : "");
		c->added_at = (uint32_t)dl_num(n > 7 ? f[7] : "");
		idx->coll_count++;
	} else if (strcmp(f[0], "R") == 0 && n >= 3) {
		/* Refs come after what they name, so both are known by now. */
		int c = dl_find_collection(idx, f[1]);
		int t = dl_find_track(idx, f[2]);
		if (c >= 0 && t >= 0)
			dl_add_ref(idx, c, t);
	}
}

/* A whole file in memory (for the tests; dl_store.c reads line by line).
 * Returns true when it was this format and version; `*whole` says the end
 * marker was there. */
static inline bool dl_index_parse(const char *text, size_t len, dl_index *idx,
                                  bool *whole)
{
	dl_index_clear(idx);
	dl_parse_state st = {0};
	char line[DL_LINE_MAX];
	size_t at = 0;
	while (at < len) {
		size_t end = at;
		while (end < len && text[end] != '\n')
			end++;
		size_t n = end - at;
		if (n < sizeof(line)) {
			memcpy(line, text + at, n);
			line[n] = '\0';
			dl_index_parse_line(idx, &st, line);
		}
		at = end + 1;
	}
	if (whole)
		*whole = st.ended;
	return st.header;
}

/* ------------------------------------------------- the engine's states */

/* A download starts: its track leaves the queue. */
static inline void dl_mark_active(dl_index *idx, int t)
{
	idx->tracks[t].state = DL_ACTIVE;
	idx->tracks[t].error[0] = '\0';
}

/* The file is on the card, whole and checked. */
static inline void dl_mark_done(dl_index *idx, int t, uint32_t size)
{
	idx->tracks[t].state = DL_DONE;
	idx->tracks[t].size = size;
	idx->tracks[t].error[0] = '\0';
}

/* Given up on until the user retries it; `why` is shown. */
static inline void dl_mark_failed(dl_index *idx, int t, const char *why)
{
	idx->tracks[t].state = DL_FAILED;
	dl_copy(idx->tracks[t].error, sizeof(idx->tracks[t].error), why);
}

/* A download that was stopped (the player needed the network, the session
 * ended) waits again, in its place: it resumes from its .part. */
static inline void dl_mark_paused(dl_index *idx, int t)
{
	if (idx->tracks[t].state == DL_ACTIVE)
		idx->tracks[t].state = DL_QUEUED;
}

/* The audio key arrived, wrapped, with the file it opens. Kept at once,
 * even if the download then stops: resuming needs no new key. */
static inline void dl_set_key(dl_index *idx, int t,
                              const uint8_t wrapped[DL_WRAPPED_LEN],
                              const uint8_t file_id[20], int format)
{
	dl_track *d = &idx->tracks[t];
	memcpy(d->wrapped_key, wrapped, DL_WRAPPED_LEN);
	memcpy(d->file_id, file_id, sizeof(d->file_id));
	d->format = format;
	d->has_key = true;
}

/* The user no longer wants track `t` on its own. It stays while a
 * collection holds it; otherwise it leaves the index. Returns true when it
 * left, so its files go too. */
static inline bool dl_unwant_track(dl_index *idx, int t)
{
	if (t < 0 || t >= idx->track_count)
		return false;
	idx->tracks[t].standalone = false;
	if (dl_track_refs(idx, t) > 0)
		return false;
	dl_remove_track(idx, t);
	return true;
}

/* ---------------------------------------------------- the key allowance
 *
 * The access point gives an account about 15 audio keys in a row, then
 * refuses with 0x0002 on any session until it refills, about one every
 * 30 s (probe_offline 0.2, on the console). The player draws on the same
 * allowance, so the engine keeps a model of it, counts the player's keys
 * too, and never takes it below a reserve the player can still use. */
#define DL_KEY_BUCKET 15
#define DL_KEY_REFILL_MS 30000
#define DL_KEY_RESERVE 5
/* After a 0x0002: the model was wrong, so the engine waits this long. */
#define DL_KEY_BACKOFF_MS 60000
/* The refusal code that means "too many keys", not "not this track". */
#define DL_KEY_LIMIT_CODE 0x0002

typedef struct {
	int      tokens;
	uint64_t refilled_at;     /* when the last whole token came */
	uint64_t blocked_until;   /* after a refusal */
} dl_bucket;

static inline void dl_bucket_init(dl_bucket *b, uint64_t now)
{
	b->tokens = DL_KEY_BUCKET;
	b->refilled_at = now;
	b->blocked_until = 0;
}

static inline void dl_bucket_refill(dl_bucket *b, uint64_t now)
{
	if (b->tokens >= DL_KEY_BUCKET || now <= b->refilled_at) {
		if (b->tokens >= DL_KEY_BUCKET)
			b->refilled_at = now;
		return;
	}
	uint64_t whole = (now - b->refilled_at) / DL_KEY_REFILL_MS;
	if (whole == 0)
		return;
	if (whole >= (uint64_t)(DL_KEY_BUCKET - b->tokens)) {
		b->tokens = DL_KEY_BUCKET;
		b->refilled_at = now;
	} else {
		b->tokens += (int)whole;
		b->refilled_at += whole * DL_KEY_REFILL_MS;
	}
}

/* A key was asked for, by the engine or by the player. */
static inline void dl_bucket_take(dl_bucket *b, uint64_t now)
{
	dl_bucket_refill(b, now);
	if (b->tokens > 0)
		b->tokens--;
}

/* The access point said 0x0002: nothing is left, whatever the model said. */
static inline void dl_bucket_refused(dl_bucket *b, uint64_t now)
{
	b->tokens = 0;
	b->refilled_at = now;
	b->blocked_until = now + DL_KEY_BACKOFF_MS;
}

/* Whether the engine may ask for a key now, leaving the reserve. */
static inline bool dl_bucket_engine_may_take(dl_bucket *b, uint64_t now)
{
	dl_bucket_refill(b, now);
	return now >= b->blocked_until && b->tokens > DL_KEY_RESERVE;
}

/* --------------------------------------------------- retries and pauses */

/* Network failures of the same track: wait 5 s, 15 s, 60 s, then give up
 * on it (0). The first failure is `failures` 1. */
static inline uint32_t dl_net_retry_ms(int failures)
{
	static const uint32_t STEPS[] = { 5000, 15000, 60000 };
	if (failures < 1)
		return 0;
	return failures <= 3 ? STEPS[failures - 1] : 0;
}

/* The player is using the network for the track that plays: preparing it
 * (token, access point, key) or still downloading it. The engine then
 * stops its request and starts none, so two audio downloads never share
 * the Wi-Fi nor two large buffers the memory. */
static inline bool dl_player_busy(bool preparing, bool playing,
                                  bool stream_whole)
{
	return preparing || (playing && !stream_whole);
}

/* ------------------------------------------------------------ buttons */

/* What a track's download button says, and does on A. Only DOWNLOAD,
 * RETRY and REMOVE act; the rest are disabled (STYLES.md 6.1). */
typedef enum {
	DL_BUTTON_DOWNLOAD,    /* not in the index: queue it */
	DL_BUTTON_QUEUED,      /* waiting its turn */
	DL_BUTTON_PROGRESS,    /* downloading, with its share */
	DL_BUTTON_RETRY,       /* failed: back in the queue */
	DL_BUTTON_REMOVE,      /* on the card as a song of its own: off it */
	DL_BUTTON_HELD         /* on the card for an album or playlist only */
} dl_button;

static inline dl_button dl_track_button(bool known, dl_state state,
                                        bool standalone)
{
	if (!known)
		return DL_BUTTON_DOWNLOAD;
	switch (state) {
	case DL_QUEUED:
		return DL_BUTTON_QUEUED;
	case DL_ACTIVE:
		return DL_BUTTON_PROGRESS;
	case DL_FAILED:
		return DL_BUTTON_RETRY;
	default:
		return standalone ? DL_BUTTON_REMOVE : DL_BUTTON_HELD;
	}
}

static inline bool dl_button_enabled(dl_button b)
{
	return b == DL_BUTTON_DOWNLOAD || b == DL_BUTTON_RETRY ||
	       b == DL_BUTTON_REMOVE;
}

/* An album's or a playlist's button: download it all; it is being
 * resolved into tracks; "8 of 12"; all on the card. Only DOWNLOAD acts. */
typedef enum {
	DL_COLL_BUTTON_DOWNLOAD,
	DL_COLL_BUTTON_WAITING,
	DL_COLL_BUTTON_PROGRESS,
	DL_COLL_BUTTON_DONE
} dl_coll_button;

static inline dl_coll_button dl_collection_button(bool pending, bool known,
                                                  int done, int count)
{
	if (pending)
		return DL_COLL_BUTTON_WAITING;
	if (!known)
		return DL_COLL_BUTTON_DOWNLOAD;
	return done >= count ? DL_COLL_BUTTON_DONE : DL_COLL_BUTTON_PROGRESS;
}

#endif
