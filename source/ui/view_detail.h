/* The detail screens on the bottom screen (docs/plan_vistas_detalle.md 4):
 * a track, an album or an artist, whichever is on top of the active tab's
 * detail stack (details.h). The top screen belongs to top.h.
 *
 * Track: three buttons with the geometry of the Settings tab
 * (detail_logic.h) -- Play, the artist and the album, the last two
 * disabled when the track does not name them -- and the bar
 * "[A] Choose [B] Back". The focus starts on Play.
 *
 * Every call takes the document on screen. When it is a different one (or
 * the same one after it loaded), the view parses it again and starts with
 * no press, focus or bubble left over. Input follows STYLES.md 6 like the
 * other views: pressed states for at least ANIM_PRESS_MIN frames, touch
 * acts on lifting the stylus, and actions are returned once their pressed
 * state has been seen. Main thread only. */
#ifndef VIEW_DETAIL_H
#define VIEW_DETAIL_H

#include "details.h"

#include <3ds.h>

#include <stdbool.h>

typedef enum {
	DETAIL_ACTION_NONE,
	/* Track: Play. Queue the document's tracks from `selected`. */
	DETAIL_ACTION_PLAY,
	/* Track: open its artist or its album on the same tab. */
	DETAIL_ACTION_ARTIST,
	DETAIL_ACTION_ALBUM,
	/* B: one level back. */
	DETAIL_ACTION_BACK,
	/* SELECT: switch the language; the view shows the bubble itself. */
	DETAIL_ACTION_LANGUAGE
} detail_action;

/* After ui_init and icons_init. */
bool view_detail_init(void);
void view_detail_exit(void);

/* A detail comes on screen (a new level, a level back, or its tab): no
 * press, touch or bubble carries over. */
void view_detail_enter(void);

/* Once per frame, after hidScanInput, with that frame's new key presses.
 * START and the tab bar stay with the caller. */
detail_action view_detail_update(const detail_doc *doc, u32 down);

bool view_detail_dirty(const detail_doc *doc);

/* The bottom screen, under the tab bar the caller draws. */
void view_detail_draw_bottom(const detail_doc *doc);

#endif
