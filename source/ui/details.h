/* The detail screens' data: one document per open level, the per-tab
 * stacks of detail_nav.h, and the background loader for albums, artists
 * and playlists (STYLES.md 7.9-7.11).
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
#include "offline_logic.h"
#include "spclient.h"
#include "tabs_logic.h"

#include <stdbool.h>
#include <stdint.h>

/* A list's worth: SEARCH_LIMIT, THUMBS_MAX and SP_ALBUM_MAX_TRACKS. */
#define DETAIL_MAX_TRACKS 24
/* A playlist loads page after page (SP_PLAYLIST_PAGE) as its list nears
 * the end, up to this many tracks, in a buffer of its own (~1 KB each)
 * made when it opens and let go when its document is reused. */
#define DETAIL_PLAYLIST_MAX 500
/* The next page is asked for when the list shows a row this close to the
 * last one loaded. */
#define DETAIL_MORE_AHEAD 8

/* DETAIL_DOWNLOADS (the Home tab's Downloads) and DETAIL_OFFLINE_COLLECTION
 * (an album or playlist on the card) come from the downloads index, with
 * no network: READY as they open, and filled again when the downloads
 * change. */
typedef enum {
	DETAIL_TRACK, DETAIL_ALBUM, DETAIL_ARTIST, DETAIL_PLAYLIST,
	DETAIL_DOWNLOADS, DETAIL_OFFLINE_COLLECTION
} detail_kind;
typedef enum { DETAIL_LOADING, DETAIL_READY, DETAIL_FAILED } detail_state;

typedef struct {
	detail_kind  kind;
	detail_state state;
	char uri[SP_URI_MAX];         /* the album's, artist's or playlist's;
	                               * the track's */
	/* The header, for the top screen. */
	char title[128];              /* track, album, artist or playlist */
	char subtitle[128];           /* the artist of a track or album */
	char art[80];                 /* cover or portrait, large */
	/* The same picture, small: the list's thumbnail of it, shown scaled
	 * up until `art` arrives (artwork_set_preview). May be empty. */
	char art_small[80];
	char label[64];
	int  year, album_type, total_tracks;
	/* DETAIL_ARTIST: the start of the biography (sp_bio_clean, "" when
	 * none), the monthly listeners (-1 while unknown) and how many
	 * releases its discography holds. */
	char bio[SP_ARTIST_BIO_MAX];
	long listeners;
	int  release_count;
	/* DETAIL_TRACK: a copy of the list it came from; the track shown is
	 * tracks[selected] and "Play" queues the whole list. DETAIL_ALBUM,
	 * DETAIL_ARTIST and DETAIL_PLAYLIST: their tracks. `tracks` points at
	 * inline_tracks, or at a playlist's own buffer; it never moves while
	 * the document is in use. */
	sp_track *tracks;
	int  count, capacity;
	/* DETAIL_TRACK: opened from a single track rather than from a list (a
	 * search result, or the radio playing): "Play" starts a radio from it
	 * instead of queueing the list. */
	bool single;
	sp_track inline_tracks[DETAIL_MAX_TRACKS];
	sp_track *owned;
	/* DETAIL_PLAYLIST paging: the first item not read yet; whether items
	 * are left and fit; a page asked for or on its way; after a page
	 * failed, not again before this osGetTime(). */
	int  next_from;
	bool more, loading_more;
	uint64_t retry_at;
	/* A line for the card: where it was left ("title - artist"), or "". */
	char resume[128];
	/* The offline kinds: what each row is (offline_logic.h), in a buffer
	 * of their own as long as `tracks`; how many rows are collections and
	 * how many tracks (Downloads); whether the collection is an album, and
	 * the bytes of its tracks on the card; the downloads generation it was
	 * filled at. */
	track_row *rows;
	int  coll_rows, track_rows;
	bool offline_album;
	uint64_t bytes;
	unsigned dl_generation;
	/* The view's state, so going back finds the list as it was left. */
	int  selected, first;
	char error[192];
	/* Bumped every time the document is (re)used, so a load that finishes
	 * late cannot fill a level opened since. */
	unsigned serial;
	/* Not an open detail but a search result's card, built from the row
	 * (view_results_preview): no tracks, no year, no counts to tell, so
	 * the card leaves those lines out (STYLES.md 7.12). */
	bool preview;
} detail_doc;

/* True while `tab` shows a detail instead of its own view. */
bool details_active(app_tab tab);
/* The detail on screen in `tab`, or NULL. The view may change `selected`
 * and `first`; everything else is the loader's. */
detail_doc *details_top(app_tab tab);

/* Opens a track detail on `tab`'s stack from list[index]. The list is
 * copied (at most DETAIL_MAX_TRACKS, keeping `index` inside). `single`:
 * the track stands alone, and the list is only where it was found. */
void details_open_track(app_tab tab, const sp_track *list, int count,
                        int index, bool single);
/* Opens an album, artist or playlist by URI, in DETAIL_LOADING;
 * details_poll starts the load. Ignored for an empty URI. A playlist loads
 * page by page (details_follow), episodes and local files left out;
 * total_tracks counts every item it holds. SP_LIKED_URI opens Liked Songs
 * the same way. A playlist opens at its first track. */
void details_open_album(app_tab tab, const char *uri);
void details_open_artist(app_tab tab, const char *uri);
void details_open_playlist(app_tab tab, const char *uri);
/* The downloads, from the index: the Home tab's Downloads, and an album or
 * playlist on the card by URI. No network. */
void details_open_downloads(app_tab tab);
void details_open_offline_collection(app_tab tab, const char *uri);

/* Pops one level of `tab`; a load for it is asked to stop. False when the
 * stack was already empty, so B does what the tab's own view does. */
bool details_back(app_tab tab);
/* Asks for the next page of the playlist on top of `tab` when its list
 * (selected and first, as the view wrote them) nears the end of what is
 * loaded. Once per frame while the detail shows; details_poll runs it. */
void details_follow(app_tab tab);
/* Loads a failed album or artist on top of `tab` again. */
void details_retry(app_tab tab);
/* Empties `tab`'s stack: a new search replaces what its details came from. */
void details_clear(app_tab tab);

/* Once per frame, next to search_poll: reaps a finished load into its
 * document and starts the next one waiting, `active` tab first. The
 * credentials are copied into the worker's job, which clears them when it
 * is reaped. True when a document on top of a stack changed. `username`
 * names the account's Liked Songs. */
bool details_poll(app_tab active, const char *host, const char *token,
                  const char *username);

/* Leaving the tabs: stops the load and waits for it (call with the TLS
 * abort on, before thumbs_wait), then empties both stacks. */
void details_stop(void);

#endif
