/* Result thumbnails (docs/STYLES.md 5.3 and 9).
 *
 * One worker thread fetches the tracks' small covers (art_small, 64 px)
 * over a single keep-alive connection to i.scdn.co -- without it every
 * thumbnail would pay a ~3 s TLS handshake -- visible rows first, then the
 * next RESULTS_PER_PAGE, nothing further until the list scrolls there. It
 * decodes and tiles each image into a static buffer; thumbs_poll, on the
 * main thread, copies it into one of a fixed pool of 64x64 textures made
 * once at start-up. No allocation per thumbnail, and no citro3d call off
 * the main thread.
 *
 * thumbs_stop cancels the worker: call it before playback, so it does not
 * compete with the audio download, and when leaving the results. It does
 * not wait: the request in flight, if any, ends on its own and the worker
 * is reaped afterwards; thumbs_stopping says when it is gone. Only
 * thumbs_exit waits for it. */
#ifndef THUMBS_H
#define THUMBS_H

#include "spclient.h"

#include <citro2d.h>

#include <stdbool.h>

/* Thumbnails per list: the search limit. */
#define THUMBS_MAX 24

/* After ui_init. Without the linear memory for the pool the app runs on
 * with placeholders. */
bool thumbs_init(void);
/* Stops the worker and frees the pool. Before ui_exit. */
void thumbs_exit(void);

/* A new (or the same) list of results. Covers already fetched for the same
 * position and URL are kept, so coming back from the player costs nothing.
 * Starts the worker when there is something to fetch. */
void thumbs_set_tracks(const sp_track *tracks, int count);
/* The first visible row: the fetch priority follows the scroll. */
void thumbs_set_window(int first);

/* Uploads finished thumbnails. Once per frame on the main thread; true when
 * one of them changed what the list shows. */
bool thumbs_poll(void);

/* The thumbnail of track `index`, when it is ready. */
bool thumbs_image(int index, C2D_Image *out);

/* Cancels pending fetches and asks the worker to stop, without waiting. */
void thumbs_stop(void);
/* True while a stopped worker is still finishing its request in flight. */
bool thumbs_stopping(void);
/* Stops the worker and waits for it to return. Only when the network is
 * going away (leaving the tabs, before tls_global_exit and socExit): a
 * worker left running there would use a freed TLS context and socket. */
void thumbs_wait(void);

#endif
