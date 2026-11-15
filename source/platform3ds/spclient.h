/* spclient: Spotify's internal API, the one that answers for this client.
 *
 * Needs a login5 Bearer token -- the OAuth token is refused here, and
 * api.spotify.com answers 429 for the desktop client id, so this is the only
 * way in. Everything speaks protobuf except a few endpoints that answer JSON.
 */
#ifndef SPCLIENT_H
#define SPCLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "spotify_id.h"

typedef struct http_session http_session;

#define SP_MAX_FILES 8

/* Audio formats, from metadata.proto's AudioFile.Format. Only the Vorbis ones
 * matter here: the 3DS decodes Vorbis in software and has no AAC decoder worth
 * using. 160 kbps is the one to prefer -- 320 triples the bandwidth over
 * 802.11g for a difference the console's speakers cannot show. */
#define SP_FMT_OGG_96   0
#define SP_FMT_OGG_160  1
#define SP_FMT_OGG_320  2

typedef struct {
	uint8_t id[20];
	int     format;
} sp_file;

typedef struct {
	char    uri[64];
	/* The track's raw 16-byte id. The audio key request wants this, not the
	 * URI -- it is the same value the base62 in the URI decodes to. */
	uint8_t gid[16];
	bool    has_gid;
	char    name[128];
	char    artist[128];
	char    album[128];
	/* Cover art as full i.scdn.co URLs: small (64px) for list rows, large
	 * (300px) for the now-playing screen. Empty when the album carries no
	 * images. */
	char    art_small[80];
	char    art_large[80];
	int     duration_ms;
	sp_file files[SP_MAX_FILES];
	int     file_count;
	/* What the detail views need to follow a track to its album and its
	 * first artist: the metadata names them only by gid, so these are built
	 * from it. Empty when the track does not carry them. The rest comes from
	 * the same Track (and the Album embedded in it), so no request is added.
	 * Track.explicit is absent from the wire when false; year is 0 when
	 * unknown. */
	char    album_uri[SP_URI_MAX];
	char    artist_uri[SP_URI_MAX];
	int     track_number;
	int     disc_number;
	bool    is_explicit;
	int     year;
} sp_track;

/* Album types, from metadata.proto's Album.Type. 0 when the album does not
 * say. */
#define SP_ALBUM_ALBUM       1
#define SP_ALBUM_SINGLE      2
#define SP_ALBUM_COMPILATION 3
#define SP_ALBUM_EP          4

/* As many as a list shows (SEARCH_LIMIT in main.c); the rest are counted in
 * total_tracks but not kept. */
#define SP_ALBUM_MAX_TRACKS  24
#define SP_ARTIST_MAX_TRACKS 24

/* An album's header and its track URIs in disc order. The tracks come back
 * as bare gids, so naming them is a second request, a metadata batch. */
typedef struct {
	char uri[SP_URI_MAX];
	char name[128];
	char artist[128];             /* the first of Album.artist */
	char artist_uri[SP_URI_MAX];
	char label[64];
	int  year;                    /* 0 when unknown */
	int  type;                    /* SP_ALBUM_*, 0 when unknown */
	char art_small[80];
	char art_large[80];
	int  total_tracks;            /* across every disc, kept or not */
	int  track_count;
	char track_uris[SP_ALBUM_MAX_TRACKS][SP_URI_MAX];
} sp_album;

/* An artist's name, portrait and popular tracks. The service sends one
 * TopTracks group already chosen for the account's country; when several
 * come, the one matching `country` wins, else the first. */
typedef struct {
	char uri[SP_URI_MAX];
	char name[128];
	char art_small[80];
	char art_large[80];
	int  track_count;
	char track_uris[SP_ARTIST_MAX_TRACKS][SP_URI_MAX];
} sp_artist;

/* One playlist as the home tab shows it. `art` is an i.scdn.co URL (the
 * 300px cover) or empty: covers Spotify generates live on other hosts
 * (blend-playlist-covers.spotifycdn.com, lexicon-assets...) that neither the
 * CA bundle nor the thumbnail session reach, so they read as "no cover".
 * `name` may be empty: some playlists carry none. `length` is the track
 * count the rootlist reports, 0 when unknown. */
typedef struct {
	char uri[SP_URI_MAX];
	char name[128];
	char art[80];
	int  length;
} sp_playlist_ref;

/* Items asked for per request: a page of a long playlist, named afterwards
 * by one metadata batch (the batch takes this many at once). */
#define SP_PLAYLIST_PAGE 50

/* A playlist's header and the track URIs among one range of its items.
 * Episodes and local files are left out of track_uris and counted in
 * `skipped`; `items` is every item the range held, so the next page starts
 * at from + items. total_tracks is what the service reports for the whole
 * list, all kinds included. Probed (probe_meta step 15) on
 * playlist4_external.proto: SelectedListContent { length 2, attributes 3,
 * contents 5 }.
 *
 * `live`: not a list but a live session another service plays (the AI DJ),
 * marked by `lexicon_*` format attributes. It holds no tracks and this
 * client cannot play it, so it is never shown. */
typedef struct {
	char uri[SP_URI_MAX];
	char name[128];
	char art[80];
	int  total_tracks;
	int  items;
	int  skipped;
	bool live;
	int  track_count;
	char track_uris[SP_PLAYLIST_PAGE][SP_URI_MAX];
} sp_playlist_info;

/* Which spclient host to use. Falls back to the well-known name when apresolve
 * has nothing to say, which is what librespot does too. */
int  sp_resolve(char *host, size_t hostlen, char *err, size_t errlen);

/* One track's metadata, via the extended-metadata batch service. */
int  sp_track_metadata(const char *host, const char *token, const char *track_uri,
                       sp_track *out, char *err, size_t errlen);

/* Several at once. The service takes a repeated entity_request, so a screenful
 * of tracks costs one round trip instead of one each -- which on this console
 * is the difference between a list that fills instantly and one that crawls.
 * Returns how many were filled in, or -1. They come back in the order asked
 * (minus any the service left out), although the service itself answers
 * shuffled: see sp_parse_track_batch. */
int  sp_track_metadata_batch(const char *host, const char *token,
                             const char *const *uris, int n,
                             sp_track *out, char *err, size_t errlen);

/* Same batch over a keep-alive session: chunk after chunk of a long list
 * pays one handshake total instead of one each. */
int  sp_track_metadata_batch_s(http_session *sess, const char *token,
                               const char *const *uris, int n,
                               sp_track *out, char *err, size_t errlen);

/* An album or an artist through extended-metadata, over a keep-alive session
 * because the caller names the tracks right after with
 * sp_track_metadata_batch_s on the same link. `country` may be NULL or
 * empty. Both return 0 or -1. An artist answer runs to tens of KB (the
 * biography and every album it appears on come along), which the default
 * reply limit covers. */
int  sp_album_metadata(http_session *sess, const char *token,
                       const char *album_uri, sp_album *out,
                       char *err, size_t errlen);
int  sp_artist_metadata(http_session *sess, const char *token,
                        const char *artist_uri, const char *country,
                        sp_artist *out, char *err, size_t errlen);

/* Searching the catalogue. Needs no library, which is what makes a fresh
 * account usable: the answer is a list of track URIs, which
 * sp_track_metadata_batch then turns into names.
 * `query` is plain text; spaces become '+'. */
int  sp_search_tracks(const char *host, const char *token, const char *query,
                      char uris[][64], int max, char *err, size_t errlen);

/* Recommendations seeded from one track. Unlike search, this answers with the
 * titles and artists inline, so no metadata round trip is needed at all. */
int  sp_radio(const char *host, const char *token, const char *seed_uri,
              int count, sp_track *out, int max, char *err, size_t errlen);

/* The user's playlist folder, decorated: one request brings every entry's
 * name, cover and length along in meta_items, where naming each playlist on
 * its own would be one request per row. Only playlists are kept, folders
 * flattened, live sessions (the AI DJ) left out. `total` (may be NULL) gets the entries the rootlist holds.
 * Returns how many were kept (up to `max`), or -1. An empty rootlist is a
 * valid answer, not an error. `username` is the canonical one from the
 * access point; it is percent-encoded into the path here. */
int  sp_rootlist_s(http_session *sess, const char *token, const char *username,
                   sp_playlist_ref *out, int max, int *total,
                   char *err, size_t errlen);

/* The playlists the account played last, most recent first and without
 * repeats, from the playback history the desktop client reads
 * (/recently-played/v3, JSON). Other contexts (albums, artists, tracks) are
 * left out. Returns how many URIs were kept, or -1. */
int  sp_recent_playlists_s(http_session *sess, const char *token,
                           const char *username, char uris[][SP_URI_MAX],
                           int max, char *err, size_t errlen);

/* One playlist's header and `max_tracks` items from item `from` (at most
 * SP_PLAYLIST_PAGE; 0 asks for the header alone, a few hundred bytes). The
 * range is asked of the service, so each page of a list of thousands costs
 * what a short list does. `uri` is the full spotify:playlist: URI. Returns
 * 0 or -1. */
int  sp_playlist_s(http_session *sess, const char *token, const char *uri,
                   int from, int max_tracks, sp_playlist_info *out,
                   char *err, size_t errlen);

/* Liked Songs. This one answers JSON, not protobuf, and carries the whole
 * first page inline. An empty page means an empty library, not a failure --
 * which is worth knowing, because that is what it looked like before this
 * account had anything saved. */
int  sp_liked(const char *host, const char *token, const char *username,
              char uris[][64], int max, char *err, size_t errlen);

/* Where the CDN is serving one audio file from. Up to three signed URLs, each
 * with an expiry; any of them serves the same bytes. */
#define SP_MAX_CDN 3

typedef struct {
	char urls[SP_MAX_CDN][512];
	int  count;
	int  result;                 /* 0 = CDN, 1 = storage, 3 = restricted */
} sp_storage;

/* `file_id` is the 20 raw bytes from sp_file. */
int  sp_storage_resolve(const char *host, const char *token,
                        const uint8_t file_id[20], sp_storage *out,
                        char *err, size_t errlen);

/* Picks the best file this client can actually play, or NULL. */
const sp_file *sp_pick_file(const sp_track *t);
const char *sp_format_name(int format);

/* The user's profile image URL, from the Web API's /v1/me (the one service
 * that lives on api.spotify.com rather than spclient). Only i.scdn.co URLs
 * are accepted: the hero art buffer is 80 chars and the CA bundle only
 * covers Spotify's own CDN, so a lookaside avatar is reported as "none"
 * rather than half-supported. Returns 0, or -1 when the account has no
 * usable image. On HTTP 429, `retry_after` (when given) receives the
 * server's Retry-After seconds so the caller waits on its clock. */
int  sp_user_image(const char *token, char *url, size_t urllen,
                   char *err, size_t errlen, int *retry_after);

/* Adds or removes a track in the user's library ("Me gusta"): PUT/DELETE
 * /v1/me/tracks?ids=<id>. Needs the user-library-modify scope, which tokens
 * paired before it was added do not carry -- a 403 means re-pair. On 429
 * `retry_after` gets the server's seconds, like sp_user_image. */
int  sp_library_modify(const char *token, const char *track_id, bool save,
                       char *err, size_t errlen, int *retry_after);

#endif
