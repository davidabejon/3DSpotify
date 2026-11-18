/* The results view, the Search tab (docs/STYLES.md 5.3 and 7.4): nothing
 * searched yet, the search in progress, the results, no results, or the
 * error, all in one view. The results come by kind (results_logic.h):
 * tracks, artists, albums and playlists, one kind on screen at a time,
 * switched with left/right. Nothing searched yet suggests the last
 * searches (recent_logic.h) under the search button, when there are any.
 *
 * Bottom screen: under the tab bar (tabbar.h, drawn by the caller), 4 rows
 * of ROW_H with the cover thumbnail (thumbs.h; the placeholder of 5.3
 * until it arrives), scrolled just enough to keep the focused row visible.
 * While searching, a "Searching..." bubble with the loading indicator sits
 * over the skeleton rows. The top screen belongs to top.h, which this
 * view feeds while the results are on screen: the strip of kinds
 * (view_results_strip) and the focused row's card (view_results_preview).
 *
 * Input follows STYLES.md 6: pressed states for at least ANIM_PRESS_MIN
 * frames, touch acts on lifting the stylus (tapping a row focuses it,
 * tapping the focused row plays it), dragging scrolls the list (6.9) next
 * to a scroll indicator against the right edge (5.11), up/down move the
 * focus and left/right switch the kind (6.6, no wrapping), A/B/X/Y and
 * the D-pad invert their glyph, and the actions that leave the view are returned once the pressed state has been
 * seen. Text is parsed when the content, the scroll position or the
 * language changes, never per frame. Main thread only. */
#ifndef VIEW_RESULTS_H
#define VIEW_RESULTS_H

#include "details.h"
#include "lang.h"
#include "recent_logic.h"
#include "results_logic.h"
#include "search_results.h"
#include "spclient.h"

#include <3ds.h>

#include <stdbool.h>

typedef enum {
	RESULTS_ACTION_NONE,
	/* A or a tap on the focused row: play view_results_selected() of the
	 * tracks, or open the artist, album or playlist (view_results_kind). */
	RESULTS_ACTION_OPEN,
	/* X, on the tracks only: the detail of view_results_selected().
	 * Playing stays with A, with no screen in between (STYLES.md 7.4). */
	RESULTS_ACTION_DETAIL,
	/* B with nothing searched yet: back to the Home tab. */
	RESULTS_ACTION_BACK,
	/* B on the results, none found or the error: back to the screen
	 * before them, nothing searched yet (view_results_idle). */
	RESULTS_ACTION_CLOSE,
	/* A or a tap on a recent search: run view_results_recent_query()
	 * again. */
	RESULTS_ACTION_RECENT,
	/* Y, or the idle and empty states' button: ask for another query. */
	RESULTS_ACTION_NEW_SEARCH,
	/* A, or the error state's button: run the same query again. */
	RESULTS_ACTION_RETRY,
	/* SELECT: switch the language. The view shows the bubble with the new
	 * language's name by itself. */
	RESULTS_ACTION_LANGUAGE
} results_action;

/* After ui_init and icons_init. */
bool view_results_init(void);
void view_results_exit(void);

/* Nothing searched yet, or the last search was cancelled or left: the
 * search button focused. */
void view_results_idle(void);

/* The recent searches to suggest, copied. */
void view_results_set_recent(const recent_list *recent);
/* The recent search chosen by the last RESULTS_ACTION_RECENT. */
const char *view_results_recent_query(void);

/* A search is running. B shows its glyph pressed; cancelling stays with
 * the caller, which calls view_results_cancelling. */
void view_results_searching(void);
void view_results_cancelling(void);

/* A finished search, every kind of it. `items` must stay valid while the
 * view is shown; the tracks are playable sp_tracks, the other kinds rows
 * as search_results.h fills them. */
typedef struct {
	const sp_track *items[RESULT_KINDS];
	int  count[RESULT_KINDS];
	bool offered[RESULT_KINDS];   /* the fallback offers no playlists */
	int  preferred_kind;          /* the best result's kind, -1: none */
	int  preferred_index;         /* and its row */
} results_content;

/* The finished search, on its preferred kind (else the first with
 * results), or the empty state when no kind has any. */
void view_results_show(const results_content *c);

/* The search failed: `advice` says what to do about it. */
void view_results_error(str_id advice);

/* The kind on screen, and the focused row within it. */
result_kind view_results_kind(void);
int view_results_selected(void);

/* The top screen's strip of kinds (STYLES.md 5.17): false, and `out`
 * untouched, unless the results are on screen. */
bool view_results_strip(results_strip *out);

/* The focused row's card for the top screen (STYLES.md 7.4, 7.12), or NULL
 * when no results are on screen. Built from the row: a track's is that
 * track (DETAIL_TRACK, single); an artist's, album's or playlist's is a
 * preview (detail_doc.preview). Its large picture is named only once the
 * focus has rested PREVIEW_DELAY frames, unless it is already cached. */
const detail_doc *view_results_preview(void);

/* Switching to the tab: no press or drag carries over. */
void view_results_enter(void);
/* Whether the thumbnails may download (STYLES.md 9): not while the audio
 * downloads. Disallowing asks the worker to stop without waiting for it
 * (thumbs_stop); what it fetched stays on screen. */
void view_results_allow_thumbs(bool allow);

/* Once per frame, after hidScanInput, with that frame's new key presses.
 * START stays with the caller. */
results_action view_results_update(u32 down);

/* True when a GPU frame is needed. */
bool view_results_dirty(void);

/* Draw into the current scene (the bottom screen). */
void view_results_draw_bottom(void);

#endif
