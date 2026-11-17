/* Typed search results: what spclient's searchview answers, measured on the
 * console before any of it is relied on.
 *
 *   S1  GET /searchview/km/v4/search/<q> in three variants (no parameters;
 *       full parameters; without username and country), each with the
 *       space sent as %20 and as '+'. Per reply: status, Retry-After, bytes,
 *       time, and the shape of the JSON -- keys, arrays and their lengths,
 *       totals, a "top hit", how images and non-ASCII names arrive.
 *   S2  The five queries back to back with the best variant of S1, to see
 *       whether a 429 shows up.
 *   S3  Today's path for the same queries: context-resolve plus the
 *       metadata batch, for the times to compare against.
 *   S4  The first 200 body of each query is kept as probe_search_<n>.json,
 *       the raw material for a test fixture.
 *
 * The screen gets the summary; the log gets the full JSON outline. Neither
 * shows the token, the username or a URL that carries it (logged with
 * username=<redacted>). The saved bodies have any occurrence of the
 * username overwritten, but read them before sharing anyway.
 */
#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ap3ds.h"
#include "cfg3ds.h"
#include "http3ds.h"
#include "login5.h"
#include "pair3ds.h"
#include "spclient.h"
#include "tls3ds.h"

#define SOC_BUFFER      0x100000

/* Large enough to measure a reply over http3ds's default 512 KB cap. */
#define REPLY_LIMIT     (2 * 1024 * 1024)
#define DEFAULT_CAP     (512 * 1024)
#define SEARCH_LIMIT    24
#define PATH_MAX_LEN    1024
/* How deep the outline goes, and how much of a string it shows. */
#define OUTLINE_DEPTH   6
#define STR_SHOWN       56
#define ARRAYS_MAX      64
#define HOSTS_MAX       8

static const char *const QUERIES[] = {
	"the killers",
	"rosal\xC3\xAD" "a",
	"lofi",
	"ok computer",
	"\xE5\xAE\x87\xE5\xA4\x9A\xE7\x94\xB0",
};
#define NQUERIES ((int)(sizeof(QUERIES) / sizeof(QUERIES[0])))

typedef enum { V_BARE, V_FULL, V_ANON, V_COUNT } variant;
static const char *const VARIANT_NAME[V_COUNT] = { "bare", "full", "anon" };

typedef struct {
	char host[128];
	char token[1024];          /* login5 access token: never printed */
	char username[128];        /* never printed */
	char country[8];
	char locale[8];
} net_ctx;

static FILE *logf;
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
	if (logf) {
		fputs(line, logf);
		fflush(logf);
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
	logf = fopen(p, "w");
	if (!logf)
		logf = fopen("sdmc:/probe_search.log", "w");
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

/* ------------------------------------------------------------ requests */

/* Percent-encodes `in`, keeping the unreserved set; a space becomes '+' or
 * "%20". False when it does not fit. */
static bool encode(const char *in, bool plus, char *out, size_t outlen)
{
	static const char HEX[] = "0123456789ABCDEF";
	size_t n = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
		unsigned char c = *p;
		if (c == ' ' && plus) {
			if (n + 1 >= outlen)
				return false;
			out[n++] = '+';
		} else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		           (c >= '0' && c <= '9') || c == '-' || c == '_' ||
		           c == '.' || c == '~') {
			if (n + 1 >= outlen)
				return false;
			out[n++] = (char)c;
		} else {
			if (n + 3 >= outlen)
				return false;
			out[n++] = '%';
			out[n++] = HEX[c >> 4];
			out[n++] = HEX[c & 15];
		}
	}
	out[n] = '\0';
	return true;
}

/* The searchview path for `q`. With `redact`, the username is replaced by a
 * placeholder: that is the form that may be logged. */
static bool searchview_path(const char *q, variant v, bool plus, bool redact,
                            char *out, size_t outlen)
{
	char eq[384], euser[384];
	if (!encode(q, plus, eq, sizeof(eq)))
		return false;
	int n;
	switch (v) {
	case V_BARE:
		n = snprintf(out, outlen, "/searchview/km/v4/search/%s", eq);
		break;
	case V_FULL:
		if (redact)
			snprintf(euser, sizeof(euser), "<redacted>");
		else if (!encode(net.username, false, euser, sizeof(euser)))
			return false;
		n = snprintf(out, outlen,
		             "/searchview/km/v4/search/%s?entityVersion=2&limit=10"
		             "&imageSize=large&catalogue=&country=%s&locale=%s"
		             "&username=%s", eq, net.country, net.locale, euser);
		break;
	default:
		n = snprintf(out, outlen,
		             "/searchview/km/v4/search/%s?entityVersion=2&limit=10"
		             "&imageSize=large&catalogue=&locale=%s", eq, net.locale);
		break;
	}
	return n > 0 && (size_t)n < outlen;
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

/* --------------------------------------------------------------- one reply */

typedef struct {
	int    status;
	int    retry_after;
	size_t bytes;
	double ms;
	int    kinds;           /* how many of the four kinds came */
	bool   parsed;
	char  *body;            /* kept for S4; NULL otherwise */
} reply_info;

/* Asks `q` in variant `v` and writes up what came back. The body is kept in
 * `out->body` when `keep` and the status is 200. */
static void ask(const char *q, int qi, variant v, bool plus, bool keep,
                reply_info *out)
{
	memset(out, 0, sizeof(*out));
	char path[PATH_MAX_LEN], shown[PATH_MAX_LEN];
	if (!searchview_path(q, v, plus, false, path, sizeof(path)) ||
	    !searchview_path(q, v, plus, true, shown, sizeof(shown))) {
		say("   q%d %s%s: path too long\n", qi + 1, VARIANT_NAME[v],
		    plus ? "+" : "%");
		return;
	}
	note("\nGET %s\n", shown);

	http_reply r;
	double t0 = now_ms();
	int rc = http_request(net.host, path, "GET", net.token, NULL,
	                      "application/json", NULL, 0, REPLY_LIMIT, &r);
	out->ms = now_ms() - t0;
	memset(path, 0, sizeof(path));
	if (rc != 0) {
		say("   q%d %s%s: %s (%.0f ms)\n", qi + 1, VARIANT_NAME[v],
		    plus ? "+" : "%", r.err, out->ms);
		return;
	}
	out->status = r.status;
	out->retry_after = r.retry_after;
	out->bytes = r.len;

	outline_stats *st = calloc(1, sizeof(outline_stats));
	bool present[4] = { false, false, false, false };
	if (r.status == 200 && st && r.body) {
		const char *e = r.body + r.len;
		const char *p = ws(r.body, e);
		out->parsed = p < e && *p == '{' && skip_value(p, e) != NULL;
		note("outline:\n");
		walk(st, r.body, e, 0, "");
		out->kinds = kinds_present(st, present);
	}

	say("   q%d %s%s: HTTP %d, %u B, %.0f ms", qi + 1, VARIANT_NAME[v],
	    plus ? "+" : "%", r.status, (unsigned)r.len, out->ms);
	if (r.retry_after)
		say(", Retry-After %d", r.retry_after);
	if (r.len > DEFAULT_CAP)
		say(", OVER the %u KB cap", DEFAULT_CAP / 1024);
	say("\n");

	if (r.status != 200) {
		/* Error bodies say why; they carry nothing personal. */
		note("body: %.*s\n", (int)(r.len < 300 ? r.len : 300),
		     r.body ? r.body : "");
	} else if (st && r.body) {
		say("      kinds:");
		for (int k = 0; k < 4; k++)
			say(" %s%s", KINDS[k], present[k] ? "+" : "-");
		say("%s\n", out->parsed ? "" : "  (JSON did NOT parse)");

		note("arrays:\n");
		int sum = 0, most = 0;
		for (int i = 0; i < st->narrays; i++) {
			note("   %s = %d\n", st->arrays[i].path, st->arrays[i].len);
			if (!strchr(st->arrays[i].path, '[')) {
				sum += st->arrays[i].len;
				if (st->arrays[i].len > most)
					most = st->arrays[i].len;
			}
		}
		say("      top-level arrays: largest %d, sum %d\n", most, sum);
		if (st->ntop) {
			say("      top hit?");
			for (int i = 0; i < st->ntop; i++)
				say(" %s", st->top_keys[i]);
			say("\n");
		} else {
			say("      no key like top/best\n");
		}

		image_stats im;
		scan_images(r.body, r.len, &im);
		say("      images: i.scdn.co %d, spotify:image: %d", im.scdn,
		    im.spotify_image);
		for (int i = 0; i < im.nhosts; i++)
			say(", %s %d", im.host[i], im.host_count[i]);
		say("\n");

		text_stats t;
		scan_text(r.body, r.len, &t);
		say("      text: raw UTF-8 bytes %d, \\u %d (beyond Latin-1 %d, "
		    "surrogates %d)\n", t.raw_high, t.escapes, t.beyond_latin1,
		    t.surrogates);

		if (keep) {
			out->body = r.body;
			r.body = NULL;
		}
	}
	free(st);
	http_reply_free(&r);
}

/* S4: the body, with the username blotted out, as probe_search_<n>.json. */
static void save_body(int qi, const char *body, size_t len)
{
	char *copy = malloc(len);
	if (!copy) {
		say("   q%d: out of memory to save\n", qi + 1);
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
	char name[48], path[320];
	snprintf(name, sizeof(name), "probe_search_%d.json", qi + 1);
	data_path(name, path, sizeof(path));
	FILE *f = fopen(path, "wb");
	bool ok = f && fwrite(copy, 1, len, f) == len;
	if (f)
		fclose(f);
	free(copy);
	say("   saved %s (%u B)%s%s\n", name, (unsigned)len,
	    ok ? "" : " FAILED", blotted ? ", username blotted" : "");
}

/* ------------------------------------------------------------------ steps */

/* S1: every variant on the first query. Returns the best variant (most
 * kinds; on a tie, the one sending the least about the user), or -1 when
 * none answered 200. */
static int step_variants(bool *plus_out)
{
	say("S1 searchview variants, \"%s\"\n", QUERIES[0]);
	/* Least to most identifying. */
	static const variant ORDER[V_COUNT] = { V_BARE, V_ANON, V_FULL };
	int best = -1, best_kinds = -1;
	bool best_plus = false;
	for (int i = 0; i < V_COUNT && aptMainLoop(); i++) {
		if (ORDER[i] == V_FULL && !net.country[0])
			note("(no country cached: the full variant goes without)\n");
		for (int enc = 0; enc < 2; enc++) {
			reply_info ri;
			ask(QUERIES[0], 0, ORDER[i], enc == 1, false, &ri);
			if (ri.status == 200 && ri.kinds > best_kinds) {
				best = ORDER[i];
				best_kinds = ri.kinds;
				best_plus = enc == 1;
			}
			frame();
		}
	}
	if (best < 0)
		say("   VERDICT: no variant answered 200.\n\n");
	else
		say("   VERDICT: best is %s with %s for the space (%d kinds).\n\n",
		    VARIANT_NAME[best], best_plus ? "'+'" : "%20", best_kinds);
	*plus_out = best_plus;
	return best;
}

/* S2 and S4: the five queries back to back. */
static void step_pace(variant v, bool plus)
{
	say("S2 five queries back to back (%s, %s)\n", VARIANT_NAME[v],
	    plus ? "'+'" : "%20");
	double start = now_ms(), sum = 0, worst = 0;
	int ok = 0, limited = 0;
	for (int q = 0; q < NQUERIES && aptMainLoop(); q++) {
		reply_info ri;
		ask(QUERIES[q], q, v, plus, true, &ri);
		if (ri.status == 200) {
			ok++;
			sum += ri.ms;
			if (ri.ms > worst)
				worst = ri.ms;
		}
		if (ri.status == 429)
			limited++;
		if (ri.body) {
			save_body(q, ri.body, ri.bytes);
			free(ri.body);
		}
		frame();
	}
	say("   VERDICT: %d of %d ok, %d with 429; average %.0f ms, worst "
	    "%.0f ms, %.1f s in all.\n\n", ok, NQUERIES, limited,
	    ok ? sum / ok : 0.0, worst, (now_ms() - start) / 1000.0);
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

/* The language the app runs in, as searchview's locale. */
static void pick_locale(void)
{
	char code[8];
	if (cfg_get("language", code, sizeof(code)) && strcmp(code, "es") == 0)
		snprintf(net.locale, sizeof(net.locale), "es");
	else
		snprintf(net.locale, sizeof(net.locale), "en");
}

/* The country the app cached from /v1/me: the third field of "profile". */
static void pick_country(void)
{
	char value[192];
	net.country[0] = '\0';
	if (!cfg_get("profile", value, sizeof(value)))
		return;
	char *s = strchr(value, '\x1f');
	s = s ? strchr(s + 1, '\x1f') : NULL;
	if (s)
		snprintf(net.country, sizeof(net.country), "%.2s", s + 1);
}

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
	pick_locale();
	pick_country();
	say("   ok; host %s, locale %s, country %s\n\n", net.host, net.locale,
	    net.country[0] ? net.country : "(none cached)");
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

	say("3DS Spotify -- search probes\n");
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
		bool plus = false;
		int best = step_variants(&plus);
		if (best >= 0)
			step_pace((variant)best, plus);
		else
			say("S2 and S4 skipped: nothing to pace.\n\n");
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
	if (logf)
		fclose(logf);
	tls_global_exit();
	if (ps)
		psExit();
	socExit();
	gfxExit();
	return 0;
}
