/* Typed search results: what pathfinder answers, read into this client's
 * structs, and the same kinds made up from a list of tracks when pathfinder
 * cannot be asked.
 *
 * Pathfinder (api-partner.spotify.com/pathfinder/v2/query) is the service
 * the mobile web player searches with. Its operations are persisted
 * queries named by a hash that the player's bundle declares; finding it
 * again is pure text work, kept here (pf_find_bundle, gzip_payload,
 * pf_find_hash) so the host tests cover it. The requests themselves are in
 * pathfinder.c, the hash's upkeep in pf_hash.c.
 *
 * Shapes, as probe_search saw them: everything under data.searchV2, one
 * section per kind -- tracksV2.items[].item.data, artists.items[].data,
 * albumsV2.items[].data, playlists.items[].data -- and, in findTopResults,
 * topResultsV2.itemsV2[].item.data with a __typename that may be a kind this
 * client does not show (Genre, User, Podcast...).
 *
 * Nothing here touches the 3DS, so it builds on the host too.
 */
#ifndef SEARCH_RESULTS_H
#define SEARCH_RESULTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "spclient.h"

/* Rows kept per kind besides tracks (which keep SEARCH_LIMIT). */
#define SEARCH_ITEMS_MAX 12

/* The kinds, in the order the results show them. */
typedef enum {
	RESULT_TRACKS, RESULT_ARTISTS, RESULT_ALBUMS, RESULT_PLAYLISTS,
	RESULT_KINDS
} result_kind;

/* The find* operation that lists one kind. */
const char *pf_operation(result_kind kind);

/* Request bodies. False when the query does not fit. */
bool pf_body_top(const char *query, int top, const char *hash, char *out,
                 size_t size);
bool pf_body_page(result_kind kind, const char *query, int limit,
                  const char *hash, char *out, size_t size);

/* One page of `kind` from a find* reply (or the sections of
 * findTopResults), as sp_track rows: an artist fills uri, name and art_*;
 * an album uri, name, artist and art_*; a playlist uri, name, artist (its
 * owner) and art_*; a track its uri alone, since the metadata batch names
 * it and brings the files to play. Items of another type or without a URI
 * are passed over. Returns how many, or -1 if the section is missing or
 * does not parse. */
int pf_parse_page(const char *json, size_t len, result_kind kind,
                  sp_track *out, int max);

/* The first top result that is one of our kinds: its kind and uri. False
 * when there is none. */
bool pf_parse_best(const char *json, size_t len, result_kind *kind,
                   char *uri, size_t size);

/* A cover URL as an i.scdn.co one, or "" when there is none this client
 * can show. image-cdn-{fa,ak}.spotifycdn.com/image/<id> serves the same
 * image as i.scdn.co/image/<id>; a mosaic.scdn.co/<size>/<id1><id2>... is
 * shown by its first tile; an uploaded cover (ab67706c0000d72c...) comes as
 * WebP, which stb_image cannot read, while the same id with the size code
 * da84 comes as JPEG. pickasso (covers drawn on the fly) and any other host
 * read as no cover, and so does a URL that does not fit. */
void pf_cover_url(const char *url, char *out, size_t size);

/* The persisted-query hash, step by step. pf_find_bundle: the mobile
 * player's script URL out of the page (the same name comes first as a
 * stylesheet, so only "<hex>.js" counts). gzip_payload: where the deflate
 * data of a gzip member lies, and its inflated size (the CDN sends the
 * bundle gzipped unasked). pf_find_hash: the 64 hex declared for `op`. */
bool pf_find_bundle(const char *html, char *url, size_t size);
bool gzip_payload(const uint8_t *b, size_t n, size_t *off, size_t *len,
                  size_t *isize);
bool pf_find_hash(const char *js, size_t len, const char *op, char hash[65]);

/* Today's search, when pathfinder cannot be asked, still shows artists and
 * albums: the distinct ones of `tracks`, by URI, in the order they first
 * appear. Two limits: an album's artist is its track's first one, which in
 * a compilation is not the album's; and an artist has no portrait, since
 * a track does not carry one. */
int search_derive_artists(const sp_track *tracks, int n, sp_track *out,
                          int max);
int search_derive_albums(const sp_track *tracks, int n, sp_track *out,
                         int max);

/* Where `uri` is among `items`, or -1. */
int search_find_uri(const sp_track *items, int n, const char *uri);

#endif
