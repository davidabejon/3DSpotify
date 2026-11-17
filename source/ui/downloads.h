/* Downloads: the engine that keeps tracks on the SD card for later.
 *
 * The main thread owns the index (downloads_logic.h), the queue, the model
 * of the audio key allowance and every decision; one worker at a time does
 * the slow part of one job: a track (its key, then its file, then a check),
 * or an album or playlist resolved into tracks. The worker's job is a
 * global, never on a thread's stack, and the token it carries is cleared
 * when it is reaped.
 *
 * What a download keeps from the player (STYLES.md 9): while the player is
 * preparing a track or still downloading the one that plays, the engine
 * stops its request and starts none, and the player waits for the engine
 * to be quiet before preparing. A stopped download resumes from its .part
 * and, once its key came, without asking for another.
 *
 * Main thread only. */
#ifndef DOWNLOADS_H
#define DOWNLOADS_H

#include "ap3ds.h"
#include "downloads_logic.h"
#include "lang.h"
#include "spclient.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Logs in to an access point for an audio key: main.c's, which shares the
 * session (and its OAuth renewal) with the player. Called from the worker. */
typedef ap_session *(*downloads_ap_fn)(char *err, size_t errlen);

/* At start-up, after psInit: reads the index in the background. */
void downloads_init(void);
/* Closing: the last changes reach the card. */
void downloads_exit(void);

/* With a session: what the worker needs to reach Spotify. Copied. */
void downloads_start(const char *host, const char *token,
                     const char *username, downloads_ap_fn open_ap);
/* Leaving the tabs (with the TLS abort on, before thumbs_wait): stops the
 * worker, waits for it and forgets the session. The queue stays. */
void downloads_stop(void);

/* Once per frame, after the player: reaps a finished job, starts the next
 * one when it may. `player_busy` is dl_player_busy's. True when the index
 * changed. */
bool downloads_poll(bool player_busy);
/* A track is being downloaded: the player waits for it before preparing. */
bool downloads_busy(void);
/* The player asked for an audio key (`rc` 0, AP_KEY_REFUSED or -1 when it
 * never got that far): it counts against the same allowance. */
void downloads_note_player_key(int rc, int refused_code);

/* Queues a track the user asked for. Returns what to tell them:
 * STR_DL_ADDED or STR_DL_ALREADY when it is (or was) in, or why not:
 * STR_DL_NOT_READY (the index is loading, or there is no session),
 * STR_DL_UNREADABLE, STR_DL_OTHER_ACCOUNT, STR_DL_FULL. */
str_id downloads_add_track(const sp_track *t);
/* The same for the track that plays, with the key the player already has:
 * no key is asked for again. */
str_id downloads_add_track_keyed(const sp_track *t, const uint8_t key[16],
                                 const uint8_t file_id[20], int format);
/* Queues an album or a playlist (SP_LIKED_URI too): resolved into its
 * tracks in the background, then each queued. `name` and `art` show until
 * it is resolved. */
bool downloads_add_collection(dl_coll_kind kind, const char *uri,
                              const char *name, const char *art);

/* What the player needs to play a download from the card, instead of the
 * network: the file, its key as stored (wrapped) and the salt that opens
 * it, and the format. A copy, so the preparation thread can use it. */
typedef struct {
	char    path[340];
	uint8_t wrapped[DL_WRAPPED_LEN];
	uint8_t salt[DL_SALT_LEN];
	int     format;
} downloads_local;

/* True when `uri` is downloaded whole, with `out` filled. */
bool downloads_local_track(const char *uri, downloads_local *out);

/* Where a track is: false when it is not in the index. `pct` (may be NULL)
 * is the download's progress, while DL_ACTIVE. */
bool downloads_track_state(const char *uri, dl_state *state, int *pct);
/* The index, NULL while loading, unreadable, or another account's. */
const dl_index *downloads_index(void);
/* Bumped on every change, as home_feed_generation. */
unsigned downloads_generation(void);

/* A queued or downloading track or collection out of the queue, and its
 * files with it (the same as removing it). */
void downloads_cancel(const char *uri);
/* A failed track back in the queue. */
void downloads_retry(const char *uri);
/* A track or a collection off the card. A track a collection still holds
 * stays for it. */
void downloads_remove(const char *uri);
void downloads_set_paused(bool paused);
bool downloads_paused(void);
/* The queue stopped because the card is full. */
bool downloads_sd_full(void);

#endif
