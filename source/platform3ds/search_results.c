#include "search_results.h"
#include "json3ds.h"

#include <stdio.h>
#include <string.h>

#define SCDN_IMAGE "https://i.scdn.co/image/"
#define BUNDLE_MARK "/cdn/build/mobile-web-player/mobile-web-player."
/* An uploaded cover's size codes: the WebP one pathfinder names, and the
 * JPEG one of the same image. */
#define UPLOAD_WEBP "ab67706c0000d72c"
#define UPLOAD_JPEG "ab67706c0000da84"
/* A mosaic tile id: 40 hex, like every image id on i.scdn.co. */
#define IMAGE_ID_LEN 40

static const char *const SECTION[RESULT_KINDS] = {
	"tracksV2", "artists", "albumsV2", "playlists",
};
static const char *const TYPENAME[RESULT_KINDS] = {
	"Track", "Artist", "Album", "Playlist",
};
static const char *const OPERATION[RESULT_KINDS] = {
	"findTracks", "findArtists", "findAlbums", "findPlaylists",
};

const char *pf_operation(result_kind kind)
{
	return (unsigned)kind < RESULT_KINDS ? OPERATION[kind] : NULL;
}

/* ------------------------------------------------------------- requests */

/* `in` as the inside of a JSON string. False when it does not fit. */
static bool json_escape(const char *in, char *out, size_t outlen)
{
	static const char hex[] = "0123456789abcdef";
	size_t n = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
		char esc[6];
		size_t k = 1;
		if (*p == '"' || *p == '\\') {
			esc[0] = '\\';
			esc[1] = (char)*p;
			k = 2;
		} else if (*p < 0x20) {
			memcpy(esc, "\\u00", 4);
			esc[4] = hex[*p >> 4];
			esc[5] = hex[*p & 15];
			k = 6;
		} else {
			esc[0] = (char)*p;
		}
		if (n + k >= outlen)
			return false;
		memcpy(out + n, esc, k);
		n += k;
	}
	out[n] = '\0';
	return true;
}

static bool valid_hash(const char *hash)
{
	if (!hash || strlen(hash) != 64)
		return false;
	for (const char *p = hash; *p; p++)
		if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
			return false;
	return true;
}

bool pf_body_top(const char *query, int top, const char *hash, char *out,
                 size_t size)
{
	char eq[384];
	if (!query || !query[0] || !out || !valid_hash(hash) ||
	    !json_escape(query, eq, sizeof(eq)))
		return false;
	int n = snprintf(out, size,
	                 "{\"variables\":{\"query\":\"%s\",\"numberOfTopResults\":%d,"
	                 "\"includePreReleases\":false,"
	                 "\"includeAlbumPreReleases\":false},"
	                 "\"operationName\":\"findTopResults\",\"extensions\":"
	                 "{\"persistedQuery\":{\"version\":1,\"sha256Hash\":\"%s\"}}}",
	                 eq, top, hash);
	return n > 0 && (size_t)n < size;
}

bool pf_body_page(result_kind kind, const char *query, int limit,
                  const char *hash, char *out, size_t size)
{
	char eq[384];
	const char *op = pf_operation(kind);
	if (!op || !query || !query[0] || !out || !valid_hash(hash) ||
	    !json_escape(query, eq, sizeof(eq)))
		return false;
	int n = snprintf(out, size,
	                 "{\"variables\":{\"query\":\"%s\",\"limit\":%d,\"offset\":0},"
	                 "\"operationName\":\"%s\",\"extensions\":"
	                 "{\"persistedQuery\":{\"version\":1,\"sha256Hash\":\"%s\"}}}",
	                 eq, limit, op, hash);
	return n > 0 && (size_t)n < size;
}

/* --------------------------------------------------------------- covers */

static bool is_id_char(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
	       (c >= 'A' && c <= 'Z');
}

/* "https://i.scdn.co/image/<id>" for the `n` characters at `id`, the WebP
 * size code of an uploaded cover swapped for the JPEG one. */
static void scdn_url(const char *id, size_t n, char *out, size_t size)
{
	out[0] = '\0';
	if (n == 0)
		return;
	for (size_t i = 0; i < n; i++)
		if (!is_id_char(id[i]))
			return;
	const size_t code = sizeof(UPLOAD_WEBP) - 1;
	bool webp = n > code && strncmp(id, UPLOAD_WEBP, code) == 0;
	int w = webp
	    ? snprintf(out, size, SCDN_IMAGE UPLOAD_JPEG "%.*s",
	               (int)(n - code), id + code)
	    : snprintf(out, size, SCDN_IMAGE "%.*s", (int)n, id);
	if (w < 0 || (size_t)w >= size)
		out[0] = '\0';
}

void pf_cover_url(const char *url, char *out, size_t size)
{
	static const char *const SAME[] = {
		SCDN_IMAGE,
		"https://image-cdn-fa.spotifycdn.com/image/",
		"https://image-cdn-ak.spotifycdn.com/image/",
	};
	static const char MOSAIC[] = "https://mosaic.scdn.co/";
	if (!out || size == 0)
		return;
	out[0] = '\0';
	if (!url)
		return;
	for (size_t i = 0; i < sizeof(SAME) / sizeof(SAME[0]); i++) {
		size_t k = strlen(SAME[i]);
		if (strncmp(url, SAME[i], k) == 0) {
			scdn_url(url + k, strlen(url + k), out, size);
			return;
		}
	}
	if (strncmp(url, MOSAIC, sizeof(MOSAIC) - 1) == 0) {
		const char *tiles = strchr(url + sizeof(MOSAIC) - 1, '/');
		if (tiles && strlen(tiles + 1) >= IMAGE_ID_LEN)
			scdn_url(tiles + 1, IMAGE_ID_LEN, out, size);
	}
}

/* The two covers out of a sources array: the large one the widest that is
 * no wider than 640, the small one the narrowest at least 64 wide. When no
 * source says its width (a playlist's), the first goes to both. */
static void read_sources(const char *arr, size_t len, sp_track *t)
{
	long large_w = -1, small_w = -1;
	char first[sizeof(t->art_large)] = "";
	size_t cur = 0;
	const char *src;
	size_t sl;
	while (json_item_next(arr, len, &cur, &src, &sl)) {
		char raw[256], url[sizeof(t->art_large)];
		if (!json_member_str(src, sl, "url", raw, sizeof(raw)))
			continue;
		pf_cover_url(raw, url, sizeof(url));
		if (!url[0])
			continue;
		long w;
		if (!json_member_int(src, sl, "width", &w)) {
			if (!first[0])
				snprintf(first, sizeof(first), "%s", url);
			continue;
		}
		if (w <= 640 && w > large_w) {
			large_w = w;
			snprintf(t->art_large, sizeof(t->art_large), "%s", url);
		}
		if (w >= 64 && (small_w < 0 || w < small_w)) {
			small_w = w;
			snprintf(t->art_small, sizeof(t->art_small), "%s", url);
		}
	}
	if (!t->art_large[0] && t->art_small[0])
		snprintf(t->art_large, sizeof(t->art_large), "%s", t->art_small);
	if (!t->art_small[0] && t->art_large[0])
		snprintf(t->art_small, sizeof(t->art_small), "%s", t->art_large);
	if (!t->art_large[0] && first[0]) {
		snprintf(t->art_large, sizeof(t->art_large), "%s", first);
		snprintf(t->art_small, sizeof(t->art_small), "%s", first);
	}
}

/* ---------------------------------------------------------------- items */

/* The first artist's name of an `artists` object ({items: [...]}). */
static void first_artist(const char *data, size_t len, char *out,
                         size_t size)
{
	const char *arr;
	size_t al;
	out[0] = '\0';
	if (!json_path(data, len, "artists.items", &arr, &al))
		return;
	size_t cur = 0;
	const char *a;
	size_t a_len;
	if (json_item_next(arr, al, &cur, &a, &a_len) &&
	    !json_member_str(a, a_len, "profile.name", out, size))
		out[0] = '\0';
}

/* One item's data object into `t`. False when it is not of `kind`. */
static bool read_item(const char *data, size_t len, result_kind kind,
                      sp_track *t)
{
	char type[24];
	if (!json_member_str(data, len, "__typename", type, sizeof(type)) ||
	    strcmp(type, TYPENAME[kind]) != 0)
		return false;
	memset(t, 0, sizeof(*t));
	if (!json_member_str(data, len, "uri", t->uri, sizeof(t->uri)) ||
	    !t->uri[0])
		return false;
	if (kind == RESULT_TRACKS)
		return true;

	const char *src;
	size_t sl;
	switch (kind) {
	case RESULT_ARTISTS:
		json_member_str(data, len, "profile.name", t->name, sizeof(t->name));
		if (json_path(data, len, "visuals.avatarImage.sources", &src, &sl))
			read_sources(src, sl, t);
		break;
	case RESULT_ALBUMS:
		json_member_str(data, len, "name", t->name, sizeof(t->name));
		first_artist(data, len, t->artist, sizeof(t->artist));
		if (json_path(data, len, "coverArt.sources", &src, &sl))
			read_sources(src, sl, t);
		break;
	case RESULT_PLAYLISTS: {
		json_member_str(data, len, "name", t->name, sizeof(t->name));
		json_member_str(data, len, "ownerV2.data.name", t->artist,
		                sizeof(t->artist));
		const char *imgs;
		size_t il;
		size_t cur = 0;
		const char *img;
		size_t img_len;
		if (json_path(data, len, "images.items", &imgs, &il) &&
		    json_item_next(imgs, il, &cur, &img, &img_len) &&
		    json_member(img, img_len, "sources", &src, &sl))
			read_sources(src, sl, t);
		break;
	}
	default:
		break;
	}
	return true;
}

int pf_parse_page(const char *json, size_t len, result_kind kind,
                  sp_track *out, int max)
{
	if (!json || (unsigned)kind >= RESULT_KINDS || !out || max < 0)
		return -1;
	char path[64];
	snprintf(path, sizeof(path), "data.searchV2.%s.items", SECTION[kind]);
	const char *arr;
	size_t al;
	if (!json_path(json, len, path, &arr, &al) || al == 0 || *arr != '[')
		return -1;

	int count = 0;
	size_t cur = 0;
	const char *it;
	size_t il;
	while (count < max && json_item_next(arr, al, &cur, &it, &il)) {
		const char *data;
		size_t dl;
		bool found = kind == RESULT_TRACKS
		    ? json_path(it, il, "item.data", &data, &dl)
		    : json_member(it, il, "data", &data, &dl);
		if (found && read_item(data, dl, kind, &out[count]))
			count++;
	}
	return count;
}

bool pf_parse_best(const char *json, size_t len, result_kind *kind,
                   char *uri, size_t size)
{
	const char *arr;
	size_t al;
	if (!json || !json_path(json, len, "data.searchV2.topResultsV2.itemsV2",
	                        &arr, &al))
		return false;
	size_t cur = 0;
	const char *it;
	size_t il;
	while (json_item_next(arr, al, &cur, &it, &il)) {
		const char *data;
		size_t dl;
		char type[24];
		if (!json_path(it, il, "item.data", &data, &dl) ||
		    !json_member_str(data, dl, "__typename", type, sizeof(type)))
			continue;
		for (int k = 0; k < RESULT_KINDS; k++) {
			if (strcmp(type, TYPENAME[k]) != 0)
				continue;
			char u[SP_URI_MAX];
			if (!json_member_str(data, dl, "uri", u, sizeof(u)) || !u[0] ||
			    strlen(u) >= size)
				break;          /* unusable: the next one may do */
			if (kind)
				*kind = (result_kind)k;
			snprintf(uri, size, "%s", u);
			return true;
		}
	}
	return false;
}

/* ------------------------------------------------------------------ hash */

static bool is_hex(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool pf_find_bundle(const char *html, char *url, size_t size)
{
	if (!html || !url || size == 0)
		return false;
	url[0] = '\0';
	for (const char *m = strstr(html, BUNDLE_MARK); m;
	     m = strstr(m + 1, BUNDLE_MARK)) {
		const char *e = m + sizeof(BUNDLE_MARK) - 1;
		const char *hex = e;
		while (is_hex(*e))
			e++;
		if (e == hex || strncmp(e, ".js\"", 4) != 0)
			continue;
		const char *s = m;
		while (s > html && s[-1] != '"')
			s--;
		size_t n = (size_t)(e + 3 - s);
		if (n >= size || strncmp(s, "https://", 8) != 0)
			continue;
		memcpy(url, s, n);
		url[n] = '\0';
		return true;
	}
	return false;
}

bool gzip_payload(const uint8_t *b, size_t n, size_t *off, size_t *len,
                  size_t *isize)
{
	/* RFC 1952: magic, deflate, flags; then the optional fields; the
	 * trailer is CRC32 and the inflated size, both little-endian. */
	if (!b || n < 18 || b[0] != 0x1f || b[1] != 0x8b || b[2] != 8)
		return false;
	uint8_t flg = b[3];
	if (flg & 0xE0)
		return false;                    /* reserved bits */
	size_t p = 10;
	if (flg & 4) {                       /* FEXTRA */
		if (p + 2 > n)
			return false;
		p += 2 + (size_t)(b[p] | b[p + 1] << 8);
	}
	for (int field = 8; field <= 16; field <<= 1) {   /* FNAME, FCOMMENT */
		if (!(flg & field))
			continue;
		while (p < n && b[p])
			p++;
		p++;
	}
	if (flg & 2)                         /* FHCRC */
		p += 2;
	if (p + 8 >= n)
		return false;
	if (off)
		*off = p;
	if (len)
		*len = n - p - 8;
	if (isize)
		*isize = (size_t)b[n - 4] | (size_t)b[n - 3] << 8 |
		         (size_t)b[n - 2] << 16 | (size_t)b[n - 1] << 24;
	return true;
}

bool pf_find_hash(const char *js, size_t len, const char *op, char hash[65])
{
	char mark[64];
	if (!js || !op || !hash)
		return false;
	int ml = snprintf(mark, sizeof(mark), "\"%s\",\"query\",\"", op);
	if (ml <= 0 || (size_t)ml >= sizeof(mark) || len < (size_t)ml + 65)
		return false;
	for (size_t i = 0; i + (size_t)ml + 65 <= len; i++) {
		if (js[i] != '"' || memcmp(js + i, mark, (size_t)ml) != 0)
			continue;
		const char *h = js + i + ml;
		int k = 0;
		while (k < 64 && is_hex(h[k]))
			k++;
		if (k != 64 || h[64] != '"')
			continue;
		memcpy(hash, h, 64);
		hash[64] = '\0';
		return true;
	}
	return false;
}

/* --------------------------------------------------------------- derived */

int search_find_uri(const sp_track *items, int n, const char *uri)
{
	if (!items || !uri || !uri[0])
		return -1;
	for (int i = 0; i < n; i++)
		if (strcmp(items[i].uri, uri) == 0)
			return i;
	return -1;
}

int search_derive_artists(const sp_track *tracks, int n, sp_track *out,
                          int max)
{
	int count = 0;
	for (int i = 0; tracks && out && i < n && count < max; i++) {
		const sp_track *t = &tracks[i];
		if (!t->artist_uri[0] || search_find_uri(out, count, t->artist_uri) >= 0)
			continue;
		sp_track *a = &out[count++];
		memset(a, 0, sizeof(*a));
		snprintf(a->uri, sizeof(a->uri), "%s", t->artist_uri);
		snprintf(a->name, sizeof(a->name), "%s", t->artist);
	}
	return count;
}

int search_derive_albums(const sp_track *tracks, int n, sp_track *out,
                         int max)
{
	int count = 0;
	for (int i = 0; tracks && out && i < n && count < max; i++) {
		const sp_track *t = &tracks[i];
		if (!t->album_uri[0] || search_find_uri(out, count, t->album_uri) >= 0)
			continue;
		sp_track *a = &out[count++];
		memset(a, 0, sizeof(*a));
		snprintf(a->uri, sizeof(a->uri), "%s", t->album_uri);
		snprintf(a->name, sizeof(a->name), "%s", t->album);
		snprintf(a->artist, sizeof(a->artist), "%s", t->artist);
		snprintf(a->art_small, sizeof(a->art_small), "%s", t->art_small);
		snprintf(a->art_large, sizeof(a->art_large), "%s", t->art_large);
		a->year = t->year;
	}
	return count;
}
