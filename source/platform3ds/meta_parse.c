#include "meta_parse.h"

#include <stdio.h>
#include <string.h>

static void copy_str(char *dst, size_t cap, const pbv *v)
{
	size_t n = v->len < cap - 1 ? v->len : cap - 1;
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
	return true;
}

/* ------------------------------------------------------------ playlists */

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

int sp_parse_recent_playlists(const char *json, size_t len,
                              char uris[][SP_URI_MAX], int max)
{
	static const char KEY[] = "\"uri\"";
	const char *end = json + len;
	int n = 0;
	for (const char *p = json; n < max && p < end; ) {
		const char *k = NULL;
		for (const char *s = p; s + sizeof(KEY) - 1 <= end; s++)
			if (memcmp(s, KEY, sizeof(KEY) - 1) == 0) {
				k = s;
				break;
			}
		if (!k)
			break;
		p = k + sizeof(KEY) - 1;
		while (p < end && (*p == ' ' || *p == ':' || *p == '\t' ||
		                   *p == '\r' || *p == '\n'))
			p++;
		if (p >= end || *p != '"')
			continue;
		const char *v = ++p;
		while (p < end && *p != '"')
			p++;
		if (p >= end)
			break;                 /* cut short: never keep half a URI */
		size_t vlen = (size_t)(p - v);
		p++;
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
		n++;
	}
	return n;
}
