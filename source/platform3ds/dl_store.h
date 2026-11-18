/* Downloads on the SD card: the index, the audio files and their covers.
 *
 * Everything lives under downloads/ beside the config (cfg_dir):
 *
 *   downloads/index.dat       the index (downloads_logic.h), written whole
 *   downloads/index.dat.tmp   only while it is being written
 *   downloads/t/<id>.enc      a track's audio as the CDN serves it: encrypted
 *   downloads/t/<id>.part     audio still downloading; its size is where to
 *                             resume
 *   downloads/a/<hash>.img    covers, by URL
 *
 * No UI here. Functions that can fail return false or -1 with `err` set.
 */
#ifndef DL_STORE_H
#define DL_STORE_H

#include "downloads_logic.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Creates downloads/ and its folders when missing. */
bool dl_store_init(void);

/* Reads the index into `out`. No index yet is an empty one, and returns 0.
 * Returns -1 when there is one this build cannot read (another version, or
 * unreadable): the caller must then leave it alone, not save over it. */
int  dl_store_load(dl_index *out, char *err, size_t errlen);

/* Writes the index whole: to index.dat.tmp, then in place of index.dat. A
 * cut in the middle leaves the old index or the new one, never half of
 * one. Blocks on the SD card for a moment: not for the main loop. */
int  dl_store_save(const dl_index *idx, char *err, size_t errlen);

void dl_store_track_path(const char *uri, bool part, char *out, size_t outlen);
void dl_store_art_path(const char *url, char *out, size_t outlen);
bool dl_store_art_has(const char *url);
/* The cover's bytes (malloc'd; the caller frees), or NULL. */
void *dl_store_art_read(const char *url, size_t *len);
bool dl_store_art_write(const char *url, const void *data, size_t len);
/* Deletes the track's audio, whole or partial. True when nothing is left. */
bool dl_store_remove_track(const char *uri);

uint64_t dl_store_free_bytes(void);
/* What the downloaded audio takes, from the sizes in the index. */
uint64_t dl_store_used_bytes(const dl_index *idx);

/* What a .part holds so far (0 when there is none): where to resume. */
uint32_t dl_store_part_size(const char *uri);
/* The .part is whole: it becomes the .enc. */
bool dl_store_finish_track(const char *uri);
/* Deletes what t/ holds that `idx` does not name: files of tracks removed
 * while the app was off, or written by a download the index never saw. */
void dl_store_sweep(const dl_index *idx);

/* Audio keys are only ever stored wrapped, under a key derived from this
 * console and the index's salt. dl_store_ensure_salt gives the index one
 * when it has none (the index must be saved after). Wrapping and
 * unwrapping take the salt itself, so a worker can do them with a copy.
 * Both return false on failure; unwrapping fails too when the key was
 * wrapped on another console or for another track. `key` is the caller's
 * to wipe. */
bool dl_store_ensure_salt(dl_index *idx);
bool dl_store_wrap_key(const uint8_t salt[DL_SALT_LEN], const char *uri,
                       const uint8_t key[16], uint8_t out[DL_WRAPPED_LEN]);
bool dl_store_unwrap_key(const uint8_t salt[DL_SALT_LEN], const char *uri,
                         const uint8_t in[DL_WRAPPED_LEN], uint8_t key[16]);

#endif
