#include "spclient.h"
#include "http3ds.h"
#include "json3ds.h"
#include "search_path.h"
#include "proto3ds.h"
#include "meta_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPCLIENT_FALLBACK "spclient.wg.spotify.com"

/* extended_metadata.proto: BatchedEntityRequest { header 1, entity_request 2 },
 * EntityRequest { entity_uri 1, query 2 }, ExtensionQuery { extension_kind 1 }.
 * Putting the entity request in field 1 yields HTTP 200 and an empty body,
 * which is a confusing way to be told the shape is wrong. The kinds and the
 * message parsers live in meta_parse.c. */


const char *sp_format_name(int format)
{
	switch (format) {
	case SP_FMT_OGG_96:  return "Vorbis 96";
	case SP_FMT_OGG_160: return "Vorbis 160";
	case SP_FMT_OGG_320: return "Vorbis 320";
	case 3: case 4: case 5: case 6: case 7: return "MP3";
	case 8: case 9: return "AAC";
	case 16: case 22: return "FLAC";
	default: return "other";
	}
}

const sp_file *sp_pick_file(const sp_track *t)
{
	/* 160 first, then 96, then 320: the order of what this console should
	 * actually stream and decode, not of audio quality. */
	static const int WANT[] = { SP_FMT_OGG_160, SP_FMT_OGG_96, SP_FMT_OGG_320 };
	for (size_t w = 0; w < sizeof(WANT) / sizeof(WANT[0]); w++)
		for (int i = 0; i < t->file_count; i++)
			if (t->files[i].format == WANT[w])
				return &t->files[i];
	return NULL;
}

int sp_resolve(char *host, size_t hostlen, char *err, size_t errlen)
{
	http_reply r;
	if (http_get("apresolve.spotify.com", "/?type=spclient", NULL, &r) == 0 &&
	    r.status == 200) {
		const char *key = strstr(r.body, "\"spclient\"");
		const char *open = key ? strchr(key, '[') : NULL;
		const char *q1 = open ? strchr(open, '"') : NULL;
		const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
		if (q2) {
			char entry[160];
			size_t n = (size_t)(q2 - q1 - 1);
			if (n < sizeof(entry)) {
				memcpy(entry, q1 + 1, n);
				entry[n] = '\0';
				/* The entry carries a port; 443 is implied and the port is
				 * dropped, since everything here goes over TLS. */
				char *colon = strrchr(entry, ':');
				if (colon)
					*colon = '\0';
				snprintf(host, hostlen, "%s", entry);
				http_reply_free(&r);
				return 0;
			}
		}
	}
	http_reply_free(&r);
	snprintf(host, hostlen, "%s", SPCLIENT_FALLBACK);
	snprintf(err, errlen, "apresolve gave no spclient; using %s", SPCLIENT_FALLBACK);
	return 0;
}

/* One entity of one kind through extended-metadata, over `sess` when given
 * and a fresh connection to `host` otherwise. On 0, `r` holds the body that
 * `entity` points into and the caller frees it. `what` names the request in
 * errors ("metadata", "album", "artist") and `noun` the missing entity. */
static int entity_request(const char *host, http_session *sess,
                          const char *token, const char *uri, int kind,
                          const char *what, const char *noun,
                          http_reply *r, pbv *entity, char *err, size_t errlen)
{
	uint8_t q_store[16], er_store[256], req_store[320];
	pb q, er, req;
	pb_init(&q, q_store, sizeof(q_store));
	pb_varint(&q, 1, (uint64_t)kind);
	pb_init(&er, er_store, sizeof(er_store));
	pb_str(&er, 1, uri);
	pb_sub(&er, 2, &q);
	pb_init(&req, req_store, sizeof(req_store));
	pb_sub(&req, 2, &er);
	if (!pb_ok(&req)) {
		snprintf(err, errlen, "%s request did not fit", what);
		return -1;
	}

	/* The Bearer matters here as much as on the GETs: without it the service
	 * answers 200 with an empty body rather than 401, which reads like a
	 * malformed request. */
	int rc = sess
	    ? http_session_request(sess, "/extended-metadata/v0/extended-metadata",
	                           "POST", token, "application/x-protobuf",
	                           "application/x-protobuf", req.p, req.len, 0, r)
	    : http_request(host, "/extended-metadata/v0/extended-metadata", "POST",
	                   token, "application/x-protobuf", "application/x-protobuf",
	                   req.p, req.len, 0, r);
	if (rc != 0) {
		snprintf(err, errlen, "%s: %s", what, r->err);
		return -1;
	}
	if (r->status != 200) {
		snprintf(err, errlen, "%s returned HTTP %d", what, r->status);
		http_reply_free(r);
		return -1;
	}
	if (!sp_parse_entity((const uint8_t *)r->body, r->len, entity)) {
		snprintf(err, errlen, "no %s in the metadata response (%u bytes)",
		         noun, (unsigned)r->len);
		http_reply_free(r);
		return -1;
	}
	return 0;
}

int sp_track_metadata(const char *host, const char *token, const char *track_uri,
                      sp_track *out, char *err, size_t errlen)
{
	memset(out, 0, sizeof(*out));
	http_reply r;
	pbv track;
	if (entity_request(host, NULL, token, track_uri, EXT_KIND_TRACK_V4,
	                   "metadata", "track", &r, &track, err, errlen) != 0)
		return -1;
	sp_parse_track(&track, out);
	http_reply_free(&r);
	return 0;
}

int sp_album_metadata(http_session *sess, const char *token,
                      const char *album_uri, sp_album *out,
                      char *err, size_t errlen)
{
	memset(out, 0, sizeof(*out));
	http_reply r;
	pbv album;
	if (entity_request(NULL, sess, token, album_uri, EXT_KIND_ALBUM_V4,
	                   "album", "album", &r, &album, err, errlen) != 0)
		return -1;
	bool ok = sp_parse_album(&album, out);
	http_reply_free(&r);
	if (!ok) {
		snprintf(err, errlen, "album metadata carried no id");
		return -1;
	}
	return 0;
}

int sp_artist_metadata(http_session *sess, const char *token,
                       const char *artist_uri, const char *country,
                       sp_artist *out, char *err, size_t errlen)
{
	memset(out, 0, sizeof(*out));
	http_reply r;
	pbv artist;
	if (entity_request(NULL, sess, token, artist_uri, EXT_KIND_ARTIST_V4,
	                   "artist", "artist", &r, &artist, err, errlen) != 0)
		return -1;
	bool ok = sp_parse_artist(&artist, country, out);
	http_reply_free(&r);
	if (!ok) {
		snprintf(err, errlen, "artist metadata carried no id");
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------- playlists */

/* One protobuf GET over the session, checked for a 200. */
static int session_get_proto(http_session *sess, const char *token,
                             const char *path, const char *what,
                             http_reply *r, char *err, size_t errlen)
{
	if (http_session_request(sess, path, "GET", token, NULL,
	                         "application/x-protobuf", NULL, 0, 0, r) != 0) {
		snprintf(err, errlen, "%s: %s", what, r->err);
		return -1;
	}
	if (r->status != 200) {
		snprintf(err, errlen, "%s returned HTTP %d", what, r->status);
		http_reply_free(r);
		return -1;
	}
	return 0;
}

int sp_rootlist_s(http_session *sess, const char *token, const char *username,
                  sp_playlist_ref *out, int max, int *total,
                  char *err, size_t errlen)
{
	char user[192], path[320];
	url_encode(username, user, sizeof(user));
	snprintf(path, sizeof(path),
	         "/playlist/v2/user/%s/rootlist?decorate=revision,attributes,length",
	         user);
	http_reply r;
	if (session_get_proto(sess, token, path, "rootlist", &r, err, errlen) != 0)
		return -1;
	int n = sp_parse_rootlist((const uint8_t *)r.body, r.len, out, max, total);
	http_reply_free(&r);
	return n;
}

int sp_recent_playlists_s(http_session *sess, const char *token,
                          const char *username, char uris[][SP_URI_MAX],
                          int max, char *err, size_t errlen)
{
	/* The desktop client's own request. limit=50 counts every kind of
	 * context, so the playlists among them are fewer. */
	char user[192], path[320];
	url_encode(username, user, sizeof(user));
	snprintf(path, sizeof(path),
	         "/recently-played/v3/user/%s/recently-played"
	         "?format=json&offset=0&limit=50&filter=default", user);
	http_reply r;
	if (http_session_request(sess, path, "GET", token, NULL,
	                         "application/json", NULL, 0, 0, &r) != 0) {
		snprintf(err, errlen, "history: %s", r.err);
		return -1;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "history returned HTTP %d", r.status);
		http_reply_free(&r);
		return -1;
	}
	int n = sp_parse_recent_playlists(r.body, r.len, uris, max);
	http_reply_free(&r);
	return n;
}

int sp_playlist_s(http_session *sess, const char *token, const char *uri,
                  int from, int max_tracks, sp_playlist_info *out,
                  char *err, size_t errlen)
{
	memset(out, 0, sizeof(*out));
	if (strncmp(uri, "spotify:playlist:", 17) != 0 || !uri[17]) {
		snprintf(err, errlen, "not a playlist: %s", uri);
		return -1;
	}
	if (max_tracks > SP_PLAYLIST_PAGE)
		max_tracks = SP_PLAYLIST_PAGE;
	if (max_tracks < 0)
		max_tracks = 0;
	if (from < 0)
		from = 0;

	/* from/length is honoured (a 1275-track list: 110 KB whole, 2.3 KB for
	 * 24); without the decoration the header came back with no length. */
	char path[192];
	snprintf(path, sizeof(path),
	         "/playlist/v2/playlist/%s?decorate=attributes,length"
	         "&from=%d&length=%d", uri + 17, from, max_tracks);
	http_reply r;
	if (session_get_proto(sess, token, path, "playlist", &r, err, errlen) != 0)
		return -1;
	sp_parse_playlist((const uint8_t *)r.body, r.len, uri, max_tracks, out);
	http_reply_free(&r);
	return 0;
}

int sp_liked(const char *host, const char *token, const char *username,
             char uris[][64], int max, char *err, size_t errlen)
{
	char path[192];
	snprintf(path, sizeof(path),
	         "/context-resolve/v1/spotify:user:%s:collection", username);

	http_reply r;
	if (http_get(host, path, token, &r) != 0) {
		snprintf(err, errlen, "liked: %s", r.err);
		return -1;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "liked returned HTTP %d", r.status);
		http_reply_free(&r);
		return -1;
	}

	/* {"pages":[{"tracks":[{"uri":"spotify:track:...", "metadata":{...}}, ...]}]}
	 * The array iterator finds "tracks" wherever it sits, which saves walking
	 * into "pages" first. */
	int n = 0;
	size_t cursor = 0;
	const char *obj;
	size_t objlen;
	while (n < max && json_array_next(r.body, "tracks", &cursor, &obj, &objlen)) {
		char buf[512];
		if (objlen >= sizeof(buf))
			continue;
		memcpy(buf, obj, objlen);
		buf[objlen] = '\0';
		char uri[64];
		if (json_str(buf, "uri", uri, sizeof(uri)))
			snprintf(uris[n++], 64, "%s", uri);
	}
	http_reply_free(&r);
	return n;
}

/* ------------------------------------------------------- search and radio */

/* json_str and json_int want a NUL-terminated string, and the array iterator
 * hands back a slice, so each object is copied out. They are small: the
 * biggest seen is a radio entry at a few hundred bytes. */
static bool slice_to_buf(const char *p, size_t len, char *buf, size_t cap)
{
	if (len >= cap)
		return false;
	memcpy(buf, p, len);
	buf[len] = '\0';
	return true;
}

static int metadata_batch_request(const char *host, http_session *sess,
                                  const char *token,
                                  const char *const *uris, int n,
                                  sp_track *out, char *err, size_t errlen)
{
	if (n <= 0)
		return 0;

	uint8_t q_store[16];
	pb q;
	pb_init(&q, q_store, sizeof(q_store));
	pb_varint(&q, 1, EXT_KIND_TRACK_V4);

	size_t cap = (size_t)n * 128 + 64;
	uint8_t *req_store = malloc(cap);
	if (!req_store) {
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	pb req;
	pb_init(&req, req_store, cap);
	for (int i = 0; i < n; i++) {
		uint8_t er_store[192];
		pb er;
		pb_init(&er, er_store, sizeof(er_store));
		pb_str(&er, 1, uris[i]);
		pb_sub(&er, 2, &q);
		pb_sub(&req, 2, &er);          /* repeated entity_request */
	}
	if (!pb_ok(&req)) {
		snprintf(err, errlen, "metadata batch of %d did not fit", n);
		free(req_store);
		return -1;
	}

	http_reply r;
	int rc = sess
	    ? http_session_request(sess, "/extended-metadata/v0/extended-metadata",
	                           "POST", token, "application/x-protobuf",
	                           "application/x-protobuf", req.p, req.len, 0, &r)
	    : http_request(host, "/extended-metadata/v0/extended-metadata", "POST",
	                   token, "application/x-protobuf", "application/x-protobuf",
	                   req.p, req.len, 0, &r);
	free(req_store);
	if (rc != 0) {
		snprintf(err, errlen, "metadata batch: %s", r.err);
		return -1;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "metadata batch returned HTTP %d", r.status);
		http_reply_free(&r);
		return -1;
	}

	/* Placed by URI, not by position: the answer comes back shuffled. */
	int filled = sp_parse_track_batch((const uint8_t *)r.body, r.len, uris, n,
	                                  out);
	http_reply_free(&r);
	if (filled == 0)
		snprintf(err, errlen, "no tracks in a %u byte response", (unsigned)r.len);
	return filled;
}

int sp_track_metadata_batch(const char *host, const char *token,
                            const char *const *uris, int n,
                            sp_track *out, char *err, size_t errlen)
{
	return metadata_batch_request(host, NULL, token, uris, n, out, err, errlen);
}

int sp_track_metadata_batch_s(http_session *sess, const char *token,
                              const char *const *uris, int n,
                              sp_track *out, char *err, size_t errlen)
{
	return metadata_batch_request(NULL, sess, token, uris, n, out, err, errlen);
}

int sp_search_tracks(const char *host, const char *token, const char *query,
                     char uris[][64], int max, char *err, size_t errlen)
{
	if (!host || !token || !query || !uris || max <= 0) {
		snprintf(err, errlen, "invalid search arguments");
		return -1;
	}

	char path[448];
	if (!sp_search_path(query, path, sizeof(path)))
		goto query_too_long;

	http_reply r;
	if (http_get(host, path, token, &r) != 0) {
		snprintf(err, errlen, "search: %s", r.err);
		return -1;
	}
	if (r.status != 200) {
		if (r.status == 429 && r.retry_after > 0)
			snprintf(err, errlen,
			         "search returned HTTP 429 (Retry-After: %d seconds)",
			         r.retry_after);
		else
			snprintf(err, errlen, "search returned HTTP %d", r.status);
		http_reply_free(&r);
		return -1;
	}

	int count = 0;
	size_t cursor = 0;
	const char *obj;
	size_t objlen;
	while (count < max && json_array_next(r.body, "tracks", &cursor, &obj, &objlen)) {
		char buf[256];
		if (!slice_to_buf(obj, objlen, buf, sizeof(buf)))
			continue;
		char uri[64];
		if (json_str(buf, "uri", uri, sizeof(uri)))
			snprintf(uris[count++], 64, "%s", uri);
	}
	http_reply_free(&r);
	if (count == 0)
		snprintf(err, errlen, "no tracks matched");
	return count;

query_too_long:
	snprintf(err, errlen, "search query is too long");
	return -1;
}

int sp_radio(const char *host, const char *token, const char *seed_uri,
             int count, sp_track *out, int max, char *err, size_t errlen)
{
	char path[256];
	snprintf(path, sizeof(path),
	         "/radio-apollo/v3/tracks/%s?autoplay=true&count=%d", seed_uri, count);

	http_reply r;
	if (http_get(host, path, token, &r) != 0) {
		snprintf(err, errlen, "radio: %s", r.err);
		return -1;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "radio returned HTTP %d", r.status);
		http_reply_free(&r);
		return -1;
	}

	int n = 0;
	size_t cursor = 0;
	const char *obj;
	size_t objlen;
	while (n < max && json_array_next(r.body, "tracks", &cursor, &obj, &objlen)) {
		char buf[1024];
		if (!slice_to_buf(obj, objlen, buf, sizeof(buf)))
			continue;
		memset(&out[n], 0, sizeof(out[n]));
		json_str(buf, "uri", out[n].uri, sizeof(out[n].uri));
		/* These live in the entry's nested "metadata" object; json_str looks
		 * at any depth, so one pass over the entry finds them. */
		json_str(buf, "title", out[n].name, sizeof(out[n].name));
		json_str(buf, "artist_name", out[n].artist, sizeof(out[n].artist));
		json_str(buf, "album_title", out[n].album, sizeof(out[n].album));
		if (out[n].uri[0])
			n++;
	}
	http_reply_free(&r);
	if (n == 0)
		snprintf(err, errlen, "radio returned no tracks");
	return n;
}

/* --------------------------------------------------------- storage resolve */

int sp_storage_resolve(const char *host, const char *token,
                       const uint8_t file_id[20], sp_storage *out,
                       char *err, size_t errlen)
{
	memset(out, 0, sizeof(*out));

	char hex[41];
	for (int i = 0; i < 20; i++)
		snprintf(hex + i * 2, 3, "%02x", file_id[i]);

	char path[128];
	snprintf(path, sizeof(path),
	         "/storage-resolve/files/audio/interactive/%s", hex);

	http_reply r;
	if (http_get_proto(host, path, token, &r) != 0) {
		snprintf(err, errlen, "storage-resolve: %s", r.err);
		return -1;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "storage-resolve returned HTTP %d", r.status);
		http_reply_free(&r);
		return -1;
	}

	const uint8_t *body = (const uint8_t *)r.body;
	uint64_t result = 0;
	pb_get_varint(body, r.len, 1, &result);
	out->result = (int)result;

	/* cdnurl is repeated, so the list is walked by hand. */
	size_t at = 0;
	while (at < r.len && out->count < SP_MAX_CDN) {
		pbv rest = { body + at, r.len - at };
		pbv url;
		if (!pb_get_bytes(rest.p, rest.len, 2, &url))
			break;
		if (url.len < sizeof(out->urls[0])) {
			memcpy(out->urls[out->count], url.p, url.len);
			out->urls[out->count][url.len] = '\0';
			out->count++;
		}
		at = (size_t)(url.p + url.len - body);
	}
	http_reply_free(&r);

	if (out->count == 0) {
		snprintf(err, errlen, "no CDN url (result %d)", out->result);
		return -1;
	}
	return out->count;
}

int sp_user_image(const char *token, char *url, size_t urllen,
                  char *err, size_t errlen, int *retry_after)
{
	http_reply r;
	memset(&r, 0, sizeof(r));
	if (retry_after)
		*retry_after = 0;
	if (http_get("api.spotify.com", "/v1/me", token, &r) != 0) {
		snprintf(err, errlen, "perfil: %s", r.err);
		http_reply_free(&r);
		return -1;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "perfil: HTTP %d", r.status);
		if (r.status == 429 && retry_after)
			*retry_after = r.retry_after;
		http_reply_free(&r);
		return -1;
	}

	/* images is an array of {height, width, url}, largest first; the hero
	 * art is 128px, so any of them downscales fine. */
	size_t cursor = 0;
	const char *obj;
	size_t objlen;
	url[0] = '\0';
	while (json_array_next(r.body, "images", &cursor, &obj, &objlen)) {
		char buf[512], cand[96];
		if (objlen >= sizeof(buf))
			continue;
		memcpy(buf, obj, objlen);
		buf[objlen] = '\0';
		if (!json_str(buf, "url", cand, sizeof(cand)))
			continue;
		if (strncmp(cand, "https://i.scdn.co/image/", 24) != 0 ||
		    strlen(cand) >= urllen)
			continue;   /* see the header: only the Spotify CDN is in scope */
		snprintf(url, urllen, "%s", cand);
		break;
	}
	http_reply_free(&r);
	if (!url[0]) {
		snprintf(err, errlen, "la cuenta no tiene foto usable");
		return -1;
	}
	return 0;
}

int sp_library_modify(const char *token, const char *track_id, bool save,
                      char *err, size_t errlen, int *retry_after)
{
	if (retry_after)
		*retry_after = 0;
	char path[96];
	snprintf(path, sizeof(path), "/v1/me/tracks?ids=%s", track_id);
	http_reply r;
	memset(&r, 0, sizeof(r));
	/* The ids query form takes an empty body; the JSON body form would need
	 * a Content-Type the tiny client does not bother with elsewhere. */
	if (http_request("api.spotify.com", path, save ? "PUT" : "DELETE",
	                 token, NULL, NULL, NULL, 0, 0, &r) != 0) {
		snprintf(err, errlen, "me gusta: %s", r.err);
		http_reply_free(&r);
		return -1;
	}
	if (r.status == 429 && retry_after)
		*retry_after = r.retry_after;
	if (r.status != 200) {
		snprintf(err, errlen, "me gusta: HTTP %d", r.status);
		http_reply_free(&r);
		return r.status == 403 ? 1 : -1;   /* 1: missing scope, re-pair */
	}
	http_reply_free(&r);
	return 0;
}
