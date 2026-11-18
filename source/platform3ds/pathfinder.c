#include "pathfinder.h"
#include "http3ds.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#define PF_HOST "api-partner.spotify.com"
#define PF_PATH "/pathfinder/v2/query"
/* Pathfinder serves browsers only: the Bearer alone, or with the web
 * player's App-Platform, is refused with 403 "Client/request not allowed",
 * while a browser's User-Agent alone is let through (probe_search, P1).
 * Written here and nowhere else, so the one header this depends on is easy
 * to find when Spotify moves the line. */
#define BROWSER_UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) " \
                   "AppleWebKit/537.36 (KHTML, like Gecko) " \
                   "Chrome/130.0.0.0 Safari/537.36"
#define PF_HEADERS "User-Agent: " BROWSER_UA "\r\n"

/* findTopResults runs to ~56 KB with ten top results; a page of 20 tracks
 * to ~30 KB. */
#define PF_REPLY_LIMIT (256 * 1024)
#define PF_TOP         10
#define PF_TRACKS      20
#define PF_BODY_MAX    1024

/* One query over the session. Returns the HTTP status, or -1 when no
 * answer came; the caller frees the reply when the status is set. */
static int ask(http_session *sess, const char *token, const char *body,
               http_reply *r, int *ms, char *err, size_t errlen)
{
	u64 t0 = osGetTime();
	int rc = http_session_request_ex(sess, PF_PATH, "POST", token,
	                                 "application/json;charset=UTF-8",
	                                 "application/json", PF_HEADERS, body,
	                                 strlen(body), PF_REPLY_LIMIT, r);
	*ms = (int)(osGetTime() - t0);
	if (rc != 0) {
		snprintf(err, errlen, "pathfinder: %s", r->err);
		return -1;
	}
	return r->status;
}

/* What a status other than 200 means for the whole search. */
static int refused(int status, sp_typed_search *out, char *err,
                   size_t errlen)
{
	out->status = status;
	snprintf(err, errlen, "pathfinder returned HTTP %d", status);
	return status == 412 ? PF_STALE_HASH : -1;
}

int sp_search_typed(const char *token, const char *query, const char *hash,
                    sp_typed_search *out, volatile bool *cancel,
                    char *err, size_t errlen)
{
	if (!token || !query || !hash || !out) {
		snprintf(err, errlen, "invalid search arguments");
		return -1;
	}
	for (int k = 0; k < RESULT_KINDS; k++)
		out->count[k] = 0;
	out->best_kind = -1;
	out->best_uri[0] = '\0';
	out->status = 0;
	memset(out->ms, 0, sizeof(out->ms));

	char body[PF_BODY_MAX];
	if (!pf_body_top(query, PF_TOP, hash, body, sizeof(body))) {
		snprintf(err, errlen, "pathfinder: query too long");
		return -1;
	}
	http_session *sess = http_session_open(PF_HOST);
	if (!sess) {
		snprintf(err, errlen, "pathfinder: out of memory");
		return -1;
	}

	int rc = -1;
	http_reply r;
	int status = ask(sess, token, body, &r, &out->ms[0], err, errlen);
	if (status < 0)
		goto done;
	if (status != 200) {
		http_reply_free(&r);
		rc = refused(status, out, err, errlen);
		goto done;
	}
	out->status = status;
	/* The best result, and five of each kind, which stand in for a page
	 * that fails below. */
	result_kind best;
	if (pf_parse_best(r.body, r.len, &best, out->best_uri,
	                  sizeof(out->best_uri)))
		out->best_kind = (int)best;
	for (int k = 0; k < RESULT_KINDS; k++) {
		int n = pf_parse_page(r.body, r.len, (result_kind)k, out->items[k],
		                      out->max[k]);
		out->count[k] = n > 0 ? n : 0;
	}
	http_reply_free(&r);

	/* Tracks first: they are the kind every search shows. */
	static const result_kind ORDER[] = {
		RESULT_TRACKS, RESULT_ARTISTS, RESULT_ALBUMS, RESULT_PLAYLISTS,
	};
	for (size_t i = 0; i < sizeof(ORDER) / sizeof(ORDER[0]); i++) {
		result_kind k = ORDER[i];
		if (cancel && *cancel) {
			rc = PF_CANCELLED;
			goto done;
		}
		int limit = k == RESULT_TRACKS ? PF_TRACKS : SEARCH_ITEMS_MAX;
		if (limit > out->max[k])
			limit = out->max[k];
		if (!pf_body_page(k, query, limit, hash, body, sizeof(body))) {
			snprintf(err, errlen, "pathfinder: query too long");
			goto done;
		}
		status = ask(sess, token, body, &r, &out->ms[1 + k], err, errlen);
		bool required = k == RESULT_TRACKS;
		if (status < 0) {
			if (required)
				goto done;
			continue;
		}
		if (status != 200) {
			http_reply_free(&r);
			if (required || status == 412) {
				rc = refused(status, out, err, errlen);
				goto done;
			}
			continue;
		}
		int n = pf_parse_page(r.body, r.len, k, out->items[k], out->max[k]);
		http_reply_free(&r);
		if (n >= 0)
			out->count[k] = n;
		else if (required) {
			snprintf(err, errlen, "pathfinder: tracks do not parse");
			goto done;
		}
	}
	rc = 0;

done:
	http_session_close(sess);
	if (rc != 0 && rc != PF_STALE_HASH && cancel && *cancel)
		rc = PF_CANCELLED;
	return rc;
}
