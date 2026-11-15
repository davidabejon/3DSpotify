/* Phase 4: the library and the metadata.
 *
 * Chains the whole thing together for the first time: stored refresh token ->
 * OAuth access token -> access point login -> the reusable credentials
 * APWelcome returns -> a login5 Bearer token -> spclient. Then reads the
 * rootlist and a track's metadata, which is where the audio file ids live and
 * therefore what the next phase needs.
 *
 * Steps 11-13 came later, for the album and artist detail views: they read
 * the sample track's album and artist ids and dump what the service sends for
 * each, to fix the field numbers before the client parses them.
 */
#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ap3ds.h"
#include "cfg3ds.h"
#include "login5.h"
#include "http3ds.h"
#include "meta_parse.h"
#include "pair3ds.h"
#include "proto3ds.h"
#include "spclient.h"
#include "tls3ds.h"

#define SOC_BUFFER 0x100000

/* "Mr. Brightside" -- a track that is certainly on Spotify and in the
 * catalogue everywhere, so a failure here is this client's fault. */
#define SAMPLE_TRACK "spotify:track:3n3Ppam7vgaVa1iaRUc9Lp"

static FILE *logf;

static void say(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fputs(line, stdout);
	fputs(line, stderr);
	if (logf) {
		fputs(line, logf);
		fflush(logf);
	}
}

static void open_log(const char *argv0)
{
	if (argv0 && argv0[0]) {
		char p[320];
		snprintf(p, sizeof(p), "%s", argv0);
		char *slash = strrchr(p, '/');
		if (slash) {
			slash[1] = '\0';
			strncat(p, "spotify-meta.log", sizeof(p) - strlen(p) - 1);
			logf = fopen(p, "w");
		}
	}
	if (!logf)
		logf = fopen("sdmc:/spotify-meta.log", "w");
}

static void frame(void)
{
	gfxFlushBuffers();
	gfxSwapBuffers();
	gspWaitForVBlank();
}

/* ------------------------------------------ album and artist (detail views)
 *
 * Phase 0 of docs/plan_vistas_detalle.md: before the client learns to read
 * albums and artists, this dumps what the service really sends, so the field
 * numbers below are checked against the wire and not just against
 * librespot's metadata.proto. A wrong field number in protobuf does not fail;
 * it reads nothing.
 *
 * The kinds come from meta_parse.h, which states them. */

#define PROBE_MAX_URIS 64

static void to_hex(const uint8_t *p, size_t n, char *out)
{
	static const char HEX[] = "0123456789abcdef";
	for (size_t i = 0; i < n; i++) {
		out[i * 2] = HEX[p[i] >> 4];
		out[i * 2 + 1] = HEX[p[i] & 15];
	}
	out[n * 2] = '\0';
}

static void pbv_str(const pbv *v, char *out, size_t cap)
{
	size_t n = v->len < cap - 1 ? v->len : cap - 1;
	memcpy(out, v->p, n);
	out[n] = '\0';
}

/* The next occurrence of a repeated length-delimited field. pb_get_bytes only
 * yields the first, so the walk restarts just past the previous value, which
 * is always a field boundary. */
static bool next_bytes(const pbv *msg, uint32_t field, size_t *at, pbv *out)
{
	if (*at >= msg->len ||
	    !pb_get_bytes(msg->p + *at, msg->len - *at, field, out))
		return false;
	*at = (size_t)(out->p + out->len - msg->p);
	return true;
}

static bool read_varint(const uint8_t *p, size_t len, size_t *at, uint64_t *out)
{
	uint64_t v = 0;
	for (unsigned shift = 0; *at < len && shift < 64; shift += 7) {
		uint8_t b = p[(*at)++];
		v |= (uint64_t)(b & 0x7F) << shift;
		if (!(b & 0x80)) {
			*out = v;
			return true;
		}
	}
	return false;
}

/* One line per top-level field, with runs of the same repeated field folded
 * into a count. Varints are shown both raw and as zigzag, because which one
 * a field is (int32 or sint32) is exactly what this is here to settle. */
static void dump_fields(const char *what, const pbv *msg)
{
	say("   [%s: %u bytes]\n", what, (unsigned)msg->len);
	size_t at = 0;
	uint32_t last = 0;
	int repeats = 0;
	while (at < msg->len) {
		uint64_t key;
		if (!read_varint(msg->p, msg->len, &at, &key)) {
			say("     (bad key at %u)\n", (unsigned)at);
			return;
		}
		uint32_t f = (uint32_t)(key >> 3);
		unsigned wire = (unsigned)(key & 7);
		uint64_t v = 0;
		const uint8_t *data = NULL;
		if (wire == 0) {
			if (!read_varint(msg->p, msg->len, &at, &v))
				return;
		} else if (wire == 2) {
			if (!read_varint(msg->p, msg->len, &at, &v) || at + v > msg->len)
				return;
			data = msg->p + at;
			at += (size_t)v;
		} else if (wire == 1) {
			at += 8;
		} else if (wire == 5) {
			at += 4;
		} else {
			say("     f%u: wire %u, stopping\n", f, wire);
			return;
		}

		if (f == last) {
			repeats++;
			continue;
		}
		if (repeats)
			say("       ... and %d more f%u\n", repeats, last);
		repeats = 0;
		last = f;

		if (wire == 0) {
			say("     f%-3u varint %llu (zigzag %lld)\n", f,
			    (unsigned long long)v,
			    (long long)((int64_t)(v >> 1) ^ -(int64_t)(v & 1)));
		} else if (wire == 2) {
			bool text = v > 0;
			for (uint64_t i = 0; i < v && text; i++)
				if (data[i] < 0x20 || data[i] == 0x7F)
					text = false;
			if (text) {
				char s[49];
				pbv sv = { data, (size_t)v };
				pbv_str(&sv, s, sizeof(s));
				say("     f%-3u len %-5llu \"%s%s\"\n", f,
				    (unsigned long long)v, s, v > 48 ? "..." : "");
			} else if (v == 16) {
				char hex[33], b62[23];
				to_hex(data, 16, hex);
				sp_id_from_gid(data, b62);
				say("     f%-3u len 16    %s = %s\n", f, hex, b62);
			} else {
				say("     f%-3u len %llu\n", f, (unsigned long long)v);
			}
		} else {
			say("     f%-3u fixed%u\n", f, wire == 1 ? 64 : 32);
		}
	}
	if (repeats)
		say("       ... and %d more f%u\n", repeats, last);
}

/* One entity through extended-metadata, left raw. On success `r` holds the
 * body that `entity` points into; the caller frees it. */
static bool fetch_extended(const char *host, const char *token, const char *uri,
                           int kind, http_reply *r, pbv *entity)
{
	uint8_t q_store[16], er_store[192], req_store[256];
	pb q, er, req;
	pb_init(&q, q_store, sizeof(q_store));
	pb_varint(&q, 1, (uint64_t)kind);
	pb_init(&er, er_store, sizeof(er_store));
	pb_str(&er, 1, uri);
	pb_sub(&er, 2, &q);
	pb_init(&req, req_store, sizeof(req_store));
	pb_sub(&req, 2, &er);
	if (!pb_ok(&req)) {
		say("   request did not fit\n");
		return false;
	}

	if (http_request(host, "/extended-metadata/v0/extended-metadata", "POST",
	                 token, "application/x-protobuf", "application/x-protobuf",
	                 req.p, req.len, 0, r) != 0) {
		say("   extended-metadata kind %d: %s\n", kind, r->err);
		return false;
	}
	if (r->status != 200) {
		say("   extended-metadata kind %d: HTTP %d\n", kind, r->status);
		http_reply_free(r);
		return false;
	}

	const uint8_t *body = (const uint8_t *)r->body;
	pbv arr, ed, any, type_url;
	if (pb_get_bytes(body, r->len, 2, &arr) &&
	    pb_get_bytes(arr.p, arr.len, 3, &ed) &&
	    pb_get_bytes(ed.p, ed.len, 3, &any) &&
	    pb_get_bytes(any.p, any.len, 2, entity)) {
		char tu[96] = "?";
		if (pb_get_bytes(any.p, any.len, 1, &type_url))
			pbv_str(&type_url, tu, sizeof(tu));
		say("   extended-metadata kind %d: %u bytes, %s\n", kind,
		    (unsigned)r->len, tu);
		return true;
	}
	say("   extended-metadata kind %d: 200 but no entity (%u bytes)\n", kind,
	    (unsigned)r->len);
	http_reply_free(r);
	return false;
}

/* The older REST shape, tried only if extended-metadata has nothing. The
 * body is the bare Album/Artist message. */
static bool fetch_legacy(const char *host, const char *token, const char *type,
                         const uint8_t gid[16], http_reply *r, pbv *entity)
{
	char hex[33], path[96];
	to_hex(gid, 16, hex);
	snprintf(path, sizeof(path), "/metadata/4/%s/%s", type, hex);
	if (http_get_proto(host, path, token, r) != 0) {
		say("   GET /metadata/4/%s: %s\n", type, r->err);
		return false;
	}
	if (r->status != 200 || r->len == 0) {
		say("   GET /metadata/4/%s: HTTP %d, %u bytes\n", type, r->status,
		    (unsigned)r->len);
		http_reply_free(r);
		return false;
	}
	say("   GET /metadata/4/%s: %u bytes\n", type, (unsigned)r->len);
	entity->p = (const uint8_t *)r->body;
	entity->len = r->len;
	return true;
}

static bool fetch_entity(const char *host, const char *token, const char *type,
                         int kind, const uint8_t gid[16], http_reply *r,
                         pbv *entity)
{
	char b62[23], uri[64];
	sp_id_from_gid(gid, b62);
	snprintf(uri, sizeof(uri), "spotify:%s:%s", type, b62);
	say("   %s\n", uri);
	return fetch_extended(host, token, uri, kind, r, entity) ||
	       fetch_legacy(host, token, type, gid, r, entity);
}

/* Counts an ImageGroup's images and names their sizes (0 default, 1 small,
 * 2 large, 3 xlarge). */
static void show_images(const char *what, const pbv *group, uint32_t field)
{
	char sizes[64] = "";
	int n = 0;
	size_t at = 0;
	pbv img;
	while (next_bytes(group, field, &at, &img)) {
		uint64_t size = 0;
		pb_get_varint(img.p, img.len, 2, &size);
		size_t used = strlen(sizes);
		if (used < sizeof(sizes) - 4)
			snprintf(sizes + used, sizeof(sizes) - used, "%s%llu",
			         n ? "," : "", (unsigned long long)size);
		n++;
	}
	say("   %s: %d images, sizes %s\n", what, n, n ? sizes : "-");
}

/* Gives each collected URI a name in one batch, which is also what the
 * client will do with an album's or artist's track list. */
static void name_uris(const char *host, const char *token,
                      char uris[][64], int n)
{
	static sp_track named[8];
	static const char *ptrs[8];
	int show = n < 8 ? n : 8;
	if (show <= 0)
		return;
	for (int i = 0; i < show; i++)
		ptrs[i] = uris[i];
	char err[256];
	int got = sp_track_metadata_batch(host, token, ptrs, show, named,
	                                  err, sizeof(err));
	if (got < 0) {
		say("   batch failed: %s\n", err);
		return;
	}
	for (int i = 0; i < got; i++)
		say("     %2d. %s - %s  %d:%02d\n", i + 1, named[i].artist,
		    named[i].name, named[i].duration_ms / 60000,
		    (named[i].duration_ms / 1000) % 60);
	if (n > got)
		say("     ... and %d more\n", n - got);
}

static void probe_details(const char *host, const char *token,
                          const char *country)
{
	static char uris[PROBE_MAX_URIS][64];
	uint8_t album_gid[16], artist_gid[16];
	bool have_album = false, have_artist = false;
	http_reply r;
	pbv t;

	say("\n11. Track ids (number, disc, explicit, album/artist gid)\n");
	{
		const char *id = SAMPLE_TRACK + strlen("spotify:track:");
		uint8_t want[16];
		char back[23], hex[33];
		bool ok = sp_gid_from_id(id, want);
		to_hex(want, 16, hex);
		sp_id_from_gid(want, back);
		say("   base62 %s -> %s -> %s %s\n", id, ok ? hex : "(invalid)",
		    back, ok && strcmp(back, id) == 0 ? "(round trip ok)" :
		    "(ROUND TRIP FAILED)");

		if (!fetch_extended(host, token, SAMPLE_TRACK, EXT_KIND_TRACK_V4,
		                    &r, &t))
			return;
		pbv v;
		if (pb_get_bytes(t.p, t.len, 1, &v) && v.len == 16)
			say("   Track.gid %s the base62 in the uri\n",
			    memcmp(v.p, want, 16) == 0 ? "matches" : "DOES NOT MATCH");
		int64_t num = -1, disc = -1;
		uint64_t expl = 0;
		bool has_num = pb_get_sint(t.p, t.len, 5, &num);
		bool has_disc = pb_get_sint(t.p, t.len, 6, &disc);
		bool has_expl = pb_get_varint(t.p, t.len, 9, &expl);
		say("   number (5, sint) %s%lld, disc_number (6, sint) %s%lld, "
		    "explicit (9) %s%llu\n",
		    has_num ? "" : "absent ", (long long)num,
		    has_disc ? "" : "absent ", (long long)disc,
		    has_expl ? "" : "absent ", (unsigned long long)expl);

		pbv album, artist, gid;
		if (pb_get_bytes(t.p, t.len, 3, &album) &&
		    pb_get_bytes(album.p, album.len, 1, &gid) && gid.len == 16) {
			memcpy(album_gid, gid.p, 16);
			have_album = true;
			char b62[23];
			sp_id_from_gid(album_gid, b62);
			say("   Track.album.gid  -> spotify:album:%s\n", b62);
		} else {
			say("   Track.album.gid missing\n");
		}
		int nart = 0;
		size_t at = 0;
		while (next_bytes(&t, 4, &at, &artist)) {
			if (pb_get_bytes(artist.p, artist.len, 1, &gid) &&
			    gid.len == 16) {
				char b62[23], name[64] = "";
				pbv nm;
				if (pb_get_bytes(artist.p, artist.len, 2, &nm))
					pbv_str(&nm, name, sizeof(name));
				sp_id_from_gid(gid.p, b62);
				say("   Track.artist[%d] -> spotify:artist:%s  %s\n", nart,
				    b62, name);
				if (!have_artist) {
					memcpy(artist_gid, gid.p, 16);
					have_artist = true;
				}
			}
			nart++;
		}
		dump_fields("Track", &t);
		if (have_album) {
			pb_get_bytes(t.p, t.len, 3, &album);
			dump_fields("Track.album (embedded)", &album);
		}
		http_reply_free(&r);
	}

	say("\n12. Album\n");
	if (!have_album) {
		say("   skipped: no album gid\n");
	} else if (fetch_entity(host, token, "album", EXT_KIND_ALBUM_V4, album_gid,
	                        &r, &t)) {
		pbv v;
		char s[128];
		if (pb_get_bytes(t.p, t.len, 2, &v)) {
			pbv_str(&v, s, sizeof(s));
			say("   name (2): %s\n", s);
		}
		int nart = 0;
		size_t at = 0;
		while (next_bytes(&t, 3, &at, &v)) {
			pbv nm;
			if (nart == 0 && pb_get_bytes(v.p, v.len, 2, &nm)) {
				pbv_str(&nm, s, sizeof(s));
				say("   artist[0] (3): %s\n", s);
			}
			nart++;
		}
		say("   %d artists\n", nart);
		uint64_t type = 0;
		if (pb_get_varint(t.p, t.len, 4, &type))
			say("   type (4): %llu  (1 album, 2 single, 3 compilation, 4 ep)\n",
			    (unsigned long long)type);
		if (pb_get_bytes(t.p, t.len, 20, &v)) {
			pbv_str(&v, s, sizeof(s));
			say("   type_str (20): %s\n", s);
		}
		if (pb_get_bytes(t.p, t.len, 5, &v)) {
			pbv_str(&v, s, sizeof(s));
			say("   label (5): %s\n", s);
		}
		pbv date;
		if (pb_get_bytes(t.p, t.len, 6, &date)) {
			int64_t y = 0, m = 0, d = 0;
			uint64_t ry = 0;
			pb_get_sint(date.p, date.len, 1, &y);
			pb_get_sint(date.p, date.len, 2, &m);
			pb_get_sint(date.p, date.len, 3, &d);
			pb_get_varint(date.p, date.len, 1, &ry);
			say("   date (6): %lld-%02lld-%02lld as sint32 (year raw %llu)\n",
			    (long long)y, (long long)m, (long long)d,
			    (unsigned long long)ry);
		} else {
			say("   date (6) missing\n");
		}
		pbv group;
		if (pb_get_bytes(t.p, t.len, 17, &group))
			show_images("cover_group (17)", &group, 1);
		show_images("cover (9)", &t, 9);

		int ndisc = 0, n = 0;
		bool dumped = false;
		pbv disc;
		at = 0;
		while (next_bytes(&t, 11, &at, &disc)) {
			int64_t number = 0;
			pb_get_sint(disc.p, disc.len, 1, &number);
			int ntr = 0;
			size_t tat = 0;
			pbv tr, gid;
			while (next_bytes(&disc, 3, &tat, &tr)) {
				if (!dumped) {
					dump_fields("Disc.track[0]", &tr);
					dumped = true;
				}
				if (n < PROBE_MAX_URIS &&
				    pb_get_bytes(tr.p, tr.len, 1, &gid) && gid.len == 16) {
					char b62[23];
					sp_id_from_gid(gid.p, b62);
					snprintf(uris[n++], 64, "spotify:track:%s", b62);
				}
				ntr++;
			}
			say("   disc (11) #%d: number (1, sint) %lld, %d tracks\n",
			    ndisc, (long long)number, ntr);
			ndisc++;
		}
		say("   %d discs, %d track uris collected\n", ndisc, n);
		dump_fields("Album", &t);
		http_reply_free(&r);
		name_uris(host, token, uris, n);
	}

	say("\n13. Artist (account country \"%s\")\n", country);
	if (!have_artist) {
		say("   skipped: no artist gid\n");
	} else if (fetch_entity(host, token, "artist", EXT_KIND_ARTIST_V4,
	                        artist_gid, &r, &t)) {
		pbv v;
		char s[128];
		if (pb_get_bytes(t.p, t.len, 2, &v)) {
			pbv_str(&v, s, sizeof(s));
			say("   name (2): %s\n", s);
		}
		pbv group;
		if (pb_get_bytes(t.p, t.len, 17, &group))
			show_images("portrait_group (17)", &group, 1);
		else
			say("   portrait_group (17) missing\n");
		show_images("portrait (11)", &t, 11);

		/* top_track (4): one TopTracks { country 1, track 2 } per country. */
		char countries[256] = "";
		int ngroups = 0, pick = -1;
		pbv top, chosen = { NULL, 0 };
		size_t at = 0;
		while (next_bytes(&t, 4, &at, &top)) {
			char c[8] = "";
			pbv cv;
			if (pb_get_bytes(top.p, top.len, 1, &cv))
				pbv_str(&cv, c, sizeof(c));
			size_t used = strlen(countries);
			if (used < sizeof(countries) - 8)
				snprintf(countries + used, sizeof(countries) - used, "%s%s",
				         ngroups ? " " : "", c[0] ? c : "\"\"");
			if (pick < 0 && country[0] && strcmp(c, country) == 0) {
				pick = ngroups;
				chosen = top;
			}
			if (ngroups == 0 && !chosen.p)
				chosen = top;
			ngroups++;
		}
		say("   top_track (4): %d groups: %s\n", ngroups, countries);
		say("   using group %d (%s)\n", pick >= 0 ? pick : 0,
		    pick >= 0 ? "account country" : "first, no match");

		int n = 0;
		if (chosen.p) {
			size_t tat = 0;
			pbv tr, gid;
			bool dumped = false;
			while (next_bytes(&chosen, 2, &tat, &tr)) {
				if (!dumped) {
					dump_fields("TopTracks.track[0]", &tr);
					dumped = true;
				}
				if (n < PROBE_MAX_URIS &&
				    pb_get_bytes(tr.p, tr.len, 1, &gid) && gid.len == 16) {
					char b62[23];
					sp_id_from_gid(gid.p, b62);
					snprintf(uris[n++], 64, "spotify:track:%s", b62);
				}
			}
		}
		say("   %d top track uris\n", n);
		dump_fields("Artist", &t);
		http_reply_free(&r);
		name_uris(host, token, uris, n);
	}
}

/* Step 14: the same three through the client's own calls (spclient.c and
 * meta_parse.c), over one keep-alive session as the detail loader will use
 * them. Steps 11-13 settle the wire format; this checks the code that reads
 * it. */
static void show_named(http_session *sess, const char *token,
                       char uris[][SP_URI_MAX], int n)
{
	static sp_track named[SP_ALBUM_MAX_TRACKS];
	static const char *ptrs[SP_ALBUM_MAX_TRACKS];
	char err[256];
	for (int i = 0; i < n; i++)
		ptrs[i] = uris[i];
	int got = sp_track_metadata_batch_s(sess, token, ptrs, n, named,
	                                    err, sizeof(err));
	if (got < 0) {
		say("   batch failed: %s\n", err);
		return;
	}
	for (int i = 0; i < got; i++)
		say("     %d-%02d %s%s  %d:%02d  %d\n", named[i].disc_number,
		    named[i].track_number, named[i].name,
		    named[i].is_explicit ? " [E]" : "",
		    named[i].duration_ms / 60000,
		    (named[i].duration_ms / 1000) % 60, named[i].year);
}

static void probe_client_calls(const char *host, const char *token,
                               const char *country)
{
	static sp_track t;
	static sp_album album;
	static sp_artist artist;
	char err[256];

	say("\n14. Client calls (sp_album_metadata, sp_artist_metadata)\n");
	if (sp_track_metadata(host, token, SAMPLE_TRACK, &t, err, sizeof(err))) {
		say("   track failed: %s\n", err);
		return;
	}
	say("   %s  #%d disc %d  %d%s\n   album  %s\n   artist %s\n", t.name,
	    t.track_number, t.disc_number, t.year, t.is_explicit ? "  [E]" : "",
	    t.album_uri[0] ? t.album_uri : "(none)",
	    t.artist_uri[0] ? t.artist_uri : "(none)");

	http_session *sess = http_session_open(host);
	if (!sess) {
		say("   http_session_open failed\n");
		return;
	}
	if (!t.album_uri[0]) {
		say("   no album uri\n");
	} else if (sp_album_metadata(sess, token, t.album_uri, &album,
	                             err, sizeof(err))) {
		say("   album failed: %s\n", err);
	} else {
		say("   %s - %s (%d), type %d, %s\n   %d of %d tracks, art %s\n",
		    album.artist, album.name, album.year, album.type, album.label,
		    album.track_count, album.total_tracks,
		    album.art_large[0] ? "yes" : "no");
		show_named(sess, token, album.track_uris, album.track_count);
	}
	if (!t.artist_uri[0]) {
		say("   no artist uri\n");
	} else if (sp_artist_metadata(sess, token, t.artist_uri, country, &artist,
	                              err, sizeof(err))) {
		say("   artist failed: %s\n", err);
	} else {
		say("   %s: %d top tracks, art %s\n", artist.name, artist.track_count,
		    artist.art_large[0] ? "yes" : "no");
		show_named(sess, token, artist.track_uris, artist.track_count);
	}
	say("   session reused on the last request: %d\n",
	    http_session_last_reused(sess));
	http_session_close(sess);
}

/* ------------------------------------------------------- home tab (step 15)
 *
 * Phase 0 of the home tab plan: what the rootlist really carries per entry
 * (names, covers in their several shapes, folders, owners, lengths), whether
 * a playback history answers on spclient with the login5 token, how heavy a
 * long playlist is and whether it takes a range, and how big a playlist cover
 * is next to the thumbnail limits. playlist4_external.proto names used here:
 *   SelectedListContent { length 2, attributes 3, contents 5, owner 16 }
 *   ListItems { items 3, meta_items 4 }   Item { uri 1 }
 *   MetaItem { attributes 2, length 3, owner_username 5 }
 *   ListAttributes { name 1, description 2, picture 3,
 *                    format_attributes 11? 12? { key 1, value 2 },
 *                    picture_size 13 { target_name 1, url 2 } }
 * The field numbers in doubt are exactly what the dump settles. */

#define HOME_PROBE_MAX 64

static struct {
	char uri[80];
	char pic_hex[41];
	int  length;
} home_rows[HOME_PROBE_MAX];

/* JPEG width/height off the first SOFn marker; 0 when not a JPEG. */
static bool jpeg_size(const uint8_t *p, size_t len, int *w, int *h)
{
	if (len < 4 || p[0] != 0xFF || p[1] != 0xD8)
		return false;
	size_t at = 2;
	while (at + 9 < len) {
		if (p[at] != 0xFF) {
			at++;
			continue;
		}
		uint8_t m = p[at + 1];
		size_t seg = ((size_t)p[at + 2] << 8) | p[at + 3];
		if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
			*h = (p[at + 5] << 8) | p[at + 6];
			*w = (p[at + 7] << 8) | p[at + 8];
			return true;
		}
		at += 2 + seg;
	}
	return false;
}

static void show_attrs(const pbv *attrs)
{
	dump_fields("ListAttributes", attrs);
	pbv v;
	if (pb_get_bytes(attrs->p, attrs->len, 3, &v))
		say("     picture: %u bytes\n", (unsigned)v.len);
	for (uint32_t f = 11; f <= 13; f++) {
		size_t at = 0;
		pbv sub;
		while (next_bytes(attrs, f, &at, &sub)) {
			pbv k, val;
			char ks[40] = "", vs[100] = "";
			if (pb_get_bytes(sub.p, sub.len, 1, &k))
				pbv_str(&k, ks, sizeof(ks));
			if (pb_get_bytes(sub.p, sub.len, 2, &val))
				pbv_str(&val, vs, sizeof(vs));
			say("     f%u { 1:\"%s\" 2:(%u) \"%.60s\" }\n", f, ks,
			    (unsigned)(val.p ? val.len : 0), vs);
		}
	}
}

static void probe_cover(const char *url)
{
	http_reply r;
	if (http_get_url(url, 1024 * 1024, &r) != 0) {
		say("     GET %s: %s\n", url, r.err);
		return;
	}
	int w = 0, h = 0;
	bool jpg = jpeg_size((const uint8_t *)r.body, r.len, &w, &h);
	say("     %.70s\n       HTTP %d, %u bytes, %s %dx%d\n", url, r.status,
	    (unsigned)r.len, jpg ? "jpeg" : "not jpeg", w, h);
	http_reply_free(&r);
}

static void probe_home(const char *host, const char *token,
                       const char *username)
{
	say("\n15. Home tab data\n");
	char err[256];

	/* 15.1 the decorated rootlist, raw. */
	say(" 15.1 rootlist, decorated\n");
	char path[320];
	snprintf(path, sizeof(path),
	         "/playlist/v2/user/%s/rootlist?decorate=revision,attributes,length",
	         username);
	http_reply r;
	int nrows = 0, folders = 0, others = 0;
	if (http_get_proto(host, path, token, &r) != 0) {
		say("   failed: %s\n", r.err);
	} else if (r.status != 200) {
		say("   HTTP %d\n", r.status);
		http_reply_free(&r);
	} else {
		pbv whole = { (const uint8_t *)r.body, r.len };
		dump_fields("SelectedListContent", &whole);
		pbv contents;
		if (pb_get_bytes(whole.p, whole.len, 5, &contents)) {
			dump_fields("ListItems", &contents);
			size_t ai = 0, am = 0;
			pbv item, meta;
			int i = 0;
			while (next_bytes(&contents, 3, &ai, &item)) {
				bool has_meta = next_bytes(&contents, 4, &am, &meta);
				pbv u;
				char uri[80] = "";
				if (pb_get_bytes(item.p, item.len, 1, &u))
					pbv_str(&u, uri, sizeof(uri));
				if (strncmp(uri, "spotify:start-group:", 20) == 0 ||
				    strncmp(uri, "spotify:end-group:", 18) == 0)
					folders++;
				else if (strncmp(uri, "spotify:playlist:", 17) != 0)
					others++;
				say("   [%d] %s\n", i, uri);
				if (i == 0)
					dump_fields("Item", &item);
				uint64_t length = 0;
				if (has_meta) {
					pb_get_varint(meta.p, meta.len, 3, &length);
					pbv attrs, owner;
					if (i < 4) {
						dump_fields("MetaItem", &meta);
						if (pb_get_bytes(meta.p, meta.len, 2, &attrs))
							show_attrs(&attrs);
					} else if (pb_get_bytes(meta.p, meta.len, 2, &attrs)) {
						pbv nm, pic;
						char ns[64] = "";
						if (pb_get_bytes(attrs.p, attrs.len, 1, &nm))
							pbv_str(&nm, ns, sizeof(ns));
						say("     \"%s\" pic %u, len %llu\n", ns,
						    pb_get_bytes(attrs.p, attrs.len, 3, &pic)
						        ? (unsigned)pic.len : 0,
						    (unsigned long long)length);
					}
					say("     owner is %s\n",
					    pb_get_bytes(meta.p, meta.len, 5, &owner)
					        ? (owner.len == strlen(username) &&
					           memcmp(owner.p, username, owner.len) == 0
					               ? "the user" : "someone else")
					        : "absent");
				}
				if (i < HOME_PROBE_MAX) {
					snprintf(home_rows[i].uri, sizeof(home_rows[i].uri),
					         "%s", uri);
					home_rows[i].length = (int)length;
					home_rows[i].pic_hex[0] = '\0';
					pbv attrs, pic;
					if (has_meta &&
					    pb_get_bytes(meta.p, meta.len, 2, &attrs) &&
					    pb_get_bytes(attrs.p, attrs.len, 3, &pic) &&
					    pic.len == 20)
						to_hex(pic.p, 20, home_rows[i].pic_hex);
					nrows = i + 1;
				}
				i++;
			}
			say("   %d entries: %d folder markers, %d not playlists\n", i,
			    folders, others);
		}
		http_reply_free(&r);
	}

	/* 15.2 playback history, as the desktop client asks for it. */
	say(" 15.2 recently played\n");
	static const char *const HIST[] = {
		"/recently-played/v3/user/%s/recently-played"
		"?format=json&offset=0&limit=50&filter=default,collection-new-episodes",
		"/recently-played/v3/user/%s/recently-played?format=json&limit=20",
		"/recently-played/v2/user/%s/recently-played?format=json&limit=20",
	};
	for (size_t k = 0; k < sizeof(HIST) / sizeof(HIST[0]); k++) {
		snprintf(path, sizeof(path), HIST[k], username);
		const char *q = strchr(path, '?');
		say("   v%c ...%s\n", path[18], q ? q : "");
		if (http_get(host, path, token, &r) != 0) {
			say("     failed: %s\n", r.err);
			continue;
		}
		say("     HTTP %d, %u bytes\n", r.status, (unsigned)r.len);
		if (r.len) {
			/* Contexts only: print each "uri" value, not the whole body. */
			int shown = 0;
			for (const char *s = r.body; (s = strstr(s, "spotify:")) && shown < 12;
			     s++) {
				const char *e = s;
				while (*e && *e != '"' && e - s < 70)
					e++;
				say("       %.*s\n", (int)(e - s), s);
				shown++;
				s = e;
			}
			if (!shown)
				say("     body: %.200s\n", r.body);
			/* The shape around the URIs, for the parser. */
			for (size_t at = 0; at < r.len && at < 900; at += 150)
				say("     | %.150s\n", r.body + at);
		}
		http_reply_free(&r);
		if (r.status == 200)
			break;
	}

	/* 15.3 the longest playlist: whole, then with a range. */
	say(" 15.3 playlist size and range\n");
	int big = -1;
	for (int i = 0; i < nrows; i++)
		if (strncmp(home_rows[i].uri, "spotify:playlist:", 17) == 0 &&
		    (big < 0 || home_rows[i].length > home_rows[big].length))
			big = i;
	if (big < 0) {
		say("   no playlist\n");
	} else {
		const char *id = home_rows[big].uri + 17;
		say("   %s, %d tracks by the rootlist\n", home_rows[big].uri,
		    home_rows[big].length);
		static const char *const Q[] = {
			"", "?from=0&length=24", "?decorate=attributes,length&from=0&length=24",
		};
		for (size_t k = 0; k < sizeof(Q) / sizeof(Q[0]); k++) {
			snprintf(path, sizeof(path), "/playlist/v2/playlist/%s%s", id, Q[k]);
			say("   %s\n", Q[k][0] ? Q[k] : "(whole)");
			if (http_get_proto(host, path, token, &r) != 0) {
				say("     failed: %s\n", r.err);
				continue;
			}
			say("     HTTP %d, %u bytes\n", r.status, (unsigned)r.len);
			if (r.status == 200) {
				pbv whole = { (const uint8_t *)r.body, r.len }, contents, attrs;
				if (k == 0) {
					dump_fields("SelectedListContent", &whole);
					if (pb_get_bytes(whole.p, whole.len, 3, &attrs))
						show_attrs(&attrs);
				}
				if (pb_get_bytes(whole.p, whole.len, 5, &contents)) {
					dump_fields("ListItems", &contents);
					size_t at = 0;
					pbv item;
					int n = 0, tracks = 0;
					while (next_bytes(&contents, 3, &at, &item)) {
						pbv u;
						if (pb_get_bytes(item.p, item.len, 1, &u) &&
						    u.len > 14 && memcmp(u.p, "spotify:track:", 14) == 0)
							tracks++;
						else if (n < 40 && pb_get_bytes(item.p, item.len, 1, &u)) {
							char us[64];
							pbv_str(&u, us, sizeof(us));
							say("     non-track: %s\n", us);
						}
						n++;
					}
					say("     %d items, %d tracks\n", n, tracks);
				}
				pbv owner;
				if (pb_get_bytes(whole.p, whole.len, 16, &owner))
					say("     owner_username (16): %u chars\n",
					    (unsigned)owner.len);
			}
			http_reply_free(&r);
		}
	}

	/* 15.4 covers: picture ids through i.scdn.co. */
	say(" 15.4 covers\n");
	int covers = 0;
	for (int i = 0; i < nrows && covers < 3; i++) {
		if (!home_rows[i].pic_hex[0])
			continue;
		char url[96];
		snprintf(url, sizeof(url), "https://i.scdn.co/image/%s",
		         home_rows[i].pic_hex);
		probe_cover(url);
		covers++;
	}
	if (!covers)
		say("   no 20-byte picture in the rootlist\n");
	/* 15.5 a playlist's own header without its tracks: what a recent
	 * playlist missing from the rootlist can be named and drawn from. */
	say(" 15.5 header only\n");
	static const char *const HDR[] = {
		"5pi5MhptTMnmV8L8Q6cJv1", "37i9dQZF1EYkqdzj48dyYq",
		"7ccWAm7GdeAUpA2Eu9YI5q",
	};
	for (size_t k = 0; k < sizeof(HDR) / sizeof(HDR[0]); k++) {
		snprintf(path, sizeof(path),
		         "/playlist/v2/playlist/%s?decorate=attributes,length&from=0&length=0",
		         HDR[k]);
		say("   %s\n", HDR[k]);
		if (http_get_proto(host, path, token, &r) != 0) {
			say("     failed: %s\n", r.err);
			continue;
		}
		say("     HTTP %d, %u bytes\n", r.status, (unsigned)r.len);
		if (r.status == 200) {
			pbv whole = { (const uint8_t *)r.body, r.len }, attrs;
			dump_fields("SelectedListContent", &whole);
			if (pb_get_bytes(whole.p, whole.len, 3, &attrs))
				show_attrs(&attrs);
		}
		http_reply_free(&r);
	}
	(void)err;
}

/* ---------------------------------------------- liked songs paging (16)
 *
 * The home tab's "Liked Songs" tile needs more than the first page. Two
 * candidates: context-resolve (JSON, what sp_liked reads), whose pages may
 * name the next one, and the collection service (protobuf, what the
 * desktop client pages with): PageRequest { username 1, set 2,
 * pagination_token 3, limit 4 } -> PageResponse { items 1 (repeated
 * { uri 1, added_at 2, is_removed 3 }), next_page_token 2 }. */
static void probe_liked(const char *host, const char *token,
                        const char *username)
{
	say("\n16. Liked songs paging\n");
	char path[320];
	http_reply r;
	snprintf(path, sizeof(path),
	         "/context-resolve/v1/spotify:user:%s:collection", username);
	say(" 16.1 context-resolve\n");
	if (http_get(host, path, token, &r) != 0) {
		say("   failed: %s\n", r.err);
	} else {
		say("   HTTP %d, %u bytes\n", r.status, (unsigned)r.len);
		int tracks = 0;
		for (const char *s = r.body; (s = strstr(s, "spotify:track:")); s++)
			tracks++;
		say("   track uris: %d\n", tracks);
		static const char *const KEYS[] = {
			"\"next_page_url\"", "\"page_url\"", "\"next_page\"",
			"\"pages\"", "\"metadata\"", "\"restrictions\"",
		};
		for (size_t k = 0; k < sizeof(KEYS) / sizeof(KEYS[0]); k++) {
			const char *at = r.body ? strstr(r.body, KEYS[k]) : NULL;
			if (at)
				say("   %s at %u: %.110s\n", KEYS[k],
				    (unsigned)(at - r.body), at);
		}
		for (size_t at = 0; at < r.len && at < 450; at += 150)
			say("   | %.150s\n", r.body + at);
		if (r.len > 300)
			say("   ...| %.300s\n", r.body + r.len - 300);
		http_reply_free(&r);
	}

	say(" 16.2 collection/v2/paging\n");
	for (int page = 0; page < 3; page++) {
		static char next[256];
		if (page == 0)
			next[0] = '\0';
		uint8_t store[512];
		pb req;
		pb_init(&req, store, sizeof(store));
		pb_str(&req, 1, username);
		pb_str(&req, 2, "collection");
		if (next[0])
			pb_str(&req, 3, next);
		pb_varint(&req, 4, 10);
		if (!pb_ok(&req)) {
			say("   request did not fit\n");
			return;
		}
		if (http_request(host, "/collection/v2/paging", "POST", token,
		                 "application/vnd.collection-v2.spotify.proto",
		                 "application/vnd.collection-v2.spotify.proto",
		                 req.p, req.len, 0, &r) != 0) {
			say("   failed: %s\n", r.err);
			return;
		}
		say("   page %d: HTTP %d, %u bytes\n", page, r.status, (unsigned)r.len);
		if (r.status != 200) {
			say("   body: %.200s\n", r.body ? r.body : "");
			http_reply_free(&r);
			return;
		}
		pbv whole = { (const uint8_t *)r.body, r.len };
		dump_fields("PageResponse", &whole);
		size_t at = 0;
		pbv item;
		int n = 0;
		while (next_bytes(&whole, 1, &at, &item)) {
			pbv u;
			char us[64] = "";
			if (pb_get_bytes(item.p, item.len, 1, &u))
				pbv_str(&u, us, sizeof(us));
			if (n < 3)
				say("     %s\n", us);
			n++;
		}
		say("   %d items\n", n);
		pbv tok;
		next[0] = '\0';
		if (pb_get_bytes(whole.p, whole.len, 2, &tok))
			pbv_str(&tok, next, sizeof(next));
		say("   next token: %u chars\n", (unsigned)strlen(next));
		http_reply_free(&r);
		if (!next[0])
			break;
	}
}

int main(int argc, char **argv)
{
	(void)argc;
	gfxInitDefault();
	consoleInit(GFX_TOP, NULL);
	const char *argv0 = (argv && argv[0]) ? argv[0] : NULL;
	open_log(argv0);
	cfg_set_path(argv0);

	say("3DS Spotify -- library & metadata (phase 4)\n");
	say("built %s\n\n", BUILD_STAMP);

	u32 *soc = memalign(0x1000, SOC_BUFFER);
	if (!soc || R_FAILED(socInit(soc, SOC_BUFFER))) {
		say("socInit failed; is Wi-Fi on?\n");
		goto hold;
	}
	if (R_FAILED(psInit())) {
		say("psInit failed.\n");
		goto hold;
	}
	int rc = tls_global_init();
	if (rc) {
		char reason[128];
		tls_strerror(rc, reason, sizeof(reason));
		say("tls_global_init: %s\n", reason);
		goto hold;
	}

	char err[256];
	char stored[512];
	if (!cfg_load_refresh_token(stored, sizeof(stored))) {
		say("Not paired. Run probe_pair first.\n");
		goto hold;
	}

	spotify_tokens tok;
	say("1. OAuth access token...\n");
	if (token_refresh(stored, &tok, err, sizeof(err)) != 0) {
		say("   failed: %s\n", err);
		goto hold;
	}
	if (tok.refresh_token[0] && strcmp(tok.refresh_token, stored) != 0)
		cfg_save_refresh_token(tok.refresh_token, tok.scope);
	say("   %u chars\n", (unsigned)strlen(tok.access_token));

	char device_id[64];
	ap_device_id(device_id, sizeof(device_id));

	say("2. Access point...\n");
	ap_candidate cands[AP_MAX_CANDIDATES];
	int ncands = ap_resolve_list(cands, AP_MAX_CANDIDATES, err, sizeof(err));
	if (ncands < 1) {
		say("   apresolve failed: %s\n", err);
		goto hold;
	}
	say("   %d candidates, first %s:%u\n", ncands, cands[0].host, cands[0].port);
	ap_info info;
	int tried = 0;
	ap_session *ap = ap_login_any(cands, ncands, tok.access_token, device_id,
	                              &info, &tried, err, sizeof(err));
	if (!ap) {
		say("   all %d failed, last: %s\n", tried, err);
		goto hold;
	}
	say("   %s (attempt %d), %s, %u bytes of credentials\n",
	    info.ap, tried, info.username, (unsigned)info.reusable_len);

	/* The AP session is not needed for the rest of this phase, but closing it
	 * is deliberate rather than accidental: the next phase keeps it open,
	 * because audio keys only come from here. */
	ap_close(ap);

	say("3. login5 token...\n");
	login5_token l5;
	if (login5_from_credentials(info.username, info.reusable, info.reusable_len,
	                            device_id, &l5, err, sizeof(err)) != 0) {
		say("   failed: %s\n", err);
		goto hold;
	}
	say("   %u chars, valid %ld s\n", (unsigned)strlen(l5.access_token),
	    (long)(l5.expires_at - time(NULL)));

	say("4. spclient host...\n");
	char sp_host[128];
	err[0] = '\0';
	sp_resolve(sp_host, sizeof(sp_host), err, sizeof(err));
	say("   %s%s%s\n", sp_host, err[0] ? "  -- " : "", err[0] ? err : "");

	say("\n5. Track metadata\n");
	sp_track t;
	if (sp_track_metadata(sp_host, l5.access_token, SAMPLE_TRACK, &t,
	                      err, sizeof(err)) != 0) {
		say("   failed: %s\n", err);
	} else {
		say("   %s\n   by %s\n   from %s\n   %d:%02d\n", t.name, t.artist,
		    t.album, t.duration_ms / 60000, (t.duration_ms / 1000) % 60);
		say("   %d audio files:\n", t.file_count);
		for (int i = 0; i < t.file_count; i++) {
			char hex[41];
			for (int j = 0; j < 20; j++)
				snprintf(hex + j * 2, 3, "%02x", t.files[i].id[j]);
			say("     %s  %s\n", hex, sp_format_name(t.files[i].format));
		}
		const sp_file *pick = sp_pick_file(&t);
		say("   pick for this console: %s\n",
		    pick ? sp_format_name(pick->format) : "none playable!");
	}

	say("\n6. Search (no library needed)\n");
	static char found[8][64];
	int nfound = sp_search_tracks(sp_host, l5.access_token, "the killers",
	                              found, 8, err, sizeof(err));
	if (nfound < 0) {
		say("   failed: %s\n", err);
	} else {
		say("   %d track uris; naming them in one batch...\n", nfound);
		static sp_track batch[8];
		static const char *ptrs[8];
		for (int i = 0; i < nfound; i++)
			ptrs[i] = found[i];
		int named = sp_track_metadata_batch(sp_host, l5.access_token, ptrs,
		                                    nfound, batch, err, sizeof(err));
		if (named < 0) {
			say("   batch failed: %s\n", err);
		} else {
			for (int i = 0; i < named; i++) {
				const sp_file *f = sp_pick_file(&batch[i]);
				say("     %s - %s  %d:%02d  %s\n", batch[i].artist,
				    batch[i].name, batch[i].duration_ms / 60000,
				    (batch[i].duration_ms / 1000) % 60,
				    f ? sp_format_name(f->format) : "no playable file");
			}
		}
	}

	say("\n7. Radio from one track (titles come inline)\n");
	static sp_track radio[8];
	int nradio = sp_radio(sp_host, l5.access_token, SAMPLE_TRACK, 8, radio, 8,
	                      err, sizeof(err));
	if (nradio < 0) {
		say("   failed: %s\n", err);
	} else {
		for (int i = 0; i < nradio; i++)
			say("     %s - %s  (%s)\n", radio[i].artist, radio[i].name,
			    radio[i].album);
	}

	say("\n8. Rootlist\n");
	static sp_playlist_ref refs[16];
	int count = 0;
	http_session *plsess = http_session_open(sp_host);
	int got = plsess ? sp_rootlist_s(plsess, l5.access_token, info.username,
	                                 refs, 16, &count, err, sizeof(err)) : -1;
	if (got < 0) {
		say("   failed: %s\n", plsess ? err : "no session");
	} else {
		say("   length field %d, %d playlists kept\n", count, got);
		for (int i = 0; i < got; i++)
			say("     %s \"%s\" %d%s\n", refs[i].uri, refs[i].name,
			    refs[i].length, refs[i].art[0] ? " +art" : "");
		if (count == 0 && got == 0)
			say("   (the account's playlist folder is empty)\n");
	}

	say("\n9. Contents of the first playlist\n");
	if (got < 1) {
		say("   no playlist in the rootlist\n");
	} else {
		static sp_playlist_info pl;
		if (sp_playlist_s(plsess, l5.access_token, refs[0].uri, 0,
		                  SP_PLAYLIST_PAGE, &pl, err, sizeof(err)) != 0) {
			say("   failed: %s\n", err);
		} else {
			say("   \"%s\": %d tracks, %d read, %d skipped\n", pl.name,
			    pl.total_tracks, pl.track_count, pl.skipped);
			static const char *ptrs2[8];
			int show = pl.track_count < 8 ? pl.track_count : 8;
			for (int i = 0; i < show; i++)
				ptrs2[i] = pl.track_uris[i];
			static sp_track batch2[8];
			int named = sp_track_metadata_batch_s(plsess, l5.access_token,
			                                      ptrs2, show, batch2,
			                                      err, sizeof(err));
			for (int i = 0; i < named; i++)
				say("     %s - %s\n", batch2[i].artist, batch2[i].name);
		}
	}
	if (plsess) {
		static char recent[8][SP_URI_MAX];
		int nr = sp_recent_playlists_s(plsess, l5.access_token, info.username,
		                               recent, NULL, 8, err, sizeof(err));
		say("   recent playlists: %d%s%s\n", nr, nr < 0 ? " -- " : "",
		    nr < 0 ? err : "");
		for (int i = 0; i < nr; i++)
			say("     %s\n", recent[i]);
		http_session_close(plsess);
	}

	say("\n10. Liked Songs\n");
	{
		static char liked[24][64];
		int n = sp_liked(sp_host, l5.access_token, info.username, liked, 24,
		                 err, sizeof(err));
		if (n < 0) {
			say("   failed: %s\n", err);
		} else if (n == 0) {
			say("   (nothing saved)\n");
		} else {
			static const char *ptrs3[8];
			int show = n < 8 ? n : 8;
			for (int i = 0; i < show; i++)
				ptrs3[i] = liked[i];
			static sp_track batch3[8];
			int named = sp_track_metadata_batch(sp_host, l5.access_token,
			                                    ptrs3, show, batch3,
			                                    err, sizeof(err));
			say("   %d liked, showing %d\n", n, named);
			for (int i = 0; i < named; i++)
				say("     %s - %s\n", batch3[i].artist, batch3[i].name);
		}
	}

	probe_details(sp_host, l5.access_token, info.country);
	probe_client_calls(sp_host, l5.access_token, info.country);
	probe_home(sp_host, l5.access_token, info.username);
	probe_liked(sp_host, l5.access_token, info.username);

	/* 17. Does a playlist hold its history's last track under the same
	 * URI? (The home tab opens recent playlists there.) */
	say("\n17. Resume track vs playlist items\n");
	{
		http_session *s17 = http_session_open(sp_host);
		static char rec[8][SP_URI_MAX], last[8][SP_URI_MAX];
		int nr = s17 ? sp_recent_playlists_s(s17, l5.access_token,
		                                     info.username, rec, last, 8,
		                                     err, sizeof(err)) : -1;
		for (int i = 0; i < nr && i < 4; i++) {
			if (!last[i][0])
				continue;
			static sp_playlist_info pl;
			int found = -1, read = 0;
			for (int from = 0; from < 200 && found < 0; from += SP_PLAYLIST_PAGE) {
				if (sp_playlist_s(s17, l5.access_token, rec[i], from,
				                  SP_PLAYLIST_PAGE, &pl, err, sizeof(err)) != 0 ||
				    pl.items == 0)
					break;
				for (int k = 0; k < pl.track_count && found < 0; k++)
					if (strcmp(pl.track_uris[k], last[i]) == 0)
						found = read + k;
				read += pl.track_count;
			}
			static sp_track t;
			const char *one[1] = { last[i] };
			int named = sp_track_metadata_batch_s(s17, l5.access_token, one, 1,
			                                      &t, err, sizeof(err));
			say("   %s last %s (%s) -> %s %d of %d read\n", rec[i], last[i],
			    named > 0 ? t.name : "?", found >= 0 ? "found at" : "NOT found",
			    found, read);
			/* Not by URI: by name, in the first page (relinked?). */
			if (found < 0 && named > 0 &&
			    sp_playlist_s(s17, l5.access_token, rec[i], 0,
			                  SP_PLAYLIST_PAGE, &pl, err, sizeof(err)) == 0) {
				static const char *uris[SP_PLAYLIST_PAGE];
				static sp_track names[SP_PLAYLIST_PAGE];
				for (int k = 0; k < pl.track_count; k++)
					uris[k] = pl.track_uris[k];
				int got = sp_track_metadata_batch_s(s17, l5.access_token, uris,
				                                    pl.track_count, names, err,
				                                    sizeof(err));
				for (int k = 0; k < got; k++)
					if (strcmp(names[k].name, t.name) == 0)
						say("     same name at %d: %s (artist %s / %s)\n", k,
						    names[k].uri, names[k].artist, t.artist);
			}
		}
		if (s17)
			http_session_close(s17);
	}

hold:
	say("\nSTART exits.\n");
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
		frame();
	}
	if (logf)
		fclose(logf);
	tls_global_exit();
	psExit();
	socExit();
	gfxExit();
	return 0;
}
