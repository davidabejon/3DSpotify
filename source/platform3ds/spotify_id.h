/* Spotify ids: the 16-byte gid the metadata carries, and the 22-character
 * base62 form a URI ends in.
 *
 * The metadata names an album or an artist only by gid, while every request
 * names it by URI, so following a track to its album needs this conversion.
 * Pure C with no 3DS dependencies, so the host tests cover it.
 *
 * The gid is a big-endian 128-bit number; base62 uses digits, then lowercase,
 * then uppercase, and always 22 characters (62^22 > 2^128), with leading
 * zeros kept. Example: 3n3Ppam7vgaVa1iaRUc9Lp <->
 * 6ecac90c03f74dcba9e21b408e14719f.
 */
#ifndef SPOTIFY_ID_H
#define SPOTIFY_ID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SP_ID_LEN 22

/* "spotify:artist:" plus 22 characters plus the NUL is 38: a URI built here
 * always fits in this, whatever its kind. */
#define SP_URI_MAX 40

typedef enum {
	SP_KIND_TRACK,
	SP_KIND_ALBUM,
	SP_KIND_ARTIST,
} sp_kind;

void sp_id_from_gid(const uint8_t gid[16], char out[SP_ID_LEN + 1]);

/* False for anything but 22 base62 characters that fit in 128 bits; `gid`
 * is zeroed then. */
bool sp_gid_from_id(const char *id, uint8_t gid[16]);

/* "spotify:<kind>:<id>". False, with `out` empty, when it does not fit. */
bool sp_uri_from_gid(sp_kind kind, const uint8_t gid[16], char *out, size_t len);

#endif
