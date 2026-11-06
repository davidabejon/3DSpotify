/* The results view (docs/STYLES.md 5.3 and 7.4): the search in progress,
 * the list of tracks, no results, or the error, all in one view.
 *
 * Bottom screen: 4 rows of ROW_H with the cover thumbnail (thumbs.h; the
 * placeholder of 5.3 until it arrives), scrolled just enough to keep the
 * focused row visible. Top screen: the query and the focused
 * track's details, or the loading indicator while searching.
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
	/* B: back to the home view. */
	RESULTS_ACTION_BACK,
	/* Y, or the empty state's button: ask for another query. */
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

/* The search for `query` is running. B shows its glyph pressed; cancelling
 * stays with the caller, which calls view_results_cancelling. */
void view_results_searching(const char *query);
void view_results_cancelling(void);

/* The finished search: `count` tracks (0 for the empty state), `selected`
 * focused and scrolled into view. `tracks` must stay valid while the view
 * is shown. */
void view_results_show(const char *query, const sp_track *tracks, int count,
                       int selected);

/* The search failed: `error` is the technical detail, `advice` what to do
 * about it. */
void view_results_error(const char *query, const char *error,
                        str_id advice);

/* The focused track. */
int view_results_selected(void);

/* Once per frame, after hidScanInput, with that frame's new key presses.
 * START stays with the caller. */
results_action view_results_update(u32 down);

/* True when a GPU frame is needed. */
bool view_results_dirty(void);

/* Draw into the current scene. */
void view_results_draw_top(void);
void view_results_draw_bottom(void);

#endif
