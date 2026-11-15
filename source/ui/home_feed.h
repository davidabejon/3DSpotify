/* The home tab's playlists, loaded in the background (home plan 3.5).
 *
 * One worker, over one keep-alive connection to spclient: the play history
 * and the decorated rootlist, merged by home_feed_logic.h (recent first,
 * then the rest of the folder), then the header of each recent playlist
 * the folder does not name. The worker's job and the list are globals,
 * never on a thread's stack; the token and the username are copied into
 * the job and cleared when it is reaped.
 *
 * It starts when the tabs open and loads once; "Retry" after a failure
 * loads again. Main thread only, except the worker, which touches nothing
 * but its job. */
#ifndef HOME_FEED_H
#define HOME_FEED_H

#include "spclient.h"

#include <stdbool.h>

/* As many as the tile grid shows covers for (THUMBS_MAX). */
#define HOME_MAX_PLAYLISTS 24

typedef enum {
	HOME_FEED_IDLE,        /* not started */
	HOME_FEED_LOADING,
	HOME_FEED_READY,       /* possibly with no playlist at all */
	HOME_FEED_FAILED
} home_feed_state;

/* Starts the load unless one ran or runs. The strings are copied. */
void home_feed_start(const char *host, const char *token,
                     const char *username);
/* Loads again after a failure. */
void home_feed_retry(const char *host, const char *token,
                     const char *username);
/* Once per frame, next to details_poll: reaps a finished load. True when
 * the state changed. */
bool home_feed_poll(void);
/* Leaving the tabs: stops the load and waits for it (call with the TLS
 * abort on, before thumbs_wait), and forgets what it had. */
void home_feed_stop(void);

home_feed_state home_feed_get_state(void);
/* The playlists, valid while READY. */
const sp_playlist_ref *home_feed_items(int *count);
/* How many playlists there are in all, which may exceed what is kept. */
int  home_feed_total(void);
/* Whether the order comes from the play history ("Recent") or only from
 * the folder ("Your playlists"): without a history the list is the
 * rootlist in its own order, which is not a recency. */
bool home_feed_is_recent(void);
/* What went wrong, while FAILED. */
const char *home_feed_error(void);

#endif
