/* Artist detail, phase 0: what an artist's metadata carries and what the
 * monthly listeners cost, measured on the console before any of it is
 * relied on.
 *
 * Per artist (The Killers, Bad Bunny, Mozart, and up to three more read
 * from probe_artist.txt beside the .3dsx: one id, URI or share link per
 * line):
 *
 *   1  The artist entity, the request sp_artist_metadata makes: bytes, ms,
 *      whether it fits http3ds's default cap, and every top-level field of
 *      the Artist with its count and bytes.
 *   2  Biography (field 12): how many, the text's length, its first 300
 *      bytes, tags, entities and line breaks. For the first artist, the
 *      same request again with Accept-Language: es.
 *   3  album_group 5, single_group 6, compilation_group 7, appears_on 8:
 *      AlbumGroups and the Albums inside each (regional variants). For the
 *      first Album of every AlbumGroup: name, type, date (and how month and
 *      day are encoded), cover, and the order the dates come in.
 *   4  Only when some release came without a name or a date: an album
 *      metadata batch of 10, 20 and 50 URIs.
 *   5a GET /creatorabout/v0/artist-insights/<id> on spclient, on the same
 *      keep-alive session as step 1: monthlyListeners, biography.
 *   5b pathfinder queryArtistOverview, its hash found in the mobile web
 *      player's bundle as pf_hash_refresh finds the search's: monthly
 *      listeners and biography under data.artistUnion.
 *   6  The free heap before and after each step.
 *
 * V0 checks first that The Killers' id is the artist of SAMPLE_TRACK.
 *
 * The log and the screen carry no token and no username. The JSON replies
 * are kept as probe_artist_<n>_insights.json and _overview.json, and the
 * first artist's protobuf as probe_artist_1_metadata.bin, each with any
 * occurrence of the username overwritten; read them before sharing anyway.
 */
#include <3ds.h>
#include <ctype.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ap3ds.h"
#include "cfg3ds.h"
#include "http3ds.h"
#include "json3ds.h"
#include "login5.h"
#include "meta_parse.h"
#include "pair3ds.h"
#include "proto3ds.h"
#include "search_results.h"
#include "spclient.h"
#include "spotify_id.h"
#include "tls3ds.h"

/* Only the inflater is wanted, for the gzip bundle. */
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "../../include/stb_image.h"

#define SOC_BUFFER      0x100000

/* Large enough to measure a reply over http3ds's default 512 KB cap. */
#define REPLY_LIMIT     (4 * 1024 * 1024)
#define DEFAULT_CAP     (512 * 1024)
#define INFLATED_MAX    (4 * 1024 * 1024)
#define ARTISTS_MAX     6
#define RELEASES_MAX    512
#define FIELDS_MAX      40
#define BIO_SHOWN       300
#define BIO_MAX         8192

#define SAMPLE_TRACK    "spotify:track:3n3Ppam7vgaVa1iaRUc9Lp"
#define EXTMETA_PATH    "/extended-metadata/v0/extended-metadata"
#define PF_HOST         "api-partner.spotify.com"
#define PF_PATH         "/pathfinder/v2/query"
#define PLAYER_PAGE     "https://open.spotify.com/"
#define ARTIST_OP       "queryArtistOverview"
/* Seen in third-party scrapers in 2024; tried only when the bundle
 * declares none, and probably stale by now. */
#define OLD_ARTIST_HASH \
	"35648a112beb1794e39ab931365f6ae4a8d45e65396d641eeda94e4003d41497"
#define BROWSER_UA      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) " \
                        "AppleWebKit/537.36 (KHTML, like Gecko) " \
                        "Chrome/130.0.0.0 Safari/537.36"

typedef struct {
	const char *id;
	const char *why;
} artist_ref;

static const artist_ref DEFAULTS[] = {
	{ "0C0XlULifJtAgn6ZNCW2eu", "The Killers" },
	{ "4q3ewBCX7sLwd24euuV69X", "Bad Bunny: many singles" },
	{ "4NJhFmfw43RLBLjQvxDuRS", "Mozart: a huge discography" },
};
#define NDEFAULTS ((int)(sizeof(DEFAULTS) / sizeof(DEFAULTS[0])))

static const char *const GROUP_NAME[4] = {
	"album", "single", "compilation", "appears_on",
};

typedef struct {
	char host[128];
	char token[1024];          /* login5 access token: never printed */
	char username[128];        /* never printed */
} net_ctx;

/* What one artist came to, for the summary at the end. */
typedef struct {
	char   id[SP_ID_LEN + 1];
	char   why[48];
	char   name[64];
	int    status;
	size_t bytes;
	double ms;
	bool   parsed;
	int    bios;
	int    bio_len;
	int    groups[4];        /* AlbumGroups in fields 5, 6, 7, 8 */
	int    albums[4];        /* Albums inside them */
	int    firsts;           /* first Albums of groups 5-7 */
	int    named, typed, dated_y, dated_ym, dated_ymd, covered;
	int    a_status;
	double a_ms;
	long   a_listeners;      /* -1: not in the reply */
	int    a_bio;            /* biography length, 0 none */
	int    b_status;
	double b_ms;
	long   b_listeners;
	int    b_bio;
} artist_sum;

static FILE *log_file;
static net_ctx net;
static artist_sum sums[ARTISTS_MAX];
static int nartists;

/* The releases of the artist with the most, for step 4. */
static char batch_uris[RELEASES_MAX][SP_URI_MAX];
static int nbatch;
static bool need_batch;

static char artist_hash[65];
static char hash_source[48];

extern char *fake_heap_end;

/* --------------------------------------------------------------- output */

static void emit(bool screen, const char *fmt, va_list ap)
{
	char line[768];
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

/* Log only. */
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
	data_path("probe_artist.log", p, sizeof(p));
	log_file = fopen(p, "w");
	if (!log_file)
		log_file = fopen("sdmc:/probe_artist.log", "w");
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

/* What malloc can still hand out: the free blocks it holds plus what sbrk
 * has not given it yet. */
static unsigned heap_free_kb(void)
{
	struct mallinfo mi = mallinfo();
	char *top = sbrk(0);
	size_t tail = (top && fake_heap_end > top)
	    ? (size_t)(fake_heap_end - top) : 0;
	return (unsigned)((tail + (size_t)mi.fordblks) / 1024);
}

/* The body, with the username blotted out, under `name` in the app folder. */
static void save_body(const char *name, const char *body, size_t len)
{
	if (!body || !len)
		return;
	char *copy = malloc(len);
	if (!copy) {
		say("      %s: out of memory to save\n", name);
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

/* A stretch of text for the log: line breaks and tabs shown as escapes,
 * cut at `max` bytes without splitting a UTF-8 character. */
static void excerpt(const char *s, size_t len, size_t max, char *out,
                    size_t outlen)
{
	size_t n = len < max ? len : max;
	if (n < len)
		while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
			n--;
	size_t o = 0;
	for (size_t i = 0; i < n && o + 3 < outlen; i++) {
		char c = s[i];
		if (c == '\n' || c == '\r' || c == '\t') {
			out[o++] = '\\';
			out[o++] = c == '\n' ? 'n' : c == '\r' ? 'r' : 't';
		} else {
			out[o++] = c;
		}
	}
	out[o] = '\0';
}

/* ------------------------------------------------------ raw protobuf */

static bool rd_varint(const uint8_t *p, size_t len, size_t *at, uint64_t *v)
{
	uint64_t r = 0;
	for (int sh = 0; *at < len && sh < 64; sh += 7) {
		uint8_t b = p[(*at)++];
		r |= (uint64_t)(b & 0x7F) << sh;
		if (!(b & 0x80)) {
			*v = r;
			return true;
		}
	}
	return false;
}

static bool skip_field(const uint8_t *p, size_t len, size_t *at, int wire)
{
	uint64_t v;
	switch (wire) {
	case 0:
		return rd_varint(p, len, at, &v);
	case 1:
		if (len - *at < 8)
			return false;
		*at += 8;
		return true;
	case 2:
		if (!rd_varint(p, len, at, &v) || v > len - *at)
			return false;
		*at += (size_t)v;
		return true;
	case 5:
		if (len - *at < 4)
			return false;
		*at += 4;
		return true;
	default:
		return false;
	}
}

typedef struct {
	uint32_t field;
	int      wire;
	int      count;
	size_t   bytes;
} field_stat;

/* Every top-level field of a message, in the order first seen. */
static int field_outline(const uint8_t *p, size_t len, field_stat *fs,
                         bool *ok)
{
	int n = 0;
	size_t at = 0;
	*ok = true;
	while (at < len) {
		size_t start = at;
		uint64_t tag;
		if (!rd_varint(p, len, &at, &tag)) {
			*ok = false;
			break;
		}
		int wire = (int)(tag & 7);
		uint32_t f = (uint32_t)(tag >> 3);
		if (!skip_field(p, len, &at, wire)) {
			*ok = false;
			break;
		}
		int i = 0;
		while (i < n && fs[i].field != f)
			i++;
		if (i == n) {
			if (n == FIELDS_MAX)
				continue;
			fs[n].field = f;
			fs[n].wire = wire;
			fs[n].count = 0;
			fs[n].bytes = 0;
			n++;
		}
		fs[i].count++;
		fs[i].bytes += at - start;
	}
	return n;
}

/* "f1 x1 18B, f2 x1 13B, ..." */
static void print_outline(bool screen, const char *indent, const pbv *m)
{
	static field_stat fs[FIELDS_MAX];
	bool ok;
	int n = field_outline(m->p, m->len, fs, &ok);
	char line[640];
	size_t o = 0;
	for (int i = 0; i < n && o + 32 < sizeof(line); i++)
		o += (size_t)snprintf(line + o, sizeof(line) - o, "%s%u%s x%d %uB",
		                      i ? ", " : "", (unsigned)fs[i].field,
		                      fs[i].wire == 2 ? "" : fs[i].wire == 0 ? "v" : "?",
		                      fs[i].count, (unsigned)fs[i].bytes);
	if (screen)
		say("%s%s%s\n", indent, line, ok ? "" : " (MALFORMED)");
	else
		note("%s%s%s\n", indent, line, ok ? "" : " (MALFORMED)");
}

/* Counts the occurrences of a field. */
static int field_count(const pbv *m, uint32_t field)
{
	int n = 0;
	size_t at = 0;
	pbv v;
	while (pb_next_bytes(m, field, &at, &v))
		n++;
	return n;
}

/* --------------------------------------------------- extended metadata */

/* One entity request (or a batch, n > 1) over the spclient session. */
static int ext_request(http_session *s, const char *const *uris, int n,
                       int kind, const char *extra, http_reply *r, double *ms)
{
	uint8_t q_store[16];
	pb q;
	pb_init(&q, q_store, sizeof(q_store));
	pb_varint(&q, 1, (uint64_t)kind);
	size_t cap = (size_t)n * 128 + 64;
	uint8_t *req_store = malloc(cap);
	if (!req_store) {
		memset(r, 0, sizeof(*r));
		snprintf(r->err, sizeof(r->err), "out of memory");
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
		pb_sub(&req, 2, &er);
	}
	if (!pb_ok(&req)) {
		free(req_store);
		memset(r, 0, sizeof(*r));
		snprintf(r->err, sizeof(r->err), "request did not fit");
		return -1;
	}
	double t0 = now_ms();
	int rc = http_session_request_ex(s, EXTMETA_PATH, "POST", net.token,
	                                 "application/x-protobuf",
	                                 "application/x-protobuf", extra, req.p,
	                                 req.len, REPLY_LIMIT, r);
	*ms = now_ms() - t0;
	free(req_store);
	return rc;
}

/* V0: the artist of SAMPLE_TRACK, against the first default. */
static void step_verify(http_session *s)
{
	say("V0 the artist of " SAMPLE_TRACK "\n");
	const char *uri = SAMPLE_TRACK;
	http_reply r;
	double ms;
	if (ext_request(s, &uri, 1, EXT_KIND_TRACK_V4, NULL, &r, &ms) != 0) {
		say("   %s\n\n", r.err);
		return;
	}
	pbv track, artist, gid;
	char id[SP_ID_LEN + 1] = "";
	if (r.status == 200 &&
	    sp_parse_entity((const uint8_t *)r.body, r.len, &track) &&
	    pb_get_bytes(track.p, track.len, 4, &artist) &&
	    pb_get_bytes(artist.p, artist.len, 1, &gid) && gid.len == 16)
		sp_id_from_gid(gid.p, id);
	int status = r.status;
	http_reply_free(&r);
	if (!id[0])
		say("   HTTP %d, no artist read\n\n", status);
	else
		say("   %s: %s the id used for %s\n\n", id,
		    strcmp(id, DEFAULTS[0].id) == 0 ? "SAME as" : "DIFFERENT from",
		    DEFAULTS[0].why);
}

/* --------------------------------------------------------- step 2: bio */

/* The text of the first Biography that has one. */
static bool first_bio(const pbv *a, pbv *text, int *count)
{
	*count = 0;
	bool found = false;
	size_t at = 0;
	pbv b;
	while (pb_next_bytes(a, 12, &at, &b)) {
		(*count)++;
		pbv t;
		if (!found && pb_get_bytes(b.p, b.len, 1, &t) && t.len) {
			*text = t;
			found = true;
		}
	}
	return found;
}

static void step_bio(http_session *s, const pbv *a, artist_sum *sum,
                     bool try_spanish)
{
	say("  2 biography (field 12)  heap %u KB\n", heap_free_kb());
	pbv text;
	if (!first_bio(a, &text, &sum->bios)) {
		say("     %d Biography, none with text\n", sum->bios);
		return;
	}
	sum->bio_len = (int)text.len;
	const char *t = (const char *)text.p;
	int tags = 0, amp = 0, nl = 0, cr = 0, high = 0;
	for (size_t i = 0; i < text.len; i++) {
		unsigned char c = (unsigned char)t[i];
		if (c == '<' && i + 1 < text.len &&
		    (isalpha((unsigned char)t[i + 1]) || t[i + 1] == '/'))
			tags++;
		else if (c == '&') {
			size_t k = i + 1;
			while (k < text.len && k < i + 10 &&
			       (isalnum((unsigned char)t[k]) || t[k] == '#'))
				k++;
			if (k < text.len && t[k] == ';')
				amp++;
		} else if (c == '\n')
			nl++;
		else if (c == '\r')
			cr++;
		else if (c >= 0x80)
			high++;
	}
	say("     %d Biography; text %u B: tags %d, entities %d, \\n %d, "
	    "\\r %d, UTF-8 bytes %d\n", sum->bios, (unsigned)text.len, tags, amp,
	    nl, cr, high);
	pbv bio;
	size_t at = 0;
	if (pb_next_bytes(a, 12, &at, &bio))
		print_outline(false, "     Biography fields: ", &bio);
	char ex[BIO_SHOWN * 2 + 8];
	excerpt(t, text.len, 100, ex, sizeof(ex));
	say("     \"%s\"\n", ex);
	excerpt(t, text.len, BIO_SHOWN, ex, sizeof(ex));
	note("     first %d B: \"%s\"\n", BIO_SHOWN, ex);

	if (!try_spanish)
		return;
	/* Which language comes, and whether a header changes it. */
	char uri[SP_URI_MAX];
	snprintf(uri, sizeof(uri), "spotify:artist:%.22s", sum->id);
	const char *u = uri;
	http_reply r;
	double ms;
	if (ext_request(s, &u, 1, EXT_KIND_ARTIST_V4, "Accept-Language: es\r\n",
	                &r, &ms) != 0) {
		say("     Accept-Language es: %s\n", r.err);
		return;
	}
	pbv a2, t2;
	int n2;
	if (r.status == 200 &&
	    sp_parse_entity((const uint8_t *)r.body, r.len, &a2) &&
	    first_bio(&a2, &t2, &n2)) {
		bool same = t2.len == text.len && memcmp(t2.p, text.p, text.len) == 0;
		excerpt((const char *)t2.p, t2.len, 80, ex, sizeof(ex));
		say("     Accept-Language es: %u B, %s\n       \"%s\"\n",
		    (unsigned)t2.len, same ? "SAME text" : "DIFFERENT text", ex);
	} else {
		say("     Accept-Language es: HTTP %d, no biography\n", r.status);
	}
	http_reply_free(&r);
}

/* ------------------------------------------------------ step 3: groups */

typedef struct {
	int  date;              /* yyyymmdd; month and day 0 when missing */
	int  group;
} rel_seen;

static rel_seen rels[RELEASES_MAX];

/* Date { year 1, month 2, day 3 }: read as sint32, the way meta_parse reads
 * the year; month and day are also checked as plain varints, since a wrong
 * guess reads a plausible number. */
static int read_date(const pbv *album, int *ok_sint, int *ok_raw, bool log)
{
	pbv d;
	if (!pb_get_bytes(album->p, album->len, 6, &d))
		return 0;
	int64_t y = 0, m = 0, day = 0;
	uint64_t rm = 0, rd = 0;
	pb_get_sint(d.p, d.len, 1, &y);
	bool hm = pb_get_sint(d.p, d.len, 2, &m);
	bool hd = pb_get_sint(d.p, d.len, 3, &day);
	pb_get_varint(d.p, d.len, 2, &rm);
	pb_get_varint(d.p, d.len, 3, &rd);
	if (hm) {
		if (m >= 1 && m <= 12)
			(*ok_sint)++;
		if (rm >= 1 && rm <= 12)
			(*ok_raw)++;
	}
	if (log) {
		print_outline(false, "       Date fields: ", &d);
		note("       Date as sint: %d-%d-%d; month/day as varint: %u/%u\n",
		     (int)y, (int)m, (int)day, (unsigned)rm, (unsigned)rd);
	}
	if (y <= 0)
		return 0;
	if (!hm || m < 1 || m > 12)
		m = 0;
	if (!hd || day < 1 || day > 31)
		day = 0;
	return (int)(y * 10000 + m * 100 + day);
}

static void step_groups(const pbv *a, artist_sum *sum)
{
	say("  3 release groups  heap %u KB\n", heap_free_kb());
	int nrels = 0, nuris = 0;
	static char uris[RELEASES_MAX][SP_URI_MAX];
	int ok_sint = 0, ok_raw = 0;
	for (int g = 0; g < 4; g++) {
		uint32_t field = 5 + (uint32_t)g;
		int types[6] = { 0 };
		int max_variants = 0, desc = 0, asc = 0, prev = -1;
		size_t at = 0;
		pbv ag;
		bool first_logged = false;
		while (pb_next_bytes(a, field, &at, &ag)) {
			sum->groups[g]++;
			int variants = field_count(&ag, 1);
			sum->albums[g] += variants;
			if (variants > max_variants)
				max_variants = variants;
			pbv album;
			if (!pb_get_bytes(ag.p, ag.len, 1, &album))
				continue;
			if (!first_logged) {
				print_outline(false, "     first AlbumGroup: ", &ag);
				print_outline(g < 3, "     first Album: ", &album);
			}
			uint64_t type = 0;
			bool typed = pb_get_varint(album.p, album.len, 4, &type);
			types[typed && type <= 4 ? (int)type : 5]++;
			int date = read_date(&album, &ok_sint, &ok_raw, !first_logged);
			pbv name, gid;
			bool named = pb_get_bytes(album.p, album.len, 2, &name) &&
			             name.len;
			if (!first_logged) {
				char nm[64];
				excerpt(named ? (const char *)name.p : "", named ? name.len : 0,
				        48, nm, sizeof(nm));
				note("       name \"%s\", type %d, date %d\n", nm,
				     typed ? (int)type : -1, date);
				first_logged = true;
			}
			if (g == 3)
				continue;
			/* What phase 1 would keep: the first Album of a group. */
			sum->firsts++;
			sum->named += named;
			sum->typed += typed;
			sum->dated_y += date > 0;
			sum->dated_ym += date % 10000 / 100 > 0;
			sum->dated_ymd += date % 100 > 0;
			sum->covered += pb_has(album.p, album.len, 17) ||
			                pb_has(album.p, album.len, 9);
			if (date > 0 && prev > 0) {
				if (date < prev)
					desc++;
				else if (date > prev)
					asc++;
			}
			if (date > 0)
				prev = date;
			if (nrels < RELEASES_MAX) {
				rels[nrels].date = date;
				rels[nrels].group = g;
				nrels++;
			}
			if (nuris < RELEASES_MAX &&
			    pb_get_bytes(album.p, album.len, 1, &gid) && gid.len == 16 &&
			    sp_uri_from_gid(SP_KIND_ALBUM, gid.p, uris[nuris],
			                    SP_URI_MAX))
				nuris++;
		}
		say("     %s(%u): %d groups, %d albums, up to %d variants\n",
		    GROUP_NAME[g], (unsigned)field, sum->groups[g], sum->albums[g],
		    max_variants);
		say("       type 0/album/single/comp/ep/missing %d/%d/%d/%d/%d/%d;"
		    " dates %d down, %d up\n", types[0], types[1], types[2],
		    types[3], types[4], types[5], desc, asc);
		frame();
	}
	int mdesc = 0, masc = 0, prev = -1;
	for (int i = 0; i < nrels; i++) {
		if (rels[i].date > 0 && prev > 0) {
			if (rels[i].date < prev)
				mdesc++;
			else if (rels[i].date > prev)
				masc++;
		}
		if (rels[i].date > 0)
			prev = rels[i].date;
	}
	say("     kept %d (groups 5-7): named %d, typed %d, cover %d\n",
	    sum->firsts, sum->named, sum->typed, sum->covered);
	say("       dated: year %d, +month %d, +day %d; month as sint ok %d, "
	    "as varint ok %d\n", sum->dated_y, sum->dated_ym, sum->dated_ymd,
	    ok_sint, ok_raw);
	say("       all groups in arrival order: %d down, %d up\n", mdesc, masc);

	if (sum->named < sum->firsts || sum->dated_y < sum->firsts)
		need_batch = true;
	if (nuris > nbatch) {
		memcpy(batch_uris, uris, sizeof(uris[0]) * (size_t)nuris);
		nbatch = nuris;
	}
}

/* ------------------------------------------------- step 5a: insights */

static void step_insights(http_session *s, int n, artist_sum *sum)
{
	say("  5a artist-insights  heap %u KB\n", heap_free_kb());
	sum->a_listeners = -1;
	char path[160];
	snprintf(path, sizeof(path),
	         "/creatorabout/v0/artist-insights/%s?format=json&locale=es",
	         sum->id);
	http_reply r;
	double t0 = now_ms();
	int rc = http_session_request(s, path, "GET", net.token, NULL,
	                              "application/json", NULL, 0, REPLY_LIMIT,
	                              &r);
	sum->a_ms = now_ms() - t0;
	if (rc != 0) {
		say("     %s (%.0f ms)\n", r.err, sum->a_ms);
		return;
	}
	sum->a_status = r.status;
	say("     HTTP %d, %u B, %.0f ms (%s link)\n", r.status, (unsigned)r.len,
	    sum->a_ms, http_session_last_reused(s) ? "kept" : "new");
	if (r.status != 200 || !r.body) {
		char ex[200];
		excerpt(r.body ? r.body : "", r.len, 160, ex, sizeof(ex));
		say("     %s\n", ex);
		http_reply_free(&r);
		return;
	}
	long v;
	if (json_int(r.body, "monthlyListeners", &v))
		sum->a_listeners = v;
	static char bio[BIO_MAX];
	bool has_bio = json_str(r.body, "biography", bio, sizeof(bio));
	bool has_auto = memmem(r.body, r.len, "\"autobiography\"", 15) != NULL;
	sum->a_bio = has_bio ? (int)strlen(bio) : 0;
	say("     monthlyListeners %ld; biography %d B; autobiography %s\n",
	    sum->a_listeners, sum->a_bio, has_auto ? "present" : "absent");
	if (has_bio && bio[0]) {
		char ex[200];
		excerpt(bio, strlen(bio), 80, ex, sizeof(ex));
		say("     \"%s\"\n", ex);
	}
	char name[48];
	snprintf(name, sizeof(name), "probe_artist_%d_insights.json", n + 1);
	save_body(name, r.body, r.len);
	http_reply_free(&r);
}

/* ------------------------------------------------- step 5b: pathfinder */

/* The operations the bundle declares with "rtist" in their name, to the
 * log: if queryArtistOverview is missing, what is there instead. */
static void list_artist_ops(const char *js, size_t len)
{
	static const char MID[] = "\",\"query\",\"";
	const char *p = js, *e = js + len;
	int shown = 0;
	while (shown < 32 &&
	       (p = memmem(p, (size_t)(e - p), MID, sizeof(MID) - 1)) != NULL) {
		const char *s = p;
		while (s > js && s[-1] != '"' && p - s < 64)
			s--;
		if (memmem(s, (size_t)(p - s), "rtist", 5)) {
			note("     op %.*s %.12s...\n", (int)(p - s), s,
			     p + sizeof(MID) - 1);
			shown++;
		}
		p += sizeof(MID) - 1;
	}
	note("     %d artist operations declared\n", shown);
}

/* 5b0: the hash, from the mobile web player, as pf_hash_refresh does. */
static void step_artist_hash(void)
{
	say("5b0 %s hash from the mobile player  heap %u KB\n", ARTIST_OP,
	    heap_free_kb());
	double start = now_ms();
	http_reply r;
	char bundle[256];
	bool named = false;
	if (http_get_url(PLAYER_PAGE, 1024 * 1024, &r) != 0) {
		say("   page: %s\n", r.err);
	} else {
		named = r.status == 200 && r.body &&
		        pf_find_bundle(r.body, bundle, sizeof(bundle));
		say("   page: HTTP %d, %u B%s\n", r.status, (unsigned)r.len,
		    named ? "" : ", names no bundle");
		http_reply_free(&r);
	}
	if (named) {
		double t0 = now_ms();
		if (http_get_url(bundle, 1024 * 1024, &r) != 0) {
			say("   bundle: %s\n", r.err);
		} else {
			say("   bundle: HTTP %d, %u B, %.0f ms\n", r.status,
			    (unsigned)r.len, now_ms() - t0);
			size_t off, dlen, isize, len = 0;
			char *js = NULL;
			if (r.body && gzip_payload((const uint8_t *)r.body, r.len, &off,
			                           &dlen, &isize) &&
			    isize && isize <= INFLATED_MAX &&
			    (js = malloc(isize + 1)) != NULL) {
				int got = stbi_zlib_decode_noheader_buffer(js, (int)isize,
				                                           r.body + off,
				                                           (int)dlen);
				if (got == (int)isize) {
					js[isize] = '\0';
					len = isize;
				} else {
					free(js);
					js = NULL;
				}
			} else if (r.body) {
				js = malloc(r.len + 1);
				if (js) {
					memcpy(js, r.body, r.len);
					js[r.len] = '\0';
					len = r.len;
				}
			}
			http_reply_free(&r);
			if (js) {
				say("   bundle text %u B; heap %u KB while held\n",
				    (unsigned)len, heap_free_kb());
				list_artist_ops(js, len);
				if (pf_find_hash(js, len, ARTIST_OP, artist_hash))
					snprintf(hash_source, sizeof(hash_source), "the bundle");
				char search[65];
				if (pf_find_hash(js, len, "findTopResults", search))
					say("   findTopResults hash %.12s...\n", search);
				free(js);
			} else {
				say("   bundle does not inflate\n");
			}
		}
	}
	if (!artist_hash[0]) {
		snprintf(artist_hash, sizeof(artist_hash), "%s", OLD_ARTIST_HASH);
		snprintf(hash_source, sizeof(hash_source), "the 2024 one (fallback)");
	}
	say("   %s hash %.12s... from %s, %.1f s\n\n", ARTIST_OP, artist_hash,
	    hash_source, (now_ms() - start) / 1000.0);
}

static void step_overview(http_session *s, int n, artist_sum *sum,
                          bool *first)
{
	say("  5b %s  heap %u KB\n", ARTIST_OP, heap_free_kb());
	sum->b_listeners = -1;
	char body[512];
	int bl = snprintf(body, sizeof(body),
	                  "{\"variables\":{\"uri\":\"spotify:artist:%s\","
	                  "\"locale\":\"\",\"includePrerelease\":true},"
	                  "\"operationName\":\"" ARTIST_OP "\",\"extensions\":"
	                  "{\"persistedQuery\":{\"version\":1,\"sha256Hash\":"
	                  "\"%s\"}}}", sum->id, artist_hash);
	static const char *const EXTRA[2] = {
		"User-Agent: " BROWSER_UA "\r\n",
		"User-Agent: " BROWSER_UA "\r\n"
		"App-Platform: WebPlayer\r\n"
		"Origin: https://open.spotify.com\r\n"
		"Referer: https://open.spotify.com/\r\n",
	};
	http_reply r;
	memset(&r, 0, sizeof(r));
	for (int h = 0; h < 2; h++) {
		double t0 = now_ms();
		int rc = http_session_request_ex(s, PF_PATH, "POST", net.token,
		                                 "application/json;charset=UTF-8",
		                                 "application/json", EXTRA[h], body,
		                                 (size_t)bl, REPLY_LIMIT, &r);
		sum->b_ms = now_ms() - t0;
		if (rc != 0) {
			say("     %s (%.0f ms)\n", r.err, sum->b_ms);
			return;
		}
		sum->b_status = r.status;
		say("     %s: HTTP %d, %u B, %.0f ms%s\n",
		    h ? "+platform+origin" : "browser UA", r.status, (unsigned)r.len,
		    sum->b_ms, *first ? " (with the handshake)" : "");
		*first = false;
		bool errors = r.body && memmem(r.body, r.len, "\"errors\"", 8);
		if (r.status == 200 && !errors)
			break;
		char ex[200];
		excerpt(r.body ? r.body : "", r.len, 160, ex, sizeof(ex));
		say("     %s\n", ex);
		int status = r.status;
		http_reply_free(&r);
		if (status == 412 || h == 1)
			return;
	}
	long v;
	if (json_member_int(r.body, r.len, "data.artistUnion.stats.monthlyListeners",
	                    &v))
		sum->b_listeners = v;
	static char bio[BIO_MAX];
	if (json_member_str(r.body, r.len,
	                    "data.artistUnion.profile.biography.text", bio,
	                    sizeof(bio)))
		sum->b_bio = (int)strlen(bio);
	say("     monthlyListeners %ld; biography %d B\n", sum->b_listeners,
	    sum->b_bio);
	if (sum->b_bio) {
		char ex[200];
		excerpt(bio, strlen(bio), 80, ex, sizeof(ex));
		say("     \"%s\"\n", ex);
	}
	if (r.len > DEFAULT_CAP)
		say("     OVER the %u KB cap\n", DEFAULT_CAP / 1024);
	char name[48];
	snprintf(name, sizeof(name), "probe_artist_%d_overview.json", n + 1);
	save_body(name, r.body, r.len);
	http_reply_free(&r);
}

/* --------------------------------------------------- one artist, 1-3 */

static void step_artist(http_session *sp, http_session *pf, int n,
                        bool *pf_first)
{
	artist_sum *sum = &sums[n];
	unsigned before = heap_free_kb();
	say("A%d %s (%s)\n", n + 1, sum->id, sum->why);
	say("  1 artist metadata  heap %u KB\n", before);
	char uri[SP_URI_MAX];
	snprintf(uri, sizeof(uri), "spotify:artist:%.22s", sum->id);
	const char *u = uri;
	http_reply r;
	if (ext_request(sp, &u, 1, EXT_KIND_ARTIST_V4, NULL, &r, &sum->ms) != 0) {
		say("     %s\n", r.err);
	} else {
		sum->status = r.status;
		sum->bytes = r.len;
		say("     HTTP %d, %u B, %.0f ms%s; heap %u KB while held\n",
		    r.status, (unsigned)r.len, sum->ms,
		    r.len > DEFAULT_CAP ? ", OVER the default cap" : ", fits",
		    heap_free_kb());
		pbv a;
		if (r.status == 200 &&
		    sp_parse_entity((const uint8_t *)r.body, r.len, &a)) {
			sum->parsed = true;
			pbv nm;
			if (pb_get_bytes(a.p, a.len, 2, &nm))
				excerpt((const char *)nm.p, nm.len, 48, sum->name,
				        sizeof(sum->name));
			say("     name \"%s\", Artist %u B\n", sum->name,
			    (unsigned)a.len);
			print_outline(true, "     fields: ", &a);
			if (n == 0)
				save_body("probe_artist_1_metadata.bin", r.body, r.len);
			frame();
			step_bio(sp, &a, sum, n == 0);
			frame();
			step_groups(&a, sum);
		} else {
			say("     no artist in the reply\n");
		}
		http_reply_free(&r);
	}
	frame();
	step_insights(sp, n, sum);
	frame();
	if (pf)
		step_overview(pf, n, sum, pf_first);
	say("  heap %u KB before, %u KB after\n\n", before, heap_free_kb());
	frame();
}

/* ------------------------------------------------ step 4: album batch */

static void step_batch(http_session *s)
{
	static const int SIZES[] = { 10, 20, 50 };
	say("4 album metadata batch  heap %u KB\n", heap_free_kb());
	if (!need_batch) {
		say("   skipped: every release came named and dated\n\n");
		return;
	}
	const char *ptrs[50];
	for (int k = 0; k < 3; k++) {
		int n = SIZES[k] < nbatch ? SIZES[k] : nbatch;
		if (n <= 0)
			break;
		for (int i = 0; i < n; i++)
			ptrs[i] = batch_uris[i];
		http_reply r;
		double ms;
		if (ext_request(s, ptrs, n, EXT_KIND_ALBUM_V4, NULL, &r, &ms) != 0) {
			say("   %d: %s\n", n, r.err);
			continue;
		}
		int got = 0, named = 0, dated = 0;
		pbv arr, ed, any, val;
		if (r.status == 200 &&
		    pb_get_bytes((const uint8_t *)r.body, r.len, 2, &arr)) {
			size_t at = 0;
			while (pb_next_bytes(&arr, 3, &at, &ed)) {
				if (!pb_get_bytes(ed.p, ed.len, 3, &any) ||
				    !pb_get_bytes(any.p, any.len, 2, &val))
					continue;
				got++;
				named += pb_has(val.p, val.len, 2);
				dated += pb_has(val.p, val.len, 6);
			}
		}
		say("   %d: HTTP %d, %u B, %.0f ms; %d back, %d named, %d dated\n",
		    n, r.status, (unsigned)r.len, ms, got, named, dated);
		http_reply_free(&r);
		frame();
		if (n < SIZES[k])
			break;
	}
	say("\n");
}

/* ------------------------------------------------------------------ setup */

/* An artist id out of a line: a URI, a share link or the bare id. */
static bool id_from_text(const char *s, char out[SP_ID_LEN + 1])
{
	const char *p = strstr(s, "artist:");
	if (!p)
		p = strstr(s, "artist/");
	p = p ? p + 7 : s;
	while (*p == ' ' || *p == '\t')
		p++;
	int k = 0;
	while (k < SP_ID_LEN && isalnum((unsigned char)p[k]))
		k++;
	if (k != SP_ID_LEN || isalnum((unsigned char)p[SP_ID_LEN]))
		return false;
	memcpy(out, p, SP_ID_LEN);
	out[SP_ID_LEN] = '\0';
	uint8_t gid[16];
	return sp_gid_from_id(out, gid);
}

static void load_artists(void)
{
	for (int i = 0; i < NDEFAULTS; i++) {
		snprintf(sums[i].id, sizeof(sums[i].id), "%s", DEFAULTS[i].id);
		snprintf(sums[i].why, sizeof(sums[i].why), "%s", DEFAULTS[i].why);
	}
	nartists = NDEFAULTS;
	char p[320];
	data_path("probe_artist.txt", p, sizeof(p));
	FILE *f = fopen(p, "r");
	if (!f)
		return;
	char line[256];
	while (nartists < ARTISTS_MAX && fgets(line, sizeof(line), f)) {
		artist_sum *s = &sums[nartists];
		if (id_from_text(line, s->id)) {
			snprintf(s->why, sizeof(s->why), "from probe_artist.txt");
			nartists++;
		}
	}
	fclose(f);
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
	say("   ok; spclient %s\n\n", net.host);
	return true;
}

/* ---------------------------------------------------------------- summary */

static void summary(void)
{
	say("SUMMARY\n");
	bool branch_a = true;
	int a_ok = 0, b_ok = 0, a_n = 0, b_n = 0;
	double a_sum = 0, b_sum = 0;
	for (int i = 0; i < nartists; i++) {
		const artist_sum *s = &sums[i];
		say(" %d %.20s: %uKB %.0fms bio %d rel %d/%d/%d\n", i + 1,
		    s->name[0] ? s->name : s->id, (unsigned)(s->bytes / 1024), s->ms,
		    s->bio_len, s->groups[0], s->groups[1], s->groups[2]);
		say("   named %d/%d dated %d/%d/%d cover %d; 5a %d %ld (%.0fms) "
		    "5b %d %ld (%.0fms)\n", s->named, s->firsts, s->dated_y,
		    s->dated_ym, s->dated_ymd, s->covered, s->a_status,
		    s->a_listeners, s->a_ms, s->b_status, s->b_listeners, s->b_ms);
		if (s->parsed && (s->named < s->firsts || s->dated_y < s->firsts ||
		                  s->covered < s->firsts))
			branch_a = false;
		if (s->a_listeners >= 0) {
			a_ok++;
			a_sum += s->a_ms;
			a_n++;
		}
		if (s->b_listeners >= 0) {
			b_ok++;
			b_sum += s->b_ms;
			b_n++;
		}
	}
	say(" releases: branch %s\n", branch_a ? "A (all in the artist reply)"
	                                       : "B (name them by batch)");
	say(" listeners: 5a %d of %d (avg %.0f ms), 5b %d of %d (avg %.0f ms)\n",
	    a_ok, nartists, a_n ? a_sum / a_n : 0.0, b_ok, nartists,
	    b_n ? b_sum / b_n : 0.0);
	say(" 5b hash from %s\n\n", hash_source[0] ? hash_source : "-");
}

int main(int argc, char **argv)
{
	(void)argc;
	gfxInitDefault();
	consoleInit(GFX_TOP, NULL);
	const char *argv0 = (argv && argv[0]) ? argv[0] : NULL;
	cfg_set_path(argv0);
	open_log();

	say("3DS Spotify -- artist detail probe\n");
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

	load_artists();
	say("heap %u KB at start; %d artists\n", heap_free_kb(), nartists);
	if (net_setup()) {
		http_session *sp = http_session_open(net.host);
		if (!sp) {
			say("no spclient session\n");
		} else {
			step_verify(sp);
			step_artist_hash();
			http_session *pf = http_session_open(PF_HOST);
			if (!pf)
				say("no pathfinder session: 5b skipped\n\n");
			bool pf_first = true;
			for (int i = 0; i < nartists && aptMainLoop(); i++)
				step_artist(sp, pf, i, &pf_first);
			if (aptMainLoop())
				step_batch(sp);
			if (pf)
				http_session_close(pf);
			http_session_close(sp);
			summary();
		}
	}
	memset(&net, 0, sizeof(net));
	say("done. Log, .json and .bin in the app folder.\n");

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
