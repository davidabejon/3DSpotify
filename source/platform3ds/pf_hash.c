#include "pf_hash.h"
#include "cfg3ds.h"
#include "http3ds.h"
#include "search_results.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The inflater only; the implementation is ui/artwork.c's. */
#include "../../include/stb_image.h"

/* With the default User-Agent the page is the mobile player's (~314 KB). */
#define PLAYER_PAGE   "https://open.spotify.com/"
#define PAGE_LIMIT    (1024 * 1024)
/* The bundle comes gzipped, ~370 KB, and inflates to ~1.4 MB. */
#define BUNDLE_LIMIT  (1024 * 1024)
#define INFLATED_MAX  (4 * 1024 * 1024)

static char current[65];
static bool loaded;
static bool tried;

static bool valid(const char *h)
{
	if (strlen(h) != 64)
		return false;
	for (const char *p = h; *p; p++)
		if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
			return false;
	return true;
}

const char *pf_hash_current(void)
{
	if (!loaded) {
		char v[80];
		const char *h = cfg_get("pf_hash", v, sizeof(v)) && valid(v)
		    ? v : PF_HASH_DEFAULT;
		memcpy(current, h, sizeof(current) - 1);   /* valid: 64 hex */
		current[sizeof(current) - 1] = '\0';
		loaded = true;
	}
	return current;
}

/* The bundle as text: inflated when it came gzipped. NULL when it cannot
 * be read; the caller frees what comes back. */
static char *bundle_text(const http_reply *r, size_t *len)
{
	size_t off, dlen, isize;
	if (!gzip_payload((const uint8_t *)r->body, r->len, &off, &dlen,
	                  &isize)) {
		/* Sent plain after all: the reply is the text. */
		char *copy = malloc(r->len + 1);
		if (copy) {
			memcpy(copy, r->body, r->len);
			copy[r->len] = '\0';
			*len = r->len;
		}
		return copy;
	}
	if (isize == 0 || isize > INFLATED_MAX)
		return NULL;
	char *out = malloc(isize + 1);
	if (!out)
		return NULL;
	int got = stbi_zlib_decode_noheader_buffer(out, (int)isize,
	                                           r->body + off, (int)dlen);
	if (got != (int)isize) {
		free(out);
		return NULL;
	}
	out[isize] = '\0';
	*len = isize;
	return out;
}

int pf_hash_refresh(char *err, size_t errlen)
{
	if (tried) {
		snprintf(err, errlen, "hash: already tried this run");
		return -1;
	}
	tried = true;

	http_reply r;
	if (http_get_url(PLAYER_PAGE, PAGE_LIMIT, &r) != 0) {
		snprintf(err, errlen, "hash: page: %s", r.err);
		return -1;
	}
	char bundle[256];
	bool named = r.status == 200 && r.body &&
	             pf_find_bundle(r.body, bundle, sizeof(bundle));
	int page_status = r.status;
	http_reply_free(&r);
	if (!named) {
		snprintf(err, errlen, "hash: page HTTP %d names no player bundle",
		         page_status);
		return -1;
	}

	if (http_get_url(bundle, BUNDLE_LIMIT, &r) != 0) {
		snprintf(err, errlen, "hash: bundle: %s", r.err);
		return -1;
	}
	if (r.status != 200 || !r.body) {
		snprintf(err, errlen, "hash: bundle HTTP %d", r.status);
		http_reply_free(&r);
		return -1;
	}
	size_t len = 0;
	char *js = bundle_text(&r, &len);
	http_reply_free(&r);
	if (!js) {
		snprintf(err, errlen, "hash: bundle does not inflate");
		return -1;
	}

	/* findTopResults and the find* of every kind share one hash; one that
	 * differs means the bundle changed shape, and nothing found is
	 * trusted. */
	char hash[65], other[65];
	bool ok = pf_find_hash(js, len, "findTopResults", hash);
	for (int k = 0; ok && k < RESULT_KINDS; k++)
		ok = pf_find_hash(js, len, pf_operation((result_kind)k), other) &&
		     strcmp(other, hash) == 0;
	free(js);
	if (!ok || !valid(hash)) {
		snprintf(err, errlen, "hash: not declared alike by every find*");
		return -1;
	}

	snprintf(current, sizeof(current), "%s", hash);
	loaded = true;
	cfg_set_async("pf_hash", hash);
	return 0;
}
