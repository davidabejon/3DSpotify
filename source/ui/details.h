/* The detail screens' data: one document per open level, the per-tab
 * stacks of detail_nav.h, and the background loader for albums and artists
 * (docs/plan_vistas_detalle.md 3.1-3.3).
 *
 * A track detail needs no network: it is a copy of the list it was opened
 * from, so "Play" can use that list as the queue however the original
 * changes. Albums and artists open in DETAIL_LOADING and one worker fetches
 * them, over one keep-alive connection for the entity and the batch that
 * names its tracks. Documents stay in memory while their level is on a
 * stack, so going back costs no request (a new connection is a TLS
 * handshake of seconds on this console).
 *
 * Opening a detail while another loads asks the running load to stop
 * without waiting for it, as the player does with a skipped track: the
 * request in flight ends on its own and is reaped by details_poll, which
 * then starts whichever document is still waiting, the active tab's first.
 * Only details_stop waits.
 *
 * Main thread only, except the worker, which touches nothing but its own
 * job. */
#ifndef DETAILS_H
#define DETAILS_H

#include "detail_nav.h"
#include "spclient.h"
#include "tabs_logic.h"

#include <stdbool.h>

/* A list's worth: SEARCH_LIMIT, THUMBS_MAX and SP_ALBUM_MAX_TRACKS. */
#define DETAIL_MAX_TRACKS 24

typedef enum { DETAIL_TRACK, DETAIL_ALBUM, DETAIL_ARTIST } detail_kind;
typedef enum { DETAIL_LOADING, DETAIL_READY, DETAIL_FAILED } detail_state;

typedef struct {
	detail_kind  kind;
	detail_state state;
	char uri[SP_URI_MAX];         /* the album's or artist's; the track's */
	/* The header, for the top screen. */
	char title[128];              /* track, album or artist */
	char subtitle[128];           /* the artist of a track or album */
	char art[80];                 /* cover or portrait, large */
	char label[64];
	int  year, album_type, total_tracks;
	/* DETAIL_TRACK: a copy of the list it came from; the track shown is
	 * tracks[selected] and "Play" queues the whole list. DETAIL_ALBUM and
	 * DETAIL_ARTIST: their tracks. */
	sp_track tracks[DETAIL_MAX_TRACKS];
	int  count;
	/* The view's state, so going back finds the list as it was left. */
	int  selected, first;
	char error[192];
	/* Bumped every time the document is (re)used, so a load that finishes
	 * late cannot fill a level opened since. */
	unsigned serial;
} detail_doc;

/* True while `tab` shows a detail instead of its own view. */
bool details_active(app_tab tab);
/* The detail on screen in `tab`, or NULL. The view may change `selected`
 * and `first`; everything else is the loader's. */
detail_doc *details_top(app_tab tab);

/* Opens a track detail on `tab`'s stack from list[index]. The list is
 * copied (at most DETAIL_MAX_TRACKS, keeping `index` inside). */
void details_open_track(app_tab tab, const sp_track *list, int count,
                        int index);
/* Opens an album or artist by URI, in DETAIL_LOADING; details_poll starts
 * the load. Ignored for an empty URI. */
void details_open_album(app_tab tab, const char *uri);
void details_open_artist(app_tab tab, const char *uri);

/* Pops one level of `tab`; a load for it is asked to stop. False when the
 * stack was already empty, so B does what the tab's own view does. */
bool details_back(app_tab tab);
/* Loads a failed album or artist on top of `tab` again. */
void details_retry(app_tab tab);
/* Empties `tab`'s stack: a new search replaces what its details came from. */
void details_clear(app_tab tab);

/* Once per frame, next to search_poll: reaps a finished load into its
 * document and starts the next one waiting, `active` tab first. The
 * credentials are copied into the worker's job, which clears them when it
 * is reaped. True when a document on top of a stack changed. */
bool details_poll(app_tab active, const char *host, const char *token);

/* Leaving the tabs: stops the load and waits for it (call with the TLS
 * abort on, before thumbs_wait), then empties both stacks. */
void details_stop(void);

#endif
