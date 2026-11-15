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

/* A new (or the same) list of cover URLs (art_small for tracks, the cover
 * of a playlist). Covers already fetched for the same position and URL are
 * kept, so coming back from the player costs nothing. URLs off i.scdn.co,
 * or empty, show the placeholder. Starts the worker when there is
 * something to fetch. */
void thumbs_set_urls(const char *const *urls, int count);
/* The first visible row: the fetch priority follows the scroll. */
void thumbs_set_window(int first);

/* Uploads finished thumbnails. Once per frame on the main thread; true when
 * one of them changed what the list shows. */
bool thumbs_poll(void);

/* The thumbnail of track `index`, when it is ready. */
bool thumbs_image(int index, C2D_Image *out);

/* --- Clients ------------------------------------------------------------
 *
 * The pool serves one client at a time: a track list or a tile grid, the
 * one on screen, which claims it from its view's update (thumbs_client_
 * frame), so a widget shown under another never takes the covers from it.
 * Each widget embeds a client; the URLs point into the widget's own data,
 * which must stay valid while it is shown. Main thread only. */
typedef struct {
	const char *urls[THUMBS_MAX];
	int  count;
	int  first;            /* the first visible item, for the priority */
	/* The pool holds this client's covers, so its images are this
	 * client's. Until then the placeholder shows. */
	bool synced;
	int  serial;           /* bumped when one of its covers arrives */
} thumbs_client;

/* New content (count 0 empties it): handed to the pool by the next claim. */
void thumbs_client_set(thumbs_client *c, const char *const *urls, int count);
/* The first visible item moved: the fetch priority follows it. */
void thumbs_client_window(thumbs_client *c, int first);
/* Once per frame from the update of the view showing the widget: claims
 * the pool if another client had it, and uploads the covers that arrived
 * (on the main thread, never while drawing). */
void thumbs_client_frame(thumbs_client *c);
/* Gives the pool to `c` only if no client holds it: a widget that is ready
 * before it is on screen starts its covers early, without taking them from
 * one on screen. */
void thumbs_client_offer(thumbs_client *c);
/* The widget goes away. */
void thumbs_client_release(thumbs_client *c);
/* Item `index`'s cover, when the pool is this client's and it is ready. */
bool thumbs_client_image(const thumbs_client *c, int index, C2D_Image *out);
/* Whether covers may download (STYLES.md 9: not while a track prepares).
 * Disallowing asks the worker to stop without waiting for it
 * (thumbs_stop); what it fetched stays on screen. */
void thumbs_allow(bool allow);

/* Cancels pending fetches and asks the worker to stop, without waiting. */
void thumbs_stop(void);
/* True while a stopped worker is still finishing its request in flight. */
bool thumbs_stopping(void);
/* Stops the worker and waits for it to return. Only when the network is
 * going away (leaving the tabs, before tls_global_exit and socExit): a
 * worker left running there would use a freed TLS context and socket. */
void thumbs_wait(void);

#endif
