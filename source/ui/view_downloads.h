/* The downloads manager: the Settings tab in another mode, reached from
 * its Downloads button, B back to it (docs/STYLES.md 7.13).
 *
 * Bottom screen: one list of what is downloading, queued, failed and on
 * the card, in sections (downloads_view_logic.h), with the covers as
 * thumbnails. A on a row: cancel what is queued or under way (a second A
 * while "Press A again to cancel" shows), try a failed one again, or open
 * a downloaded track or album in the Home tab. X removes any row, asking
 * the same way. Y pauses or resumes the whole queue. The stylus taps a row
 * to focus it, taps the focused one to act, and drags the list.
 *
 * Top screen: a card with what is on the card, the space it takes, the
 * space left on the SD card, and how the queue is.
 *
 * The list is rebuilt when the downloads change (downloads_generation) and
 * the share of the track downloading is read once a second (9). Main
 * thread only. */
#ifndef VIEW_DOWNLOADS_H
#define VIEW_DOWNLOADS_H

#include "downloads_logic.h"
#include "spclient.h"

#include <3ds.h>

#include <stdbool.h>
#include <stddef.h>

typedef enum {
	DLV_ACTION_NONE,
	/* B: back to the Settings buttons, the focus on Downloads. */
	DLV_ACTION_BACK,
	/* SELECT: switch the language. */
	DLV_ACTION_LANGUAGE,
	/* A on a downloaded track: its detail on the Home tab
	 * (view_downloads_target_track). */
	DLV_ACTION_OPEN_TRACK,
	/* A on a downloaded album or playlist: its detail on the Home tab
	 * (view_downloads_target_collection). */
	DLV_ACTION_OPEN_COLLECTION
} dlv_action;

/* After ui_init and icons_init. */
bool view_downloads_init(void);
void view_downloads_exit(void);

/* Opening the manager, or coming back to it: no press carries over; the
 * free space is read again. */
void view_downloads_enter(void);

/* Once per frame, after hidScanInput and downloads_poll. */
dlv_action view_downloads_update(u32 down);

/* What DLV_ACTION_OPEN_* is about. */
bool view_downloads_target_track(sp_track *out);
bool view_downloads_target_collection(char *uri, size_t len,
                                      dl_coll_kind *kind);

bool view_downloads_dirty(void);
void view_downloads_draw_bottom(void);

/* The top screen while the manager shows. */
bool view_downloads_top_dirty(void);
void view_downloads_draw_top(void);

#endif
