/* Typed search through pathfinder, the service the mobile web player
 * searches with: tracks, artists, albums and playlists, and the one result
 * Spotify ranks first.
 *
 * It takes the login5 token the rest of spclient uses, but only from a
 * browser: without a browser's User-Agent it answers 403. It is not a
 * stable way in -- the hash its queries are named by changes (pf_hash.h)
 * -- so a failure here is never the search's failure: the caller falls
 * back to sp_search_tracks.
 */
#ifndef PATHFINDER_H
#define PATHFINDER_H

#include <stdbool.h>
#include <stddef.h>

#include "search_results.h"

/* Returned when the hash is no longer known (HTTP 412). */
#define PF_STALE_HASH (-3)
/* Returned when `cancel` was raised between requests. */
#define PF_CANCELLED  (-2)

/* The caller's arrays, one per kind, filled in place: rows as
 * pf_parse_page leaves them (tracks by URI only). */
typedef struct {
	sp_track   *items[RESULT_KINDS];
	int         max[RESULT_KINDS];
	int         count[RESULT_KINDS];
	int         best_kind;              /* a result_kind, -1 when none */
	char        best_uri[SP_URI_MAX];
	int         status;                 /* the last HTTP status, 0 if none */
	/* Milliseconds per request: findTopResults, then each kind's page. */
	int         ms[1 + RESULT_KINDS];
} sp_typed_search;

/* findTopResults (the best result, and five of each kind to start from),
 * then a page of each kind: tracks first, the rest after. `cancel` is
 * looked at between requests. A failed page of a kind other than tracks
 * keeps the five findTopResults brought; any other failure fails it all.
 * Returns 0, PF_STALE_HASH, PF_CANCELLED or -1 (with `err` set and
 * out->status the HTTP status, when one came). */
int sp_search_typed(const char *token, const char *query, const char *hash,
                    sp_typed_search *out, volatile bool *cancel,
                    char *err, size_t errlen);

#endif
