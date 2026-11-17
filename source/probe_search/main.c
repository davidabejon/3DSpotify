/* Typed search results: what Spotify's search answers, measured on the
 * console before any of it is relied on.
 *
 * The first round asked spclient's searchview and got HTTP 400 for every
 * variant. This round asks pathfinder (api-partner.spotify.com), the
 * GraphQL service the mobile web player searches with, with the login5
 * token the app already holds:
 *
 *   H1  The persisted-query hash, found as the app would find it: the
 *       open.spotify.com page names the mobile player's bundle, the bundle
 *       (gzip, inflated here) declares the find* operations and their hash.
 *       Bytes and time of each step, and the hash against the one seen on
 *       2026-11-17.
 *   P1  findTopResults for the first query with three sets of headers
 *       (Bearer only; + App-Platform and spotify-app-version; + a browser
 *       User-Agent, Origin and Referer). The first that answers is used on.
 *   P2  For each query, findTopResults and then findArtists, findAlbums,
 *       findPlaylists and findTracks, back to back: shape, counts, totals,
 *       top results, images, non-ASCII names, and whether a 429 shows up.
 *   P3  A made-up hash: what a stale one looks like (412? an "errors" body?).
 *   P4  findArtists with limit 50: how large a page may be.
 *   S3  Today's path for the same queries: context-resolve plus the
 *       metadata batch, for the times to compare against.
 *
 * Each findTopResults body is kept as probe_search_<n>.json, and the first
 * query's per-type bodies as probe_search_1_<type>.json: the raw material
 * for a test fixture. The screen gets the summary; the log gets the full
 * JSON outline. Neither shows the token or the username; the saved bodies
 * have any occurrence of the username overwritten, but read them before
 * sharing anyway.
 */
#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/base64.h"

#include "ap3ds.h"
#include "cfg3ds.h"
#include "http3ds.h"
#include "json3ds.h"
#include "login5.h"
#include "pair3ds.h"
#include "spclient.h"
#include "tls3ds.h"

/* Only the inflater is wanted, for the gzip bundle. */
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "../../include/stb_image.h"

#define SOC_BUFFER      0x100000

/* Large enough to measure a reply over http3ds's default 512 KB cap. */
#define REPLY_LIMIT     (2 * 1024 * 1024)
#define DEFAULT_CAP     (512 * 1024)
#define SEARCH_LIMIT    24
#define PAGE_LIMIT      10
#define BODY_MAX_LEN    2048
/* How deep the outline goes, and how much of a string it shows. */
#define OUTLINE_DEPTH   12
#define STR_SHOWN       56
#define ARRAYS_MAX      64
#define HOSTS_MAX       8

#define PF_HOST         "api-partner.spotify.com"
#define PF_PATH         "/pathfinder/v2/query"
#define WEB_PAGE        "https://open.spotify.com/"
#define BUNDLE_MARK     "/cdn/build/mobile-web-player/mobile-web-player."
/* What the bundle said on 2026-11-17: used when H1 cannot find its own. */
#define KNOWN_HASH      "903df2a65d8121e27d73a2be03c01e88ebe6021bb6d4eb82a389e35d87e51d27"
#define KNOWN_VERSION   "1.3.11.1554.gf4d975765aa5"
#define BOGUS_HASH      "0000000000000000000000000000000000000000000000000000000000000000"
#define BROWSER_UA      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) " \
                        "AppleWebKit/537.36 (KHTML, like Gecko) " \
                        "Chrome/130.0.0.0 Safari/537.36"

static const char *const QUERIES[] = {
	"the killers",
	"rosal\xC3\xAD" "a",
	"lofi",
	"ok computer",
	"\xE5\xAE\x87\xE5\xA4\x9A\xE7\x94\xB0",
};
#define NQUERIES ((int)(sizeof(QUERIES) / sizeof(QUERIES[0])))

/* The operations, in the order P2 asks them; the first is the overview. */
static const char *const OPS[] = {
	"findTopResults", "findArtists", "findAlbums", "findPlaylists",
	"findTracks",
};
static const char *const OP_FILE[] = {
	NULL, "artists", "albums", "playlists", "tracks",
};
#define NOPS ((int)(sizeof(OPS) / sizeof(OPS[0])))

typedef enum { H_BEARER, H_PLATFORM, H_BROWSER, H_COUNT } header_set;
static const char *const HEADER_NAME[H_COUNT] = {
	"bearer only", "+platform", "+browser",
};

typedef struct {
	char host[128];
	char token[1024];          /* login5 access token: never printed */
	char username[128];        /* never printed */
	char hash[72];
	char version[64];
} net_ctx;

static FILE *log_file;
static net_ctx net;

/* --------------------------------------------------------------- output */

static void emit(bool screen, const char *fmt, va_list ap)
{
	char line[640];
	vsnprintf(line, sizeof(line), fmt, ap);
	if (screen) {
		fputs(line, stdout);
		fputs(line, stderr);
	}
	if (log_file) {
		fputs(line, log_file);
		fflush(log_file);
	}
}

/* Screen and log. */
static void say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	emit(true, fmt, ap);
	va_end(ap);
}

/* Log only: the outline is far longer than the screen. */
static void note(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	emit(false, fmt, ap);
	va_end(ap);
}

static void data_path(const char *name, char *out, size_t outlen)
{
	char dir[256];
	cfg_dir(dir, sizeof(dir));
	snprintf(out, outlen, "%s%s", dir[0] ? dir : "sdmc:/", name);
}

static void open_log(void)
{
	char p[320];
	data_path("probe_search.log", p, sizeof(p));
	log_file = fopen(p, "w");
	if (!log_file)
		log_file = fopen("sdmc:/probe_search.log", "w");
}

static void frame(void)
{
	gfxFlushBuffers();
	gfxSwapBuffers();
	gspWaitForVBlank();
}

static double now_ms(void)
{
	return (double)svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000.0);
}

/* ----------------------------------------------------- JSON, generically */

/* Nothing here trusts the reply: every step is bounded by `e`, and a value
 * that does not parse ends the walk instead of guessing. */

static const char *ws(const char *p, const char *e)
{
	while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t'))
		p++;
	return p;
}

/* `p` on the opening quote; returns just past the closing one, or NULL. */
static const char *skip_string(const char *p, const char *e)
{
	for (p++; p < e; p++) {
		if (*p == '\\')
			p++;
		else if (*p == '"')
			return p + 1;
	}
	return NULL;
}

static const char *skip_value(const char *p, const char *e)
{
	p = ws(p, e);
	if (p >= e)
		return NULL;
	if (*p == '"')
		return skip_string(p, e);
	if (*p == '{' || *p == '[') {
		int depth = 0;
		while (p < e) {
			if (*p == '"') {
				p = skip_string(p, e);
				if (!p)
					return NULL;
				continue;
			}
			if (*p == '{' || *p == '[')
				depth++;
			else if (*p == '}' || *p == ']') {
				if (--depth == 0)
					return p + 1;
			}
			p++;
		}
		return NULL;
	}
	while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
	       *p != '\n' && *p != '\r' && *p != '\t')
		p++;
	return p;
}

/* Calls `fn` on each element of the array or member of the object at `p`;
 * `key` is NULL for array elements. Returns how many, or -1 when it does not
 * parse. */
typedef void (*member_fn)(void *ctx, const char *key, size_t keylen,
                          const char *v, const char *vend);

static int each(const char *p, const char *e, member_fn fn, void *ctx)
{
	p = ws(p, e);
	if (p >= e || (*p != '{' && *p != '['))
		return -1;
	bool obj = *p == '{';
	char close = obj ? '}' : ']';
	int count = 0;
	p = ws(p + 1, e);
	if (p < e && *p == close)
		return 0;
	while (p < e) {
		const char *key = NULL;
		size_t keylen = 0;
		if (obj) {
			if (*p != '"')
				return -1;
			const char *kend = skip_string(p, e);
			if (!kend)
				return -1;
			key = p + 1;
			keylen = (size_t)(kend - p - 2);
			p = ws(kend, e);
			if (p >= e || *p != ':')
				return -1;
			p = ws(p + 1, e);
		}
		const char *vend = skip_value(p, e);
		if (!vend)
			return -1;
		if (fn)
			fn(ctx, key, keylen, p, vend);
		count++;
		p = ws(vend, e);
		if (p < e && *p == ',') {
			p = ws(p + 1, e);
			continue;
		}
		if (p < e && *p == close)
			return count;
		return -1;
	}
	return -1;
}

/* What the walk over one reply collects. */
typedef struct {
	char path[96];
	int  len;
} array_seen;

typedef struct {
	array_seen arrays[ARRAYS_MAX];
	int        narrays;
	char       top_keys[8][96];   /* keys that look like a best result */
	int        ntop;
} outline_stats;

typedef struct {
	outline_stats *st;
	int  depth;
	char path[96];
} walk_ctx;

static void walk(outline_stats *st, const char *v, const char *e, int depth,
                 const char *path);

/* A short, printable view of a scalar. Strings are cut, and one that holds
 * the username is not shown at all. */
static void scalar_view(const char *v, const char *e, char *out, size_t outlen)
{
	size_t n = (size_t)(e - v);
	if (*v == '"') {
		char tmp[STR_SHOWN + 8];
		size_t body = n >= 2 ? n - 2 : 0;
		size_t keep = body < STR_SHOWN ? body : STR_SHOWN;
		memcpy(tmp, v + 1, keep);
		tmp[keep] = '\0';
		if (net.username[0] && memmem(v, n, net.username,
		                              strlen(net.username)))
			snprintf(out, outlen, "\"<redacted>\"");
		else
			snprintf(out, outlen, "\"%s%s\" (%u)", tmp,
			         body > keep ? "..." : "", (unsigned)body);
		return;
	}
	if (n > 24)
		n = 24;
	snprintf(out, outlen, "%.*s", (int)n, v);
}

static bool looks_like_top(const char *key, size_t keylen)
{
	char low[64];
	size_t n = keylen < sizeof(low) - 1 ? keylen : sizeof(low) - 1;
	for (size_t i = 0; i < n; i++) {
		char c = key[i];
		low[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
	}
	low[n] = '\0';
	return strstr(low, "top") || strstr(low, "best");
}

static void walk_member(void *p, const char *key, size_t keylen,
                        const char *v, const char *vend)
{
	walk_ctx *w = p;
	/* A path too deep to fit is cut: it only labels the outline. */
	char child[96];
	size_t at = strlen(w->path);
	memcpy(child, w->path, at + 1);
	if (at + 1 < sizeof(child)) {
		child[at++] = '.';
		size_t room = sizeof(child) - 1 - at;
		size_t n = keylen < room ? keylen : room;
		memcpy(child + at, key, n);
		child[at + n] = '\0';
	}
	if (looks_like_top(key, keylen) && w->st->ntop < 8)
		snprintf(w->st->top_keys[w->st->ntop++], 96, "%s", child);

	note("%*s%.*s: ", w->depth * 2, "", (int)keylen, key);
	walk(w->st, v, vend, w->depth, child);
}

/* Prints the value at `v` and, for containers, what is inside: an object's
 * every member, an array's length and its first element only. */
static void walk(outline_stats *st, const char *v, const char *e, int depth,
                 const char *path)
{
	v = ws(v, e);
	if (*v == '{') {
		int n = each(v, e, NULL, NULL);
		note("{%d}\n", n);
		if (n <= 0 || depth >= OUTLINE_DEPTH)
			return;
		walk_ctx w = { st, depth + 1, "" };
		snprintf(w.path, sizeof(w.path), "%s", path);
		each(v, e, walk_member, &w);
		return;
	}
	if (*v == '[') {
		int n = each(v, e, NULL, NULL);
		note("[%d]\n", n);
		if (st->narrays < ARRAYS_MAX) {
			array_seen *a = &st->arrays[st->narrays++];
			snprintf(a->path, sizeof(a->path), "%s", path);
			a->len = n;
		}
		if (n <= 0 || depth >= OUTLINE_DEPTH)
			return;
		const char *first = ws(v + 1, e);
		const char *fend = skip_value(first, e);
		if (!fend)
			return;
		char child[96];
		snprintf(child, sizeof(child), "%s[0]", path);
		note("%*s[0]: ", (depth + 1) * 2, "");
		walk(st, first, fend, depth + 1, child);
		return;
	}
	char view[STR_SHOWN + 32];
	scalar_view(v, e, view, sizeof(view));
	note("%s\n", view);
}

/* How names arrive: raw UTF-8 bytes, \u escapes, and whether any escape
 * falls outside Latin-1 (the range json_str decodes today). */
typedef struct {
	int raw_high;        /* bytes >= 0x80 */
	int escapes;
	int beyond_latin1;
	int surrogates;
} text_stats;

static void scan_text(const char *b, size_t len, text_stats *t)
{
	memset(t, 0, sizeof(*t));
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)b[i];
		if (c >= 0x80) {
			t->raw_high++;
			continue;
		}
		if (c != '\\' || i + 1 >= len)
			continue;
		if (b[i + 1] != 'u') {
			i++;
			continue;
		}
		if (i + 5 >= len)
			break;
		unsigned cp = 0;
		bool ok = true;
		for (int k = 2; k < 6; k++) {
			char h = b[i + k];
			cp <<= 4;
			if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
			else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
			else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
			else ok = false;
		}
		if (!ok)
			continue;
		t->escapes++;
		if (cp > 0xFF)
			t->beyond_latin1++;
		if (cp >= 0xD800 && cp <= 0xDFFF)
			t->surrogates++;
		i += 5;
	}
}

/* Where images point: i.scdn.co links, spotify:image: ids, and every other
 * https host by name. */
typedef struct {
	int  scdn;
	int  spotify_image;
	char host[HOSTS_MAX][64];
	int  host_count[HOSTS_MAX];
	int  nhosts;
} image_stats;

static void scan_images(const char *b, size_t len, image_stats *im)
{
	memset(im, 0, sizeof(*im));
	const char *e = b + len;
	for (const char *p = b; p < e;) {
		const char *s = memmem(p, (size_t)(e - p), "spotify:image:", 14);
		if (!s)
			break;
		im->spotify_image++;
		p = s + 14;
	}
	for (const char *p = b; p < e;) {
		const char *s = memmem(p, (size_t)(e - p), "https://", 8);
		if (!s)
			break;
		const char *h = s + 8;
		const char *he = h;
		while (he < e && *he != '/' && *he != '"' && *he != '\\' &&
		       he - h < 63)
			he++;
		size_t hl = (size_t)(he - h);
		p = he;
		if (hl == 9 && memcmp(h, "i.scdn.co", 9) == 0) {
			im->scdn++;
			continue;
		}
		int i = 0;
		for (; i < im->nhosts; i++)
			if (strlen(im->host[i]) == hl && memcmp(im->host[i], h, hl) == 0)
				break;
		if (i == im->nhosts) {
			if (im->nhosts == HOSTS_MAX)
				continue;
			snprintf(im->host[i], sizeof(im->host[i]), "%.*s", (int)hl, h);
			im->nhosts++;
		}
		im->host_count[i]++;
	}
}

/* Which kinds a reply carries, by the arrays it holds: one with a kind's
 * name in its path and something in it. */
static const char *const KINDS[] = { "track", "artist", "album", "playlist" };

static int kinds_present(const outline_stats *st, bool present[4])
{
	int n = 0;
	for (int k = 0; k < 4; k++) {
		present[k] = false;
		for (int i = 0; i < st->narrays && !present[k]; i++) {
			/* Not inside an element: "artists" in every track hit says
			 * nothing about an artists section. */
			const char *path = st->arrays[i].path;
			if (st->arrays[i].len > 0 && strstr(path, KINDS[k]) &&
			    !strchr(path, '['))
				present[k] = true;
		}
		n += present[k];
	}
	return n;
}


/* ------------------------------------------------------------ one reply */

typedef struct {
	int    status;
	int    retry_after;
	size_t bytes;
	double ms;
	int    kinds;           /* how many of the four kinds came */
	bool   ok;              /* 200 with "data" and no "errors" */
	char  *body;            /* kept when asked; NULL otherwise */
} reply_info;

/* The __typename of the first few top results, in order. */
static void say_top_types(const char *b, size_t len)
{
	const char *e = b + len;
	const char *p = memmem(b, len, "\"topResultsV2\"", 14);
	if (!p)
		return;
	say("      top results:");
	int shown = 0;
	static const char MARK[] = "\"data\":{\"__typename\":\"";
	while (shown < 5) {
		const char *s = memmem(p, (size_t)(e - p), MARK, sizeof(MARK) - 1);
		if (!s)
			break;
		s += sizeof(MARK) - 1;
		const char *q = memchr(s, '"', (size_t)(e - s));
		if (!q || q - s > 40)
			break;
		say(" %.*s", (int)(q - s), s);
		shown++;
		p = q;
	}
	say("%s\n", shown ? "" : " none");
}

/* Writes up one reply: the summary on screen, the outline in the log. The
 * body moves into `out->body` when `keep` and it answered. */
static void analyze(const char *label, http_reply *r, double ms, bool keep,
                    reply_info *out)
{
	out->status = r->status;
	out->retry_after = r->retry_after;
	out->bytes = r->len;
	out->ms = ms;

	bool errors = r->body && memmem(r->body, r->len, "\"errors\"", 8);
	bool data = r->body && memmem(r->body, r->len, "\"data\"", 6);
	out->ok = r->status == 200 && data && !errors;

	say("   %s: HTTP %d, %u B, %.0f ms", label, r->status, (unsigned)r->len,
	    ms);
	if (r->retry_after)
		say(", Retry-After %d", r->retry_after);
	if (r->len > DEFAULT_CAP)
		say(", OVER the %u KB cap", DEFAULT_CAP / 1024);
	say("\n");

	if (!out->ok) {
		/* Error bodies say why; they carry nothing personal. */
		int shown = (int)(r->len < 160 ? r->len : 160);
		say("      %.*s\n", shown, r->body ? r->body : "");
		note("body: %.*s\n", (int)(r->len < 1000 ? r->len : 1000),
		     r->body ? r->body : "");
		return;
	}

	outline_stats *st = calloc(1, sizeof(outline_stats));
	if (!st)
		return;
	const char *e = r->body + r->len;
	bool parsed = skip_value(r->body, e) != NULL;
	note("outline:\n");
	walk(st, r->body, e, 0, "");
	bool present[4];
	out->kinds = kinds_present(st, present);

	say("      kinds:");
	for (int k = 0; k < 4; k++)
		say(" %s%s", KINDS[k], present[k] ? "+" : "-");
	say("%s\n", parsed ? "" : "  (JSON did NOT parse)");
	note("arrays:\n");
	for (int i = 0; i < st->narrays; i++) {
		note("   %s = %d\n", st->arrays[i].path, st->arrays[i].len);
		if (!strchr(st->arrays[i].path, '[') && st->arrays[i].len > 0)
			say("      %s = %d\n", st->arrays[i].path,
			    st->arrays[i].len);
	}
	say_top_types(r->body, r->len);

	image_stats im;
	scan_images(r->body, r->len, &im);
	say("      images: i.scdn.co %d, spotify:image: %d", im.scdn,
	    im.spotify_image);
	for (int i = 0; i < im.nhosts; i++)
		say(", %s %d", im.host[i], im.host_count[i]);
	say("\n");

	text_stats t;
	scan_text(r->body, r->len, &t);
	say("      text: raw UTF-8 bytes %d, \\u %d (beyond Latin-1 %d, "
	    "surrogates %d)\n", t.raw_high, t.escapes, t.beyond_latin1,
	    t.surrogates);
	free(st);

	if (keep) {
		out->body = r->body;
		r->body = NULL;
	}
}

/* ------------------------------------------------------------ pathfinder */

/* `in` as the inside of a JSON string. False when it does not fit. */
static bool json_escape(const char *in, char *out, size_t outlen)
{
	size_t n = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
		char esc[8];
		size_t k;
		if (*p == '"' || *p == '\\') {
			esc[0] = '\\';
			esc[1] = (char)*p;
			k = 2;
		} else if (*p < 0x20) {
			k = (size_t)snprintf(esc, sizeof(esc), "\\u%04x", *p);
		} else {
			esc[0] = (char)*p;
			k = 1;
		}
		if (n + k >= outlen)
			return false;
		memcpy(out + n, esc, k);
		n += k;
	}
	out[n] = '\0';
	return true;
}

/* The header lines of each set; the Bearer goes on its own. */
static void header_lines(header_set h, char *out, size_t outlen)
{
	out[0] = '\0';
	if (h >= H_PLATFORM)
		snprintf(out, outlen, "App-Platform: WebPlayer\r\n"
		         "spotify-app-version: %s\r\n", net.version);
	if (h >= H_BROWSER) {
		size_t at = strlen(out);
		snprintf(out + at, outlen - at, "User-Agent: " BROWSER_UA "\r\n"
		         "Origin: https://open.spotify.com\r\n"
		         "Referer: https://open.spotify.com/\r\n");
	}
}

/* One operation for `q`. `limit` is numberOfTopResults for the overview
 * and the page size for the rest. */
static void pf_ask(const char *op, const char *q, int limit, const char *hash,
                   header_set h, const char *label, bool keep,
                   reply_info *out)
{
	memset(out, 0, sizeof(*out));
	char eq[384], body[BODY_MAX_LEN], extra[512];
	if (!json_escape(q, eq, sizeof(eq))) {
		say("   %s: query too long\n", label);
		return;
	}
	int n;
	if (strcmp(op, "findTopResults") == 0)
		n = snprintf(body, sizeof(body),
		             "{\"variables\":{\"query\":\"%s\",\"numberOfTopResults\":%d,"
		             "\"includePreReleases\":false,"
		             "\"includeAlbumPreReleases\":false},"
		             "\"operationName\":\"%s\",\"extensions\":{\"persistedQuery\":"
		             "{\"version\":1,\"sha256Hash\":\"%s\"}}}",
		             eq, limit, op, hash);
	else
		n = snprintf(body, sizeof(body),
		             "{\"variables\":{\"query\":\"%s\",\"limit\":%d,\"offset\":0},"
		             "\"operationName\":\"%s\",\"extensions\":{\"persistedQuery\":"
		             "{\"version\":1,\"sha256Hash\":\"%s\"}}}",
		             eq, limit, op, hash);
	if (n < 0 || (size_t)n >= sizeof(body)) {
		say("   %s: body too long\n", label);
		return;
	}
	header_lines(h, extra, sizeof(extra));
	note("\nPOST %s%s %s (%s): %s\n", PF_HOST, PF_PATH, op, HEADER_NAME[h],
	     body);

	http_reply r;
	double t0 = now_ms();
	int rc = http_request_ex(PF_HOST, PF_PATH, "POST", net.token,
	                         "application/json;charset=UTF-8",
	                         "application/json", extra, body, (size_t)n,
	                         REPLY_LIMIT, &r);
	double ms = now_ms() - t0;
	if (rc != 0) {
		say("   %s: %s (%.0f ms)\n", label, r.err, ms);
		return;
	}
	analyze(label, &r, ms, keep, out);
	http_reply_free(&r);
}

/* The body, with the username blotted out, under `name` in the app folder. */
static void save_body(const char *name, const char *body, size_t len)
{
	char *copy = malloc(len);
	if (!copy) {
		say("   %s: out of memory to save\n", name);
		return;
	}
	memcpy(copy, body, len);
	size_t ul = strlen(net.username);
	int blotted = 0;
	if (ul) {
		for (char *p = copy; (p = memmem(p, len - (size_t)(p - copy),
		                                 net.username, ul)) != NULL;
		     p += ul) {
			memset(p, '*', ul);
			blotted++;
		}
	}
	char path[320];
	data_path(name, path, sizeof(path));
	FILE *f = fopen(path, "wb");
	bool ok = f && fwrite(copy, 1, len, f) == len;
	if (f)
		fclose(f);
	free(copy);
	say("      saved %s%s%s\n", name, ok ? "" : " FAILED",
	    blotted ? ", username blotted" : "");
}

/* ------------------------------------------------------------- H1: hash */

/* A gzip member inflated, or NULL. The CDN sends the bundle gzipped even
 * when not asked to; stb_image's inflater takes the raw deflate inside. */
static char *gunzip(const uint8_t *b, size_t n, size_t *outlen)
{
	if (n < 18 || b[0] != 0x1f || b[1] != 0x8b || b[2] != 8)
		return NULL;
	uint8_t flg = b[3];
	size_t p = 10;
	if (flg & 4)
		p += 2 + (size_t)(b[10] | b[11] << 8);
	for (int field = 8; field <= 16; field <<= 1) {
		if (!(flg & field))
			continue;
		while (p < n && b[p])
			p++;
		p++;
	}
	if (flg & 2)
		p += 2;
	if (p + 8 > n)
		return NULL;
	size_t isize = (size_t)b[n - 4] | (size_t)b[n - 3] << 8 |
	               (size_t)b[n - 2] << 16 | (size_t)b[n - 1] << 24;
	if (isize == 0 || isize > 16 * 1024 * 1024)
		return NULL;
	char *out = malloc(isize + 1);
	if (!out)
		return NULL;
	int got = stbi_zlib_decode_noheader_buffer(out, (int)isize,
	                                           (const char *)b + p,
	                                           (int)(n - p - 8));
	if (got != (int)isize) {
		free(out);
		return NULL;
	}
	out[isize] = '\0';
	*outlen = isize;
	return out;
}

/* The bundle URL and the client version out of the player page. */
static bool read_page(char *bundle, size_t blen)
{
	http_reply r;
	double t0 = now_ms();
	if (http_get_url(WEB_PAGE, REPLY_LIMIT, &r) != 0) {
		say("   page: %s\n", r.err);
		return false;
	}
	say("   page: HTTP %d, %u B, %.0f ms\n", r.status, (unsigned)r.len,
	    now_ms() - t0);
	bool ok = false;
	const char *m = r.body ? strstr(r.body, BUNDLE_MARK) : NULL;
	if (m) {
		const char *s = m;
		while (s > r.body && s[-1] != '"')
			s--;
		const char *e = strstr(m, ".js");
		if (e && (size_t)(e + 3 - s) < blen) {
			snprintf(bundle, blen, "%.*s", (int)(e + 3 - s), s);
			ok = strncmp(bundle, "https://", 8) == 0;
		}
	}
	if (!ok)
		say("   page: no mobile player bundle named%s\n",
		    r.body && strstr(r.body, "/web-player/web-player.")
		        ? " (the desktop one is)" : "");

	static const char CFG[] = "<script id=\"appServerConfig\" type=\"text/plain\">";
	const char *c = r.body ? strstr(r.body, CFG) : NULL;
	const char *ce = c ? strstr(c, "</script>") : NULL;
	if (c && ce) {
		c += sizeof(CFG) - 1;
		while (c < ce && (*c == ' ' || *c == '\n' || *c == '\r'))
			c++;
		size_t clen = (size_t)(ce - c);
		while (clen && (c[clen - 1] == ' ' || c[clen - 1] == '\n' ||
		                c[clen - 1] == '\r'))
			clen--;
		size_t dlen = 0;
		unsigned char *dec = malloc(clen + 1);
		if (dec && mbedtls_base64_decode(dec, clen, &dlen,
		                                 (const unsigned char *)c,
		                                 clen) == 0) {
			dec[dlen] = '\0';
			char v[64];
			if (json_str((const char *)dec, "clientVersion", v, sizeof(v)))
				snprintf(net.version, sizeof(net.version), "%s", v);
		}
		free(dec);
	}
	say("   client version: %s%s\n", net.version[0] ? net.version : "-",
	    net.version[0] ? "" : " (not found)");
	http_reply_free(&r);
	return ok;
}

/* H1. Leaves the hash in net.hash: the one found, else the known one. */
static void step_hash(void)
{
	say("H1 persisted-query hash from the mobile player\n");
	double start = now_ms();
	char bundle[256] = "";
	char found[72] = "";
	if (read_page(bundle, sizeof(bundle))) {
		const char *name = strrchr(bundle, '/');
		say("   bundle %s\n", name ? name + 1 : bundle);
		http_reply r;
		double t0 = now_ms();
		if (http_get_url(bundle, REPLY_LIMIT, &r) != 0) {
			say("   bundle: %s\n", r.err);
		} else {
			double t1 = now_ms();
			say("   bundle: HTTP %d, %u B, %.0f ms\n", r.status,
			    (unsigned)r.len, t1 - t0);
			size_t plain_len = r.len;
			char *plain = gunzip((const uint8_t *)r.body, r.len, &plain_len);
			if (plain)
				say("   gunzip: %u B in %.0f ms\n", (unsigned)plain_len,
				    now_ms() - t1);
			else if (r.len > 1 && (uint8_t)r.body[0] == 0x1f)
				say("   gunzip FAILED\n");
			const char *text = plain ? plain : r.body;
			size_t tlen = plain ? plain_len : r.len;
			int same = 0, declared = 0;
			for (int i = 0; i < NOPS; i++) {
				char mark[64];
				int ml = snprintf(mark, sizeof(mark), "\"%s\",\"query\",\"",
				                  OPS[i]);
				const char *s = text ? memmem(text, tlen, mark, (size_t)ml)
				                     : NULL;
				if (!s || s + ml + 64 > text + tlen) {
					say("   %s: NOT declared\n", OPS[i]);
					continue;
				}
				declared++;
				char h[72];
				snprintf(h, sizeof(h), "%.64s", s + ml);
				if (!found[0])
					snprintf(found, sizeof(found), "%s", h);
				if (strcmp(h, found) == 0)
					same++;
				note("   %s %s\n", OPS[i], h);
			}
			say("   %d of %d operations declared, %d with the same hash\n",
			    declared, NOPS, same);
			free(plain);
			http_reply_free(&r);
		}
	}
	if (!net.version[0])
		snprintf(net.version, sizeof(net.version), "%s", KNOWN_VERSION);
	if (found[0]) {
		say("   hash %.12s... %s the one of 2026-11-17\n", found,
		    strcmp(found, KNOWN_HASH) == 0 ? "SAME as" : "DIFFERENT from");
		snprintf(net.hash, sizeof(net.hash), "%s", found);
	} else {
		say("   no hash found: going on with the known one\n");
		snprintf(net.hash, sizeof(net.hash), "%s", KNOWN_HASH);
	}
	say("   VERDICT: %s in %.1f s.\n\n", found[0] ? "found" : "NOT found",
	    (now_ms() - start) / 1000.0);
}

/* ------------------------------------------------------------------ steps */

/* P1: the fewest headers pathfinder answers to. -1 when none. */
static int step_headers(void)
{
	say("P1 findTopResults, \"%s\", by header set\n", QUERIES[0]);
	int best = -1;
	for (int h = 0; h < H_COUNT && aptMainLoop(); h++) {
		reply_info ri;
		pf_ask(OPS[0], QUERIES[0], PAGE_LIMIT, net.hash, (header_set)h,
		       HEADER_NAME[h], false, &ri);
		if (ri.ok && best < 0)
			best = h;
		frame();
	}
	if (best < 0)
		say("   VERDICT: no header set answered.\n\n");
	else
		say("   VERDICT: \"%s\" is enough.\n\n", HEADER_NAME[best]);
	return best;
}

/* P2: every operation for every query, back to back. */
static void step_queries(header_set h)
{
	say("P2 five queries x %d operations, back to back (%s)\n", NOPS,
	    HEADER_NAME[h]);
	double start = now_ms(), sum = 0, worst = 0, top_sum = 0;
	int ok = 0, limited = 0, asked = 0, top_ok = 0;
	for (int q = 0; q < NQUERIES && aptMainLoop(); q++) {
		say("  q%d\n", q + 1);
		for (int o = 0; o < NOPS && aptMainLoop(); o++) {
			reply_info ri;
			bool keep = o == 0 || q == 0;
			pf_ask(OPS[o], QUERIES[q], PAGE_LIMIT, net.hash, h, OPS[o],
			       keep, &ri);
			asked++;
			if (ri.ok) {
				ok++;
				sum += ri.ms;
				if (ri.ms > worst)
					worst = ri.ms;
				if (o == 0) {
					top_ok++;
					top_sum += ri.ms;
				}
			}
			if (ri.status == 429)
				limited++;
			if (ri.body) {
				char name[48];
				if (o == 0)
					snprintf(name, sizeof(name), "probe_search_%d.json",
					         q + 1);
				else
					snprintf(name, sizeof(name), "probe_search_%d_%s.json",
					         q + 1, OP_FILE[o]);
				save_body(name, ri.body, ri.bytes);
				free(ri.body);
			}
			frame();
		}
	}
	say("   VERDICT: %d of %d ok, %d with 429; average %.0f ms (top "
	    "results %.0f ms), worst %.0f ms, %.1f s in all.\n\n", ok, asked,
	    limited, ok ? sum / ok : 0.0, top_ok ? top_sum / top_ok : 0.0, worst,
	    (now_ms() - start) / 1000.0);
}

/* P3: what a stale hash looks like. */
static void step_bogus(header_set h)
{
	say("P3 a made-up hash\n");
	reply_info ri;
	pf_ask(OPS[0], QUERIES[0], PAGE_LIMIT, BOGUS_HASH, h, "bogus", false,
	       &ri);
	say("   VERDICT: HTTP %d%s.\n\n", ri.status,
	    ri.ok ? " and data: the hash is NOT checked" : "");
}

/* P4: a page larger than the web player asks for. */
static void step_page(header_set h)
{
	say("P4 findArtists with limit 50\n");
	reply_info ri;
	pf_ask("findArtists", QUERIES[0], 50, net.hash, h, "limit 50", false,
	       &ri);
	say("\n");
}

/* S3: today's two requests for the same queries. */
static void step_reference(void)
{
	say("S3 today's search: context-resolve + metadata batch\n");
	static char uris[SEARCH_LIMIT][64];
	static sp_track tracks[SEARCH_LIMIT];
	const char *ptrs[SEARCH_LIMIT];
	char err[256];
	double sum = 0;
	int ok = 0;
	for (int q = 0; q < NQUERIES && aptMainLoop(); q++) {
		double t0 = now_ms();
		int n = sp_search_tracks(net.host, net.token, QUERIES[q], uris,
		                         SEARCH_LIMIT, err, sizeof(err));
		double t1 = now_ms();
		if (n <= 0) {
			say("   q%d: %s (%.0f ms)\n", q + 1, err, t1 - t0);
			continue;
		}
		for (int i = 0; i < n; i++)
			ptrs[i] = uris[i];
		int m = sp_track_metadata_batch(net.host, net.token, ptrs, n, tracks,
		                                err, sizeof(err));
		double t2 = now_ms();
		if (m < 0) {
			say("   q%d: %d uris in %.0f ms; metadata: %s\n", q + 1, n,
			    t1 - t0, err);
			continue;
		}
		say("   q%d: %d uris %.0f ms + %d named %.0f ms = %.0f ms\n", q + 1,
		    n, t1 - t0, m, t2 - t1, t2 - t0);
		ok++;
		sum += t2 - t0;
		frame();
	}
	say("   VERDICT: %d of %d ok, average %.0f ms.\n\n", ok, NQUERIES,
	    ok ? sum / ok : 0.0);
}

/* ------------------------------------------------------------------ setup */

/* Signs in as the app does: refresh token, OAuth, AP, login5. */
static bool net_setup(void)
{
	char err[256];
	static char stored[512];
	if (!cfg_load_refresh_token(stored, sizeof(stored))) {
		say("Not paired. Pair in the app first.\n");
		return false;
	}
	static spotify_tokens tok;
	say("signing in...\n");
	if (token_refresh(stored, &tok, err, sizeof(err)) != 0) {
		say("   %s\n", err);
		return false;
	}
	if (tok.refresh_token[0] && strcmp(tok.refresh_token, stored) != 0)
		cfg_save_refresh_token(tok.refresh_token, tok.scope);
	memset(stored, 0, sizeof(stored));
	char device_id[64];
	ap_device_id(device_id, sizeof(device_id));

	static ap_candidate cands[AP_MAX_CANDIDATES];
	int ncands = ap_resolve_list(cands, AP_MAX_CANDIDATES, err, sizeof(err));
	if (ncands < 1) {
		say("   %s\n", err);
		return false;
	}
	ap_info info;
	int tried = 0;
	ap_session *ap = ap_login_any(cands, ncands, tok.access_token, device_id,
	                              &info, &tried, err, sizeof(err));
	memset(&tok, 0, sizeof(tok));
	if (!ap) {
		say("   %s\n", err);
		return false;
	}
	static login5_token l5;
	int rc = login5_from_credentials(info.username, info.reusable,
	                                 info.reusable_len, device_id, &l5,
	                                 err, sizeof(err));
	ap_close(ap);
	if (rc != 0) {
		say("   login5: %s\n", err);
		return false;
	}
	snprintf(net.token, sizeof(net.token), "%s", l5.access_token);
	snprintf(net.username, sizeof(net.username), "%s", info.username);
	memset(&l5, 0, sizeof(l5));
	memset(&info, 0, sizeof(info));
	char ignored[128];
	sp_resolve(net.host, sizeof(net.host), ignored, sizeof(ignored));
	say("   ok; spclient %s\n\n", net.host);
	return true;
}

int main(int argc, char **argv)
{
	(void)argc;
	gfxInitDefault();
	consoleInit(GFX_TOP, NULL);
	const char *argv0 = (argv && argv[0]) ? argv[0] : NULL;
	cfg_set_path(argv0);
	open_log();

	say("3DS Spotify -- search probes (pathfinder)\n");
	say("built %s\n\n", BUILD_STAMP);

	/* TLS and the AP handshake draw their randomness from PS. */
	bool ps = R_SUCCEEDED(psInit());
	if (!ps) {
		say("psInit failed; no entropy source.\n");
		goto hold;
	}

	u32 *soc = memalign(0x1000, SOC_BUFFER);
	if (!soc || R_FAILED(socInit(soc, SOC_BUFFER))) {
		say("socInit failed.\n");
		goto hold;
	}
	int rc = tls_global_init();
	if (rc) {
		char reason[128];
		tls_strerror(rc, reason, sizeof(reason));
		say("tls_global_init: %s\n", reason);
		goto hold;
	}

	if (net_setup()) {
		step_hash();
		int h = step_headers();
		if (h >= 0) {
			step_queries((header_set)h);
			step_bogus((header_set)h);
			step_page((header_set)h);
		} else {
			step_bogus(H_BROWSER);
			say("P2 and P4 skipped: pathfinder did not answer.\n\n");
		}
		step_reference();
	}
	memset(&net, 0, sizeof(net));
	say("done. Log and .json in the app folder.\n");

hold:
	say("\nSTART exits.\n");
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
		frame();
	}
	if (log_file)
		fclose(log_file);
	tls_global_exit();
	if (ps)
		psExit();
	socExit();
	gfxExit();
	return 0;
}
