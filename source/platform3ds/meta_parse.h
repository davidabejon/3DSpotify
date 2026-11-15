/* Reading Spotify's metadata messages (metadata.proto) into this client's
 * structs.
 *
 * Kept apart from spclient.c, which does the requests, so that it depends on
 * nothing but proto3ds and the host tests can feed it messages built with
 * pb_*. Field numbers were checked against what the service really sends
 * (probe_meta, steps 11-13): a wrong one does not fail, it reads nothing.
 *
 *   Track  { gid 1, name 2, album 3, artist 4 (repeated), number 5 (sint32),
 *            disc_number 6 (sint32), duration 7 (sint32), explicit 9,
 *            file 12 (repeated) }
 *   Album  { gid 1, name 2, artist 3 (repeated), type 4, label 5, date 6,
 *            cover 9 (repeated Image), disc 11 (repeated),
 *            cover_group 17 }
 *   Artist { gid 1, name 2, top_track 4 (repeated), portrait 11 (repeated
 *            Image), portrait_group 17 }
 *   Date { year 1 (sint32) }   Disc { number 1, track 3 (repeated) }
 *   TopTracks { country 1, track 2 (repeated) }
 *   ImageGroup { image 1 (repeated) }   Image { file_id 1, size 2 }
 *
 * The tracks inside a Disc or a TopTracks carry only their gid.
 */
#ifndef META_PARSE_H
#define META_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto3ds.h"
#include "spclient.h"

/* extension_kind.proto. */
#define EXT_KIND_ARTIST_V4 8
#define EXT_KIND_ALBUM_V4  9
#define EXT_KIND_TRACK_V4  10

/* The one entity in an extended-metadata answer:
 * BatchedExtensionResponse.extended_metadata (2) -> EntityExtensionDataArray
 * .extension_data (3) -> EntityExtensionData.extension_data (3, an Any)
 * -> value (2). */
bool sp_parse_entity(const uint8_t *body, size_t len, pbv *entity);

/* Fills every field of `out` the Track carries, except `uri`, which the
 * caller knows better (the batch reads it from the response envelope). Does
 * not clear `out` first: the caller does. */
void sp_parse_track(const pbv *track, sp_track *out);

/* A batch answer for `n` track URIs, into out[0..n), in the order asked.
 *
 * The service does NOT answer in that order (an album's 14 tracks came back
 * as 14, 5, 12, 3, ...), so each EntityExtensionData is placed by its
 * entity_uri (2). An entry whose URI matches nothing asked takes the first
 * slot still free; tracks the answer leaves out are dropped and the rest
 * close ranks, keeping their relative order. Returns how many are filled. */
int sp_parse_track_batch(const uint8_t *body, size_t len,
                         const char *const *uris, int n, sp_track *out);

/* Clear `out`, then fill it. False when the message carries no gid. */
bool sp_parse_album(const pbv *album, sp_album *out);
bool sp_parse_artist(const pbv *artist, const char *country, sp_artist *out);

/* ------------------------------------------------------------ playlists
 *
 * playlist4_external.proto, checked against the wire (probe_meta step 15):
 *   SelectedListContent { length 2, attributes 3, contents 5 }
 *   ListItems { items 3 (repeated), meta_items 4 (repeated, parallel) }
 *   Item { uri 1 }   MetaItem { attributes 2, length 3 }
 *   ListAttributes { name 1, picture 3 (20-byte image id),
 *                    picture_size 13 (repeated { target_name 1, url 2 }) }
 */

/* True for spotify:track: URIs, the only items this client plays: a
 * playlist may also hold spotify:episode: and spotify:local: entries. */
bool sp_is_track_uri(const char *uri);

/* Whether a ListAttributes is a live session rather than a list: any
 * format attribute (12, { key 1, value 2 }) whose key starts with
 * "lexicon_", which is how the AI DJ comes. Those have no tracks to read
 * and only the official client's session service plays them. */
bool sp_list_is_live(const pbv *attrs);

/* A ListAttributes' name and cover. The cover is the `picture` id as an
 * i.scdn.co URL, else a picture_size URL on i.scdn.co ("small" first) that
 * fits `artlen`; anything else leaves `art` empty. A name cut to fit stays
 * whole UTF-8. Either output may be NULL. */
void sp_parse_list_attributes(const pbv *attrs, char *name, size_t namelen,
                              char *art, size_t artlen);

/* A decorated rootlist answer. Keeps spotify:playlist: entries only (folder
 * markers spotify:start-group:/end-group: and anything else are skipped, so
 * folders come out flattened) and not live sessions (sp_list_is_live), each
 * with its meta_item's name, cover and length. `total` (may be NULL) gets SelectedListContent.length. Returns
 * how many were kept, at most `max`. */
int  sp_parse_rootlist(const uint8_t *body, size_t len, sp_playlist_ref *out,
                       int max, int *total);

/* A playlist answer (header and a range of items) into `out`, which is
 * cleared first; `uri` is copied in as given. Track URIs go to
 * track_uris (at most SP_PLAYLIST_PAGE and at most `max_tracks`); other
 * items count in `skipped`. Reading stops once the page is full: `items`
 * says how many items were read, for where the next page starts. `live`
 * marks a session (sp_list_is_live). */
void sp_parse_playlist(const uint8_t *body, size_t len, const char *uri,
                       int max_tracks, sp_playlist_info *out);

/* The /recently-played/v3 JSON: {"playContexts":[{"uri":"spotify:...",
 * "lastPlayedTime":..., "lastPlayedTrackUri":...}, ...]}. Keeps the
 * spotify:playlist: contexts in the order sent (most recent first),
 * dropping repeats. Returns how many, at most `max`. */
int  sp_parse_recent_playlists(const char *json, size_t len,
                               char uris[][SP_URI_MAX], int max);

#endif
