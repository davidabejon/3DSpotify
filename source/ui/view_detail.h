/* The detail screens on the bottom screen (docs/plan_vistas_detalle.md 4):
 * a track, an album or an artist, whichever is on top of the active tab's
 * detail stack (details.h). The top screen belongs to top.h.
 *
 * Track: four buttons with the geometry of the Settings tab
 * (detail_logic.h) -- Play, its download, the artist and the album, the
 * last two disabled when the track does not name them -- and
 * "[A] Choose [B] Back". The focus starts on Play. The download button
 * says where the track is (downloads.h): "Download", "Queued",
 * "Downloading 42%", "Retry download", "Remove download", the last asking
 * for a second A while "Press A again to remove" shows.
 *
 * Album and artist: while loading, skeleton rows under a "Loading..."
 * bubble, B cancelling; if it failed, the notice with the reason and a
 * retry button; then the list of tracks (track_list.h) with
 * "[A] Play [X] Detail [Y] Shuffle/In order [B] Back": A, or tapping the
 * focused row, plays the list from that row; X opens that track's detail;
 * Y switches the order the lists play in, which the bar shows, and a bubble
 * confirms. An album's rows show their position, an artist's their cover
 * thumbnail. An album's or playlist's list has three rows under a button
 * that downloads it all, and then says how far that went; up from the
 * first row reaches it.
 *
 * Every call takes the document on screen. A different level starts with
 * no press, focus or bubble left over; the same level that finished
 * loading keeps what was going on. A list's focus and scroll are written
 * back to the document (`selected`, `first`), so coming back to it finds
 * it as it was left. Input follows STYLES.md 6 like the other views:
 * pressed states for at least ANIM_PRESS_MIN frames, touch acts on lifting
 * the stylus, and actions are returned once their pressed state has been
 * seen. Main thread only. */
#ifndef VIEW_DETAIL_H
#define VIEW_DETAIL_H

#include "details.h"
#include "lang.h"

#include <3ds.h>

#include <stdbool.h>

typedef enum {
	DETAIL_ACTION_NONE,
	/* Track: Play. Album or artist: A or a tap on the focused row. Either
	 * way, queue the document's tracks from `selected`. */
	DETAIL_ACTION_PLAY,
	/* Track: open its artist or its album on the same tab. */
	DETAIL_ACTION_ARTIST,
	DETAIL_ACTION_ALBUM,
	/* Album or artist: X, the detail of tracks[selected]. */
	DETAIL_ACTION_OPEN_TRACK,
	/* A failed album or artist: load it again. */
	DETAIL_ACTION_RETRY,
	/* B: one level back (while loading, that cancels the load). */
	DETAIL_ACTION_BACK,
	/* SELECT: switch the language; the view shows the bubble itself. */
	DETAIL_ACTION_LANGUAGE,
	/* A list's Y: switch shuffle on or off, then view_detail_set_shuffle
	 * with the new state; the view shows the bubble itself. */
	DETAIL_ACTION_SHUFFLE,
	/* Track: its download button, confirmed when it removes. The caller
	 * queues, retries or removes, then says so (view_detail_notice). */
	DETAIL_ACTION_DOWNLOAD,
	/* Album or playlist: the button over the list (download it, or once
	 * downloaded open it from the card). */
	DETAIL_ACTION_DOWNLOAD_ALL,
	/* An album or playlist on the card: its button, confirmed. */
	DETAIL_ACTION_REMOVE_ALL,
	/* The Downloads: A on an album or playlist, open it from the card. */
	DETAIL_ACTION_OPEN_COLLECTION
} detail_action;

/* After ui_init and icons_init. */
bool view_detail_init(void);
void view_detail_exit(void);

/* A detail comes on screen (a new level, a level back, or its tab): no
 * press, touch or bubble carries over. */
void view_detail_enter(void);

/* Once per frame, after hidScanInput, with that frame's new key presses.
 * START and the tab bar stay with the caller. */
detail_action view_detail_update(detail_doc *doc, u32 down);

/* Whether lists play shuffled, for Y's label and its bubble: at start-up
 * and after every DETAIL_ACTION_SHUFFLE. */
void view_detail_set_shuffle(bool on);

/* A bubble (5.9) with what a download action did; `ok` picks the note
 * icon, else the error one. */
void view_detail_notice(str_id text, bool ok);

bool view_detail_dirty(const detail_doc *doc);

/* The bottom screen, under the tab bar the caller draws. */
void view_detail_draw_bottom(const detail_doc *doc);

#endif
