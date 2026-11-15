/* The Playing tab (docs/STYLES.md 7.5): the track in the queue, previous,
 * play/pause and next, the progress and volume bars. The cover goes on the
 * top screen (artwork.h), drawn by the caller.
 *
 * The audio itself lives in main.c and keeps playing on every tab; this
 * view only shows a snapshot of it (player_info) and turns input into
 * actions. Input follows STYLES.md 6: A presses the play button, left and
 * right the skip buttons, touch acts on lifting the stylus, and up/down
 * change the volume bar in the same frame (6.7). The stylus drags the
 * bars: the volume follows it at once; the progress bar's handle and the
 * time follow it, and the track moves there when it lifts. The title, the artist and
 * the album are links, touch only, to their details
 * (docs/plan_vistas_detalle.md 4.4). Per 9, a GPU frame is
 * only needed when the clock's second, the volume, the pause, a press or
 * the phase changes; the download share is sampled with the clock. Main
 * thread only. */
#ifndef VIEW_PLAYER_H
#define VIEW_PLAYER_H

#include "lang.h"
#include "player_logic.h"
#include "spclient.h"

#include <3ds.h>

#include <stdbool.h>

typedef struct {
	player_phase phase;
	const sp_track *track;  /* NULL when idle */
	unsigned serial;        /* changes with every track started */
	int index, count;       /* place in the queue */
	str_id stage;           /* while loading */
	bool paused;
	unsigned elapsed_s;
	unsigned duration_s;
	unsigned download_pct;
	unsigned volume_pct;
	/* The track can move to another point: playing, with its length and
	 * its file's size known. */
	bool seekable;
	const char *error;      /* when failed */
} player_info;

typedef enum {
	PLAYER_ACTION_NONE,
	/* A or the middle button: pause/resume, or play the track again once
	 * finished or failed. */
	PLAYER_ACTION_TOGGLE,
	PLAYER_ACTION_PREV,
	PLAYER_ACTION_NEXT,
	PLAYER_ACTION_VOLUME_UP,
	PLAYER_ACTION_VOLUME_DOWN,
	/* B: back to the Search tab, the audio keeps playing. */
	PLAYER_ACTION_BACK,
	/* SELECT: switch the language. */
	PLAYER_ACTION_LANGUAGE,
	/* The links: the detail of the track playing, of its artist or of its
	 * album, on this tab. */
	PLAYER_ACTION_OPEN_TRACK,
	PLAYER_ACTION_OPEN_ARTIST,
	PLAYER_ACTION_OPEN_ALBUM,
	/* The stylus lifted from the progress bar: move the track to
	 * view_player_value() thousandths of its length. */
	PLAYER_ACTION_SEEK,
	/* The stylus on the volume bar: set it to view_player_value()
	 * thousandths. */
	PLAYER_ACTION_VOLUME_SET
} player_action;

bool view_player_init(void);
void view_player_exit(void);

/* Switching to the tab: no press carries over; forces a repaint. */
void view_player_enter(void);

/* What the player is doing now. Once per frame, before view_player_update
 * and view_player_dirty. `info` is copied; its strings must stay valid
 * until the next call. */
void view_player_set(const player_info *info);

/* Once per frame, after hidScanInput. START and the tabs stay with the
 * caller. */
player_action view_player_update(u32 down);

/* The value of the PLAYER_ACTION_SEEK or PLAYER_ACTION_VOLUME_SET just
 * returned, in thousandths. */
int view_player_value(void);

bool view_player_dirty(void);
void view_player_draw_bottom(void);

#endif
