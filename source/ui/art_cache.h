/* The covers downloaded lately, as their compressed bytes, keyed by URL.
 *
 * The thumbnails (thumbs.h) and the top screen's cover (artwork.h) often
 * want the same file: a playlist's tile and its preview card both show its
 * 300 px cover. Whichever downloads it first leaves the bytes here, and the
 * other decodes them without going back to the network, where each new
 * connection costs a TLS handshake of seconds.
 *
 * At most ART_CACHE_SLOTS entries and ART_CACHE_BUDGET bytes in all; the
 * least recently used goes first. Entries over ART_CACHE_ENTRY_MAX are not
 * kept. Callable from any thread: one lock guards it, held only to copy. */
#ifndef ART_CACHE_H
#define ART_CACHE_H

#include <stdbool.h>
#include <stddef.h>

/* A home grid's worth of covers, and a list's, with room to spare. */
#define ART_CACHE_SLOTS 32
#define ART_CACHE_BUDGET (1024 * 1024)
#define ART_CACHE_ENTRY_MAX (128 * 1024)

/* Before any other call, before the workers start. */
void art_cache_init(void);
/* Frees everything; no worker may be using the cache any more. */
void art_cache_exit(void);

/* Keeps a copy of `len` bytes for `url`, replacing what it had for it.
 * Silently does nothing when it cannot (too big, out of memory). */
void art_cache_put(const char *url, const void *data, size_t len);

/* Whether the bytes for `url` are here: showing it costs no download. */
bool art_cache_has(const char *url);

/* A malloc'd copy of the bytes for `url`, for the caller to free, or NULL.
 * A hit counts as a use. */
void *art_cache_get(const char *url, size_t *len);

#endif
