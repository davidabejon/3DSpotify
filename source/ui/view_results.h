/* The results view, the Search tab (docs/STYLES.md 5.3 and 7.4): nothing
 * searched yet, the search in progress, the list of tracks, no results, or
 * the error, all in one view.
 *
 * Bottom screen: under the tab bar (tabbar.h, drawn by the caller), 4 rows
 * of ROW_H with the cover thumbnail (thumbs.h; the placeholder of 5.3
 * until it arrives), scrolled just enough to keep the focused row visible.
 * While searching, a "Searching..." bubble with the loading indicator sits
 * over the skeleton rows. The top screen belongs to top.h.
 *
 * Input follows STYLES.md 6: pressed states for at least ANIM_PRESS_MIN
 * frames, touch acts on lifting the stylus (tapping a row focuses it,
 * tapping the focused row plays it), dragging scrolls the list (6.9) next
 * to a scroll indicator against the right edge (5.11), A/B/Y invert their
 * glyph, and the
 * actions that leave the view are returned once the pressed state has been
 * seen. Text is parsed when the content, the scroll position or the
 * language changes, never per frame. Main thread only. */
#ifndef VIEW_RESULTS_H
#define VIEW_RESULTS_H

#include "lang.h"
#include "spclient.h"

#include <3ds.h>

#include <stdbool.h>

typedef enum {
	RESULTS_ACTION_NONE,
	/* A or a tap on the focused row: play view_results_selected(). */
	RESULTS_ACTION_OPEN,
	/* B: back to the Home tab. */
	RESULTS_ACTION_BACK,
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

/* Nothing searched yet, or the last search was cancelled. */
void view_results_idle(void);

/* A search is running. B shows its glyph pressed; cancelling stays with
 * the caller, which calls view_results_cancelling. */
void view_results_searching(void);
void view_results_cancelling(void);

/* The finished search: `count` tracks (0 for the empty state), `selected`
 * focused and scrolled into view. `tracks` must stay valid while the view
 * is shown. */
void view_results_show(const sp_track *tracks, int count, int selected);

/* The search failed: `advice` says what to do about it. */
void view_results_error(str_id advice);

/* The focused track. */
int view_results_selected(void);
/* Focuses track `index` on whole rows, as the buttons do (6.6): the
 * player's queue moved to it. */
void view_results_focus(int index);

/* Switching to the tab: no press or drag carries over. */
void view_results_enter(void);
/* Whether the thumbnails may download (STYLES.md 9): not while the audio
 * downloads. Disallowing stops and joins the worker; what it fetched stays
 * on screen. */
void view_results_allow_thumbs(bool allow);

/* Once per frame, after hidScanInput, with that frame's new key presses.
 * START stays with the caller. */
results_action view_results_update(u32 down);

/* True when a GPU frame is needed. */
bool view_results_dirty(void);

/* Draw into the current scene (the bottom screen). */
void view_results_draw_bottom(void);

#endif
