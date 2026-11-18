#include "meta_parse.h"
#include "json3ds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_str(char *dst, size_t cap, const pbv *v)
{
	size_t n = v->len < cap - 1 ? v->len : cap - 1;
	memcpy(dst, v->p, n);
	dst[n] = '\0';
}

/* copy_str, but a cut never splits a UTF-8 sequence: playlist names are
 * user text, emoji included, and half a character draws as garbage. */
static void copy_utf8(char *dst, size_t cap, const pbv *v)
{
	size_t n = v->len < cap - 1 ? v->len : cap - 1;
	if (n < v->len)
		while (n > 0 && (v->p[n] & 0xC0) == 0x80)
			n--;
	memcpy(dst, v->p, n);
	dst[n] = '\0';
}

/* A 16-byte gid in `field` of `msg` as a URI of `kind`; `out` stays empty
 * when there is none. */
static bool read_gid_uri(const pbv *msg, uint32_t field, sp_kind kind,
                         char *out, size_t cap)
{
	pbv gid;
	if (!pb_get_bytes(msg->p, msg->len, field, &gid) || gid.len != 16)
		return false;
	return sp_uri_from_gid(kind, gid.p, out, cap);
}

/* Collects the image ids of a container holding repeated Image entries under
 * `field`, indexed by the Image.size enum: 0=DEFAULT (300px), 1=SMALL (64px),
 * 2=LARGE (640px). The id becomes the URL in lowercase hex. */
static void read_image_ids(const pbv *container, uint32_t field, char ids[3][41])
{
	size_t at = 0;
	pbv img;
	while (pb_next_bytes(container, field, &at, &img)) {
		pbv fid;
		uint64_t size = 0;
		pb_get_varint(img.p, img.len, 2, &size);
		if (pb_get_bytes(img.p, img.len, 1, &fid) && fid.len == 20 &&
		    size <= 2 && !ids[size][0]) {
			static const char HEX[] = "0123456789abcdef";
			for (int i = 0; i < 20; i++) {
				ids[size][i * 2] = HEX[fid.p[i] >> 4];
				ids[size][i * 2 + 1] = HEX[fid.p[i] & 15];
			}
			ids[size][40] = '\0';
		}
	}
}

/* Album covers and artist portraits share the shape: an ImageGroup at
 * `group_field`, or, in older metadata, repeated Images straight on the
 * message at `list_field`. Falls back across sizes, so a missing SMALL does
 * not cost a list its thumbnails. */
static void read_art(const pbv *msg, uint32_t group_field, uint32_t list_field,
                     char *small_out, char *large_out, size_t cap)
{
	char ids[3][41] = {{0}};
	pbv group;
	if (pb_get_bytes(msg->p, msg->len, group_field, &group))
		read_image_ids(&group, 1, ids);
	if (!ids[0][0] && !ids[1][0] && !ids[2][0])
		read_image_ids(msg, list_field, ids);

	const char *small = ids[1][0] ? ids[1] : ids[2][0] ? ids[2] : ids[0];
	const char *large = ids[2][0] ? ids[2] : ids[0][0] ? ids[0] : ids[1];
	if (small[0])
		snprintf(small_out, cap, "https://i.scdn.co/image/%s", small);
	if (large[0])
		snprintf(large_out, cap, "https://i.scdn.co/image/%s", large);
}

/* Date.year from the Date at field 6 of an Album. */
static int read_year(const pbv *album)
{
	pbv date;
	int64_t year = 0;
	if (pb_get_bytes(album->p, album->len, 6, &date))
		pb_get_sint(date.p, date.len, 1, &year);
	return year > 0 && year < 10000 ? (int)year : 0;
}

/* The whole Date at field 6 of an Album as yyyymmdd: 0 without a year, and
 * month or day 0 when missing or out of range. */
static int read_date(const pbv *album)
{
	pbv date;
	int year = read_year(album);
	if (!year || !pb_get_bytes(album->p, album->len, 6, &date))
		return 0;
	int64_t month = 0, day = 0;
	pb_get_sint(date.p, date.len, 2, &month);
	pb_get_sint(date.p, date.len, 3, &day);
	if (month < 1 || month > 12)
		month = day = 0;
	if (day < 1 || day > 31)
		day = 0;
	return year * 10000 + (int)month * 100 + (int)day;
}

/* The tracks of a Disc or a TopTracks, each only a gid, as URIs. */
static void read_track_gids(const pbv *msg, uint32_t field, char uris[][SP_URI_MAX],
                            int max, int *count, int *total)
{
	size_t at = 0;
	pbv tr;
	while (pb_next_bytes(msg, field, &at, &tr)) {
		char uri[SP_URI_MAX];
		if (!read_gid_uri(&tr, 1, SP_KIND_TRACK, uri, sizeof(uri)))
			continue;
		if (total)
			(*total)++;
		if (*count < max)
			memcpy(uris[(*count)++], uri, sizeof(uri));
	}
}

bool sp_parse_entity(const uint8_t *body, size_t len, pbv *entity)
{
	pbv arr, ed, any;
	return pb_get_bytes(body, len, 2, &arr) &&
	       pb_get_bytes(arr.p, arr.len, 3, &ed) &&
	       pb_get_bytes(ed.p, ed.len, 3, &any) &&
	       pb_get_bytes(any.p, any.len, 2, entity);
}

/* A Track's AudioFile list (field 12) into out->files. */
static void read_files(const pbv *t, sp_track *out)
{
	size_t at = 0;
	pbv file;
	while (out->file_count < SP_MAX_FILES && pb_next_bytes(t, 12, &at, &file)) {
		pbv fid;
		uint64_t fmt = 0;
		if (pb_get_bytes(file.p, file.len, 1, &fid) && fid.len == 20) {
			memcpy(out->files[out->file_count].id, fid.p, 20);
			pb_get_varint(file.p, file.len, 2, &fmt);
			out->files[out->file_count].format = (int)fmt;
			out->file_count++;
		}
	}
}

void sp_parse_track(const pbv *t, sp_track *out)
{
	pbv v;
	if (pb_get_bytes(t->p, t->len, 1, &v) && v.len == 16) {
		memcpy(out->gid, v.p, 16);
		out->has_gid = true;
	}
	if (pb_get_bytes(t->p, t->len, 2, &v))
		copy_str(out->name, sizeof(out->name), &v);

	int64_t n = 0;
	/* duration, number and disc_number are sint32: zigzag, not a plain
	 * varint. Read the wrong way they come back doubled, which reads as a
	 * long-but-believable track rather than as an error. */
	if (pb_get_sint(t->p, t->len, 7, &n))
		out->duration_ms = (int)n;
	if (pb_get_sint(t->p, t->len, 5, &n))
		out->track_number = (int)n;
	if (pb_get_sint(t->p, t->len, 6, &n))
		out->disc_number = (int)n;
	uint64_t flag = 0;
	out->is_explicit = pb_get_varint(t->p, t->len, 9, &flag) && flag != 0;

	if (pb_get_bytes(t->p, t->len, 3, &v)) {          /* Track.album */
		pbv an;
		if (pb_get_bytes(v.p, v.len, 2, &an))
			copy_str(out->album, sizeof(out->album), &an);
		read_gid_uri(&v, 1, SP_KIND_ALBUM, out->album_uri,
		             sizeof(out->album_uri));
		out->year = read_year(&v);
		read_art(&v, 17, 9, out->art_small, out->art_large,
		         sizeof(out->art_small));
	}
	if (pb_get_bytes(t->p, t->len, 4, &v)) {          /* first Artist */
		pbv an;
		/* Artist.gid is field 1 and Artist.name is field 2; reading the
		 * first field as the name would hand back raw bytes. */
		if (pb_get_bytes(v.p, v.len, 2, &an))
			copy_str(out->artist, sizeof(out->artist), &an);
		read_gid_uri(&v, 1, SP_KIND_ARTIST, out->artist_uri,
		             sizeof(out->artist_uri));
	}

	memcpy(out->audio_gid, out->gid, sizeof(out->audio_gid));
	read_files(t, out);

	/* A Track with no files of its own may carry them on an alternative,
	 * a Track of its own (field 13) for the same recording: the first one
	 * that has files stands in, and the audio key then names it. */
	size_t at = 0;
	pbv alt, gid;
	while (out->file_count == 0 && pb_next_bytes(t, 13, &at, &alt)) {
		if (!pb_get_bytes(alt.p, alt.len, 1, &gid) || gid.len != 16)
			continue;
		read_files(&alt, out);
		if (out->file_count)
			memcpy(out->audio_gid, gid.p, 16);
	}
}

static bool same_uri(const pbv *v, const char *uri)
{
	return strlen(uri) == v->len && memcmp(v->p, uri, v->len) == 0;
}

/* The asked slot an entry belongs in: the first free one with its URI, or,
 * with `any_free`, the first free one at all. -1 when there is none. A slot
 * is taken once its uri is set. */
static int batch_slot(const pbv *uri, const char *const *uris, int n,
                      const sp_track *out, bool any_free)
{
	for (int j = 0; j < n; j++)
		if (!out[j].uri[0] && (any_free || (uri && same_uri(uri, uris[j]))))
			return j;
	return -1;
}

static bool batch_placed(const pbv *uri, const char *const *uris, int n,
                         const sp_track *out)
{
	for (int j = 0; j < n; j++)
		if (out[j].uri[0] && same_uri(uri, uris[j]))
			return true;
	return false;
}

static void batch_fill(const pbv *ed, int slot, const char *const *uris,
                       sp_track *out)
{
	pbv any, track;
	snprintf(out[slot].uri, sizeof(out[slot].uri), "%s", uris[slot]);
	if (pb_get_bytes(ed->p, ed->len, 3, &any) &&
	    pb_get_bytes(any.p, any.len, 2, &track))
		sp_parse_track(&track, &out[slot]);
}

int sp_parse_track_batch(const uint8_t *body, size_t len,
                         const char *const *uris, int n, sp_track *out)
{
	for (int j = 0; j < n; j++)
		memset(&out[j], 0, sizeof(out[j]));

	/* One EntityExtensionDataArray per extension kind; one kind is asked. */
	pbv arr;
	if (n <= 0 || !pb_get_bytes(body, len, 2, &arr))
		return 0;

	/* First the entries that name a track asked for, so that a stray one
	 * cannot take a slot that belongs to a later entry... */
	size_t at = 0;
	pbv ed, uri;
	while (pb_next_bytes(&arr, 3, &at, &ed)) {
		if (!pb_get_bytes(ed.p, ed.len, 2, &uri))
			continue;
		int slot = batch_slot(&uri, uris, n, out, false);
		if (slot >= 0)
			batch_fill(&ed, slot, uris, out);
	}
	/* ...then the strays, into whatever is left. */
	at = 0;
	while (pb_next_bytes(&arr, 3, &at, &ed)) {
		bool has_uri = pb_get_bytes(ed.p, ed.len, 2, &uri);
		if (has_uri && batch_placed(&uri, uris, n, out))
			continue;
		int slot = batch_slot(NULL, uris, n, out, true);
		if (slot < 0)
			break;
		batch_fill(&ed, slot, uris, out);
	}

	int filled = 0;
	for (int j = 0; j < n; j++) {
		if (!out[j].uri[0])
			continue;
		if (filled != j)
			out[filled] = out[j];
		filled++;
	}
	return filled;
}

bool sp_parse_album(const pbv *a, sp_album *out)
{
	memset(out, 0, sizeof(*out));
	if (!read_gid_uri(a, 1, SP_KIND_ALBUM, out->uri, sizeof(out->uri)))
		return false;

	pbv v;
	if (pb_get_bytes(a->p, a->len, 2, &v))
		copy_str(out->name, sizeof(out->name), &v);
	if (pb_get_bytes(a->p, a->len, 3, &v)) {          /* first Artist */
		pbv an;
		if (pb_get_bytes(v.p, v.len, 2, &an))
			copy_str(out->artist, sizeof(out->artist), &an);
		read_gid_uri(&v, 1, SP_KIND_ARTIST, out->artist_uri,
		             sizeof(out->artist_uri));
	}
	uint64_t type = 0;
	if (pb_get_varint(a->p, a->len, 4, &type) && type <= SP_ALBUM_EP)
		out->type = (int)type;
	if (pb_get_bytes(a->p, a->len, 5, &v))
		copy_str(out->label, sizeof(out->label), &v);
	out->year = read_year(a);
	read_art(a, 17, 9, out->art_small, out->art_large, sizeof(out->art_small));

	/* Discs in the order sent, which is disc order; the list numbers the
	 * tracks straight through. */
	size_t at = 0;
	pbv disc;
	while (pb_next_bytes(a, 11, &at, &disc))
		read_track_gids(&disc, 3, out->track_uris, SP_ALBUM_MAX_TRACKS,
		                &out->track_count, &out->total_tracks);
	return true;
}

static void read_releases(const pbv *a, sp_artist *out);

bool sp_parse_artist(const pbv *a, const char *country, sp_artist *out)
{
	memset(out, 0, sizeof(*out));
	if (!read_gid_uri(a, 1, SP_KIND_ARTIST, out->uri, sizeof(out->uri)))
		return false;

	pbv v;
	if (pb_get_bytes(a->p, a->len, 2, &v))
		copy_str(out->name, sizeof(out->name), &v);
	read_art(a, 17, 11, out->art_small, out->art_large, sizeof(out->art_small));

	pbv group, chosen = { NULL, 0 };
	size_t at = 0;
	while (pb_next_bytes(a, 4, &at, &group)) {
		if (!chosen.p)
			chosen = group;
		pbv c;
		if (country && country[0] && pb_get_bytes(group.p, group.len, 1, &c) &&
		    c.len == strlen(country) && memcmp(c.p, country, c.len) == 0) {
			chosen = group;
			break;
		}
	}
	if (chosen.p)
		read_track_gids(&chosen, 2, out->track_uris, SP_ARTIST_MAX_TRACKS,
		                &out->track_count, NULL);

	/* Several Biography may come; the first with text is the one. */
	pbv bio, text;
	at = 0;
	while (pb_next_bytes(a, 12, &at, &bio)) {
		if (pb_get_bytes(bio.p, bio.len, 1, &text) && text.len) {
			sp_bio_clean((const char *)text.p, text.len, out->bio,
			             sizeof(out->bio));
			break;
		}
	}
	read_releases(a, out);
	return true;
}

/* -------------------------------------------------------------- releases */

/* What an Album says about itself: name, type, date and covers. `uri`,
 * `group` and `order` are the caller's. */
static void fill_release(const pbv *album, sp_release *r)
{
	pbv v;
	if (pb_get_bytes(album->p, album->len, 2, &v))
		copy_utf8(r->name, sizeof(r->name), &v);
	uint64_t type = 0;
	if (pb_get_varint(album->p, album->len, 4, &type) && type <= SP_ALBUM_EP)
		r->type = (int)type;
	r->date = read_date(album);
	read_art(album, 17, 9, r->art_small, r->art_large, sizeof(r->art_small));
}

static void read_releases(const pbv *a, sp_artist *out)
{
	/* album_group 5, single_group 6, compilation_group 7, one AlbumGroup
	 * from each in turn: each group comes newest first, so the cap cuts
	 * the oldest of every kind, not a whole kind. */
	size_t at[3] = { 0, 0, 0 };
	bool open[3] = { true, true, true };
	int left = 3;
	while (left > 0 && out->release_count < SP_ARTIST_RELEASES_MAX) {
		for (int g = 0; g < 3; g++) {
			if (!open[g] || out->release_count >= SP_ARTIST_RELEASES_MAX)
				continue;
			pbv ag, album;
			if (!pb_next_bytes(a, 5 + (uint32_t)g, &at[g], &ag)) {
				open[g] = false;
				left--;
				continue;
			}
			sp_release *r = &out->releases[out->release_count];
			/* The first Album: the others are the same release for other
			 * markets. */
			if (!pb_get_bytes(ag.p, ag.len, 1, &album) ||
			    !read_gid_uri(&album, 1, SP_KIND_ALBUM, r->uri,
			                  sizeof(r->uri)))
				continue;
			fill_release(&album, r);
			r->group = g;
			r->order = out->release_count++;
		}
	}
}

int sp_release_cmp(const void *pa, const void *pb_)
{
	const sp_release *a = pa, *b = pb_;
	if (a->date != b->date) {
		if (!a->date)
			return 1;
		if (!b->date)
			return -1;
		return a->date > b->date ? -1 : 1;
	}
	return (a->order > b->order) - (a->order < b->order);
}

void sp_sort_releases(sp_release *r, int n)
{
	if (r && n > 1)
		qsort(r, (size_t)n, sizeof(r[0]), sp_release_cmp);
}

int sp_parse_album_batch(const uint8_t *body, size_t len,
                         const char *const *uris, int n, sp_release *out)
{
	pbv arr;
	if (n <= 0 || !pb_get_bytes(body, len, 2, &arr))
		return 0;
	/* Placed by entity_uri: the service answers in its own order. */
	int named = 0;
	size_t at = 0;
	pbv ed, uri, any, album;
	while (pb_next_bytes(&arr, 3, &at, &ed)) {
		if (!pb_get_bytes(ed.p, ed.len, 2, &uri) ||
		    !pb_get_bytes(ed.p, ed.len, 3, &any) ||
		    !pb_get_bytes(any.p, any.len, 2, &album))
			continue;
		for (int j = 0; j < n; j++) {
			if (out[j].name[0] || !same_uri(&uri, uris[j]))
				continue;
			fill_release(&album, &out[j]);
			named += out[j].name[0] != '\0';
			break;
		}
	}
	return named;
}

bool sp_parse_artist_insights(const char *json, size_t len, long *listeners)
{
	long v = -1;
	/* Top level only: every city in "cities" has "listeners" too. */
	bool ok = json && json_member_int(json, len, "monthlyListeners", &v) &&
	          v >= 0;
	*listeners = ok ? v : -1;
	return ok;
}

/* ------------------------------------------------------------- biography */

static bool is_alpha(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int digit(char c, int base)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (base == 16 && c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (base == 16 && c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static size_t utf8_put(unsigned long cp, char *b)
{
	if (cp < 0x80) {
		b[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		b[0] = (char)(0xC0 | (cp >> 6));
		b[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		b[0] = (char)(0xE0 | (cp >> 12));
		b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		b[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	b[0] = (char)(0xF0 | (cp >> 18));
	b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	b[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

/* The entity at `s` (on its '&') into `buf`: its bytes, with `*used` the
 * input it took; 0 when it is not one this knows. &nbsp; is a space. */
static size_t entity(const char *s, size_t len, char *buf, size_t *used)
{
	static const struct {
		const char *name;
		char c;
	} NAMED[] = {
		{ "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' }, { "quot;", '"' },
		{ "apos;", '\'' }, { "nbsp;", ' ' },
	};
	for (size_t k = 0; k < sizeof(NAMED) / sizeof(NAMED[0]); k++) {
		size_t nl = strlen(NAMED[k].name);
		if (len > nl && memcmp(s + 1, NAMED[k].name, nl) == 0) {
			buf[0] = NAMED[k].c;
			*used = 1 + nl;
			return 1;
		}
	}
	if (len < 4 || s[1] != '#')
		return 0;
	int base = 10;
	size_t i = 2;
	if (s[2] == 'x' || s[2] == 'X') {
		base = 16;
		i = 3;
	}
	size_t start = i;
	unsigned long cp = 0;
	while (i < len && i < start + 7) {
		int d = digit(s[i], base);
		if (d < 0)
			break;
		cp = cp * (unsigned long)base + (unsigned long)d;
		i++;
	}
	if (i == start || i >= len || s[i] != ';' || cp == 0 || cp > 0x10FFFF ||
	    (cp >= 0xD800 && cp <= 0xDFFF))
		return 0;
	*used = i + 1;
	if (cp == 0xA0) {
		buf[0] = ' ';
		return 1;
	}
	return utf8_put(cp, buf);
}

void sp_bio_clean(const char *in, size_t len, char *out, size_t cap)
{
	if (!out || cap == 0)
		return;
	size_t o = 0, i = 0;
	bool gap = false;               /* a space is owed before what follows */
	while (in && i < len && in[i]) {
		unsigned char c = (unsigned char)in[i];
		if (c == '<' && i + 1 < len &&
		    (is_alpha(in[i + 1]) || in[i + 1] == '/' || in[i + 1] == '!')) {
			const char *gt = memchr(in + i, '>', len - i);
			if (!gt)
				break;          /* a tag cut short: the rest is markup */
			i = (size_t)(gt - in) + 1;
			continue;
		}
		char ch[4];
		size_t n = 1, used = 1;
		if (c == '&') {
			n = entity(in + i, len - i, ch, &used);
			if (!n) {
				ch[0] = '&';
				n = used = 1;
			}
		} else if (c >= 0x80 && c < 0xC0) {
			i++;                /* a stray continuation byte */
			continue;
		} else if (c >= 0xC0) {
			n = used = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
			if (n > len - i)
				break;          /* a character cut short */
			memcpy(ch, in + i, n);
		} else {
			ch[0] = (char)c;
		}
		i += used;
		if (n == 1 && (ch[0] == ' ' || ch[0] == '\n' || ch[0] == '\r' ||
		               ch[0] == '\t')) {
			gap = o > 0;
			continue;
		}
		if (o + n + (gap ? 1 : 0) > cap - 1)
			break;
		if (gap)
			out[o++] = ' ';
		gap = false;
		memcpy(out + o, ch, n);
		o += n;
	}
	out[o] = '\0';
}

/* ------------------------------------------------------------ playlists */

static bool has_prefix(const uint8_t *p, size_t len, const char *prefix)
{
	size_t n = strlen(prefix);
	return len >= n && memcmp(p, prefix, n) == 0;
}

bool sp_is_track_uri(const char *uri)
{
	return uri && strncmp(uri, "spotify:track:", 14) == 0 && uri[14];
}

bool sp_list_is_live(const pbv *attrs)
{
	/* The DJ's attributes (probe_meta step 15.5): lexicon_set_type
	 * "your_dj", lexicon_context_url "hm://lexicon-session-provider/...".
	 * Any lexicon_ key marks a session that service builds as it plays. */
	size_t at = 0;
	pbv entry;
	while (pb_next_bytes(attrs, 12, &at, &entry)) {
		pbv key;
		if (pb_get_bytes(entry.p, entry.len, 1, &key) &&
		    has_prefix(key.p, key.len, "lexicon_"))
			return true;
	}
	return false;
}

void sp_parse_list_attributes(const pbv *attrs, char *name, size_t namelen,
                              char *art, size_t artlen)
{
	if (name && namelen)
		name[0] = '\0';
	if (art && artlen)
		art[0] = '\0';
	pbv v;
	if (name && namelen && pb_get_bytes(attrs->p, attrs->len, 1, &v))
		copy_utf8(name, namelen, &v);
	if (!art || !artlen)
		return;

	/* A user-set cover: the 20-byte id names the 300px JPEG on i.scdn.co
	 * (about 15-30 KB, checked in probe_meta step 15.4). */
	if (pb_get_bytes(attrs->p, attrs->len, 3, &v) && v.len == 20) {
		static const char HEX[] = "0123456789abcdef";
		char hex[41];
		for (int i = 0; i < 20; i++) {
			hex[i * 2] = HEX[v.p[i] >> 4];
			hex[i * 2 + 1] = HEX[v.p[i] & 15];
		}
		hex[40] = '\0';
		snprintf(art, artlen, "https://i.scdn.co/image/%s", hex);
		if (strlen(art) != 64)
			art[0] = '\0';         /* did not fit: better none than half */
		return;
	}

	/* Otherwise a sized URL, if one is on the CDN this client can reach. */
	static const char *const PREFER[] = { "small", "default", "large" };
	for (size_t k = 0; k < sizeof(PREFER) / sizeof(PREFER[0]); k++) {
		size_t at = 0;
		pbv size;
		while (pb_next_bytes(attrs, 13, &at, &size)) {
			pbv target, url;
			if (!pb_get_bytes(size.p, size.len, 1, &target) ||
			    target.len != strlen(PREFER[k]) ||
			    memcmp(target.p, PREFER[k], target.len) != 0 ||
			    !pb_get_bytes(size.p, size.len, 2, &url))
				continue;
			if (has_prefix(url.p, url.len, "https://i.scdn.co/") &&
			    url.len < artlen) {
				memcpy(art, url.p, url.len);
				art[url.len] = '\0';
				return;
			}
		}
	}
}

int sp_parse_rootlist(const uint8_t *body, size_t len, sp_playlist_ref *out,
                      int max, int *total)
{
	uint64_t length = 0;
	pb_get_varint(body, len, 2, &length);
	if (total)
		*total = (int)length;

	pbv contents;
	if (!pb_get_bytes(body, len, 5, &contents))
		return 0;

	/* items and meta_items are two parallel repeated fields, interleaved or
	 * not: each is walked with its own cursor. */
	int n = 0;
	size_t ai = 0, am = 0;
	pbv item;
	while (n < max && pb_next_bytes(&contents, 3, &ai, &item)) {
		pbv meta = { NULL, 0 };
		bool has_meta = pb_next_bytes(&contents, 4, &am, &meta);
		pbv uri, attrs = { NULL, 0 };
		if (!pb_get_bytes(item.p, item.len, 1, &uri) ||
		    !has_prefix(uri.p, uri.len, "spotify:playlist:") ||
		    uri.len >= SP_URI_MAX)
			continue;
		bool has_attrs = has_meta && pb_get_bytes(meta.p, meta.len, 2, &attrs);
		if (has_attrs && sp_list_is_live(&attrs))
			continue;

		sp_playlist_ref *r = &out[n++];
		memset(r, 0, sizeof(*r));
		memcpy(r->uri, uri.p, uri.len);
		r->uri[uri.len] = '\0';
		if (has_attrs)
			sp_parse_list_attributes(&attrs, r->name, sizeof(r->name),
			                         r->art, sizeof(r->art));
		uint64_t tracks = 0;
		if (has_meta && pb_get_varint(meta.p, meta.len, 3, &tracks))
			r->length = (int)tracks;
	}
	return n;
}

void sp_parse_playlist(const uint8_t *body, size_t len, const char *uri,
                       int max_tracks, sp_playlist_info *out)
{
	memset(out, 0, sizeof(*out));
	if (uri)
		snprintf(out->uri, sizeof(out->uri), "%s", uri);
	if (max_tracks > SP_PLAYLIST_PAGE)
		max_tracks = SP_PLAYLIST_PAGE;

	uint64_t length = 0;
	if (pb_get_varint(body, len, 2, &length))
		out->total_tracks = (int)length;
	pbv attrs;
	if (pb_get_bytes(body, len, 3, &attrs)) {
		sp_parse_list_attributes(&attrs, out->name, sizeof(out->name),
		                         out->art, sizeof(out->art));
		out->live = sp_list_is_live(&attrs);
	}

	pbv contents;
	if (!pb_get_bytes(body, len, 5, &contents))
		return;
	/* `items` counts what was read, so the next page starts right after:
	 * past a full page nothing more is read, even if the service sent
	 * more than was asked. */
	size_t at = 0;
	pbv item;
	while (out->track_count < max_tracks &&
	       pb_next_bytes(&contents, 3, &at, &item)) {
		out->items++;
		pbv u;
		if (!pb_get_bytes(item.p, item.len, 1, &u) ||
		    !has_prefix(u.p, u.len, "spotify:track:") || u.len >= SP_URI_MAX ||
		    u.len <= 14) {
			out->skipped++;
			continue;
		}
		char *dst = out->track_uris[out->track_count++];
		memcpy(dst, u.p, u.len);
		dst[u.len] = '\0';
	}
}

/* The next `key` (quoted, as "uri") in [p, end), or NULL. */
static const char *json_key(const char *p, const char *end, const char *key)
{
	size_t n = strlen(key);
	for (const char *s = p; s + n <= end; s++)
		if (memcmp(s, key, n) == 0)
			return s;
	return NULL;
}

/* The string value after a key that ends at `*p`; `*p` moves past it. False
 * when the value is not a string or is cut short by `end`. */
static bool json_value(const char **p, const char *end, const char **v,
                       size_t *vlen)
{
	const char *s = *p;
	while (s < end && (*s == ' ' || *s == ':' || *s == '\t' || *s == '\r' ||
	                   *s == '\n'))
		s++;
	if (s >= end || *s != '"')
		return false;
	const char *start = ++s;
	while (s < end && *s != '"')
		s++;
	if (s >= end)
		return false;              /* cut short: never keep half a URI */
	*v = start;
	*vlen = (size_t)(s - start);
	*p = s + 1;
	return true;
}

int sp_parse_recent_playlists(const char *json, size_t len,
                              char uris[][SP_URI_MAX],
                              char (*last)[SP_URI_MAX], int max)
{
	static const char KEY[] = "\"uri\"";
	static const char LAST[] = "\"lastPlayedTrackUri\"";
	const char *end = json + len;
	int n = 0;
	for (const char *p = json; n < max && p < end; ) {
		const char *k = json_key(p, end, KEY);
		if (!k)
			break;
		p = k + sizeof(KEY) - 1;
		const char *v;
		size_t vlen;
		if (!json_value(&p, end, &v, &vlen)) {
			if (p >= end || !memchr(p, '"', (size_t)(end - p)))
				break;
			continue;
		}
		if (vlen >= SP_URI_MAX ||
		    !has_prefix((const uint8_t *)v, vlen, "spotify:playlist:") ||
		    vlen <= 17)
			continue;
		bool seen = false;
		for (int i = 0; i < n && !seen; i++)
			seen = strlen(uris[i]) == vlen && memcmp(uris[i], v, vlen) == 0;
		if (seen)
			continue;
		memcpy(uris[n], v, vlen);
		uris[n][vlen] = '\0';
		/* Its last track, inside the same (flat) object. */
		if (last) {
			last[n][0] = '\0';
			const char *close = memchr(p, '}', (size_t)(end - p));
			const char *lk = close ? json_key(p, close, LAST) : NULL;
			const char *lp = lk ? lk + sizeof(LAST) - 1 : NULL;
			const char *t;
			size_t tlen;
			if (lp && json_value(&lp, close, &t, &tlen) &&
			    tlen < SP_URI_MAX &&
			    has_prefix((const uint8_t *)t, tlen, "spotify:track:")) {
				memcpy(last[n], t, tlen);
				last[n][tlen] = '\0';
			}
		}
		n++;
	}
	return n;
}

int sp_parse_liked(const char *json, size_t len, char (*uris)[SP_URI_MAX],
                   int max, int *total)
{
	static const char KEY[] = "\"uri\"";
	const char *end = json + len;
	int n = 0, all = 0;
	for (const char *p = json; p < end; ) {
		const char *k = json_key(p, end, KEY);
		if (!k)
			break;
		p = k + sizeof(KEY) - 1;
		const char *v;
		size_t vlen;
		if (!json_value(&p, end, &v, &vlen))
			break;
		/* The context's own "uri" (spotify:user:...:collection) is not a
		 * track: only tracks count. */
		if (vlen >= SP_URI_MAX || vlen <= 14 ||
		    !has_prefix((const uint8_t *)v, vlen, "spotify:track:"))
			continue;
		all++;
		if (n < max) {
			memcpy(uris[n], v, vlen);
			uris[n][vlen] = '\0';
			n++;
		}
	}
	if (total)
		*total = all;
	return n;
}
