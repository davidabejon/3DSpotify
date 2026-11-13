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

/* Clear `out`, then fill it. False when the message carries no gid. */
bool sp_parse_album(const pbv *album, sp_album *out);
bool sp_parse_artist(const pbv *artist, const char *country, sp_artist *out);

#endif
