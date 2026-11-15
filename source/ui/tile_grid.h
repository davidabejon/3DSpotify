/* A grid of playlist tiles on the bottom screen (docs/STYLES.md 5.16, 5.11,
 * 6.6 and 6.9), for the home tab.
 *
 * Each tile is the result row made compact: TILE_H high, the cover in a
 * TILE_COVER square and the name on one line, cut with "...". The layout,
 * focus moves, scrolling and hit testing are grid_logic.h's; this widget
 * keeps the state, the parsed names and the covers (through a thumbs.h
 * client, so it shares the pool with the track lists).
 *
 * As track_list, it answers for the tiles only: the view around it decides
 * what A or a tap on the focused tile do, owns any buttons beside it, and
 * composes tile_grid_look into its own look so the frame is only redrawn
 * when something changed (STYLES.md 9). Names are parsed when the items or
 * the language change, never per frame. Main thread only. */
#ifndef TILE_GRID_H
#define TILE_GRID_H

#include "grid_logic.h"
#include "lang.h"
#include "list_logic.h"
#include "press_logic.h"
#include "spclient.h"
#include "thumbs.h"

#include <3ds.h>
#include <citro2d.h>

#include <stdbool.h>

/* As many covers as the pool holds. */
#define TILE_GRID_MAX THUMBS_MAX

typedef enum {
	TILE_GRID_IDLE,        /* the stylus is not on the grid */
	TILE_GRID_BUSY,        /* it is: the rest of the input waits */
	/* A tile not in focus was tapped and took the focus. */
	TILE_GRID_FOCUSED,
	/* The focused tile was tapped: act once `press` has been seen
	 * (press_shown), as for a button. */
	TILE_GRID_ACTIVATE
} tile_grid_touch;

/* What the tiles look like this frame. */
typedef struct {
	int selected;          /* -1 while the focus is elsewhere */
	int scroll;
	int pressed;           /* -1 for none */
	int edge_dx, edge_dy;  /* the focused tile's nudge, px */
	int thumbs;            /* bumped when a cover arrives */
} tile_grid_look;

typedef struct {
	grid_geom geom;
	const sp_playlist_ref *items;   /* the owner's; valid while shown */
	int count;
	int selected;
	int scroll;            /* px; whole rows except while dragging */
	/* Whether the focus is on the grid (the view may have it on a button
	 * beside it): only then is the selected tile drawn focused, and only
	 * then does tapping it activate. */
	bool has_focus;
	thumbs_client thumbs;

	press_state press;
	int press_index;
	touch_track touch;
	list_drag drag;
	int edge_frames;
	int edge_dx, edge_dy;

	C2D_TextBuf buf, scratch;
	bool stale;
	lang_id parsed_lang;
	C2D_Text name[TILE_GRID_MAX];
} tile_grid;

/* After ui_init. False when the text buffers cannot be made. */
bool tile_grid_init(tile_grid *g, grid_geom geom);
void tile_grid_exit(tile_grid *g);

/* New content: at most TILE_GRID_MAX of `items`, `selected` focused and
 * scrolled into view on whole rows. NULL/0 empties it. */
void tile_grid_set(tile_grid *g, const sp_playlist_ref *items, int count,
                   int selected);
/* No press, touch, drag or nudge in progress: entering the view. */
void tile_grid_reset_input(tile_grid *g);
/* Focuses `index` on whole rows, as the buttons do (6.6). */
void tile_grid_focus(tile_grid *g, int index);
/* The first visible row, on whole rows or not. */
int  tile_grid_first_row(const tile_grid *g);

/* Once per frame, before the input. */
void tile_grid_tick(tile_grid *g);
/* Once per frame from the update of the view showing the grid: claims the
 * covers and uploads the ones that arrived. */
void tile_grid_thumbs_frame(tile_grid *g);

/* The stylus on the grid: tapping a tile focuses it, tapping the focused
 * tile activates it, dragging scrolls (6.4, 6.9). Only touches that start
 * inside the grid area, and not on the tab bar, count. */
tile_grid_touch tile_grid_update_touch(tile_grid *g, u32 down, u32 held,
                                       u32 up);
/* 6.6: with the focus dragged out of view, the first press of the D-pad or
 * of one of `confirm_keys` only brings the grid back to it. True when it
 * did, and the press is spent. */
bool tile_grid_realign(tile_grid *g, u32 down, u32 confirm_keys);
/* Whether the D-pad would move the focus that way (false: it would nudge),
 * so a view can send it to a button past the grid instead. */
bool tile_grid_can_move(const tile_grid *g, grid_dir dir);
/* The D-pad, grid_move's rules; L/R are the tab bar's. */
void tile_grid_navigate(tile_grid *g, u32 down);
/* The focused tile as pressed by a button: its press_state for the caller
 * to begin and wait on. */
press_state *tile_grid_focused_press(tile_grid *g);
/* The confirm button went up: the tile's press ends, unless the stylus is
 * on it. */
void tile_grid_release(tile_grid *g);

/* Parses the names when the items or the language changed. */
bool tile_grid_text_stale(const tile_grid *g);
void tile_grid_ensure_parsed(tile_grid *g);

tile_grid_look tile_grid_get_look(const tile_grid *g);
bool tile_grid_same_look(const tile_grid_look *a, const tile_grid_look *b);

/* The visible tiles and, when they do not all fit, the scroll indicator.
 * The bars around the area are the view's to draw over them. */
void tile_grid_draw(const tile_grid *g, const tile_grid_look *look);
/* `count` placeholder tiles while the content loads. */
void tile_grid_draw_skeleton(grid_geom geom, int count);

#endif
