/* The queue's background loads: the pages of a playing playlist that its
 * detail had not loaded yet, and a radio's batches of recommendations.
 *
 * It owns no queue. main.c decides what to load and when, and appends what
 * comes back; each load carries the caller's `tag` (the queue it was asked
 * for), so a load for a queue already replaced is recognised and dropped.
 * It is independent of the detail stacks: closing the playlist's detail
 * does not stop its queue.
 *
 * One load at a time, in a worker below the decoder. Main thread only,
 * except the worker, which touches nothing but its own job. */
#ifndef QUEUE_FEED_H
#define QUEUE_FEED_H

#include "spclient.h"

#include <stdbool.h>

/* Recommendations asked for at a time. */
#define QUEUE_RADIO_BATCH 25

typedef struct {
	unsigned tag;          /* as the load was started with */
	int count;             /* tracks, or -1 when it failed */
	sp_track *tracks;      /* valid until the next load starts */
	/* A playlist page: the items it read (episodes and local files
	 * included), to move `from` on by, and how many the playlist holds. */
	int items, total;
} queue_feed_result;

/* Nothing running and nothing waiting for queue_feed_poll: a load may
 * start. */
bool queue_feed_idle(void);

/* The page of playlist `uri` from item `from` (SP_PLAYLIST_PAGE items),
 * named. SP_LIKED_URI reads Liked Songs, whose whole list comes with the
 * first load of each tag and is kept for the next pages. False when it did
 * not start: not idle, or no thread. */
bool queue_feed_playlist(const char *host, const char *token,
                         const char *username, const char *uri, int from,
                         unsigned tag);
/* QUEUE_RADIO_BATCH recommendations seeded from track `seed_uri`, named
 * (the radio itself brings no cover nor audio files). False when it did
 * not start. */
bool queue_feed_radio(const char *host, const char *token,
                      const char *seed_uri, unsigned tag);

/* The load running is not wanted any more: it stops at its next step. */
void queue_feed_cancel(void);

/* Once per frame: reaps a finished load. True, once per load, with its
 * result; a cancelled load is reaped without one. */
bool queue_feed_poll(queue_feed_result *out);

/* Leaving the tabs: stops the load and waits for it (with the TLS abort
 * on), and forgets the credentials and Liked Songs. */
void queue_feed_stop(void);

#endif
