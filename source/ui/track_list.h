/* A list of tracks on the bottom screen (docs/STYLES.md 5.3, 5.11, 6.6 and
 * 6.9), shared by the results and the album and artist details.
 *
 * Rows of ROW_H below the tab bar, RESULTS_PER_PAGE at a time, with the
 * focus kept on whole rows by the buttons, the list following the stylus
 * when dragged, and a scroll indicator when the list does not fit. The
 * square at the left of each row is either the cover thumbnail (thumbs.h)
 * or, where every row shares the same cover (an album), the track's
 * position in the list.
 *
 * The widget answers for the rows only: the view around it keeps its own
 * modes, buttons, glyphs and button bar, decides what A, X or a tap on the
 * focused row do, and composes track_list_look into its own look, so the
 * frame is only redrawn when something changed (STYLES.md 9). Row text is
 * parsed when the tracks or the language change, never per frame.
 *
 * Each list is its own instance, so the results keep their state under a
 * detail. Main thread only. */
#ifndef TRACK_LIST_H
#define TRACK_LIST_H

#include "lang.h"
#include "list_logic.h"
#include "press_logic.h"
#include "spclient.h"

#include <3ds.h>
#include <citro2d.h>

#include <stdbool.h>

/* As many as a search returns (SEARCH_LIMIT, THUMBS_MAX). */
#define TRACK_LIST_MAX 24

typedef enum {
	TRACK_LIST_THUMBS,     /* the cover thumbnail, via thumbs.h */
	TRACK_LIST_NUMBERS     /* the position in the list, 1-based */
} track_list_style;

typedef enum {
	TRACK_LIST_IDLE,       /* the stylus is not on the list */
	TRACK_LIST_BUSY,       /* it is: the rest of the input waits */
	/* The focused row was tapped: act once `press` has been seen
	 * (press_shown), as for a button. */
	TRACK_LIST_ACTIVATE
} track_list_touch;

/* What the rows look like this frame. */
typedef struct {
	int selected, first, scroll;
	int pressed;           /* the pressed row, -1 for none */
	int edge;              /* the focused row's nudge, px */
} track_list_look;

typedef struct {
	track_list_style style;
	const sp_track *tracks;   /* the owner's; valid while shown */
	int count;
	int selected;
	int first;             /* first visible row: scroll / LIST_PITCH */
	int scroll;            /* px; whole rows except while dragging */
	/* Set by the owner: thumbs.c holds this list, so its images are
	 * this list's. Until then the placeholder shows. */
	bool thumbs_synced;

	press_state press;     /* the focused row's, for A and taps */
	int press_index;
	touch_track touch;
	list_drag drag;
	int edge_frames;
	int edge_dir;

	C2D_TextBuf buf, scratch;
	bool stale;
	lang_id parsed_lang;
	C2D_Text title[TRACK_LIST_MAX];
	C2D_Text artist[TRACK_LIST_MAX];
	C2D_Text duration[TRACK_LIST_MAX];
	C2D_Text number[TRACK_LIST_MAX];
	float duration_width[TRACK_LIST_MAX];
} track_list;

/* After ui_init. False when the text buffers cannot be made. */
bool track_list_init(track_list *l, track_list_style style);
void track_list_exit(track_list *l);

/* New content: at most TRACK_LIST_MAX of `tracks`, `selected` focused and
 * scrolled into view on whole rows. NULL/0 empties it. No press or drag
 * carries over. */
void track_list_set(track_list *l, const sp_track *tracks, int count,
                    int selected);
/* No press, touch, drag or nudge in progress: entering the view. */
void track_list_reset_input(track_list *l);

/* Focuses `index` on whole rows, as the buttons do (6.6). */
void track_list_focus(track_list *l, int index);

/* Once per frame, before the input. */
void track_list_tick(track_list *l);

/* The stylus on the list: tapping a row focuses it, tapping the focused
 * row activates it, dragging scrolls (6.4, 6.9). Only touches that start
 * inside the list area, and not on the tab bar, count. */
track_list_touch track_list_update_touch(track_list *l, u32 down, u32 held,
                                         u32 up);
/* 6.6: with the focus dragged out of view, the first press of the D-pad or
 * of one of `confirm_keys` only brings the list back to it. True when it
 * did, and the press is spent; the caller shows its glyph pressed. */
bool track_list_realign(track_list *l, u32 down, u32 confirm_keys);
/* The D-pad: up/down by one, left/right by a page; L/R are the tab
 * bar's. */
void track_list_navigate(track_list *l, u32 down);
/* The focused row as pressed by a button: returns its press_state for the
 * caller to begin and wait on. */
press_state *track_list_focused_press(track_list *l);
/* The confirm button went up: the row's press ends, unless the stylus is
 * on it. */
void track_list_release(track_list *l);

/* Parses the rows when the tracks or the language changed. */
void track_list_ensure_parsed(track_list *l);
/* True when the next frame would parse text. */
bool track_list_text_stale(const track_list *l);

track_list_look track_list_get_look(const track_list *l);
bool track_list_same_look(const track_list_look *a, const track_list_look *b);

/* The visible rows, the focused one last so its nudge draws over its
 * neighbours, and the scroll indicator when the list does not fit. */
void track_list_draw(const track_list *l, const track_list_look *look);
/* RESULTS_PER_PAGE empty rows: the layout a list about to arrive will
 * fill. */
void track_list_draw_skeleton(void);

#endif
