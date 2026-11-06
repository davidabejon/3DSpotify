/* The player's queue steps and the layout of the Playing tab
 * (docs/STYLES.md 5.4, 5.5 and 7.5). Pure C with no 3DS headers, tested
 * in tests/native_logic_test.c. Whole pixels throughout.
 *
 * Bottom screen, 320 x 240:
 *   tabs       0..32   the tab bar (tabs_logic.h)
 *   title     44..62   the track, "3/24" on the right
 *   artist    66..81
 *   controls 100..148  previous 40x40, play/pause 48x48, next 40x40,
 *                      centred, SP_4 apart
 *   times    155..169  elapsed on the left, duration on the right
 *   progress 172..180  progress bar, its handle 169..183
 *   volume   193..199  "Vol." and the volume bar
 *   hints    216..240  button bar */
#ifndef PLAYER_LOGIC_H
#define PLAYER_LOGIC_H

#include "layout.h"
#include "theme.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef enum {
	PLAYER_PHASE_IDLE,      /* nothing played yet */
	PLAYER_PHASE_LOADING,   /* cover, audio key, connection */
	PLAYER_PHASE_PLAYING,   /* playing or paused */
	PLAYER_PHASE_FINISHED,
	PLAYER_PHASE_FAILED
} player_phase;

/* The track `direction` (-1 or +1) steps from `index` in a queue of
 * `count`, or -1: the queue does not wrap. */
static inline int playback_queue_step(int index, int count, int direction)
{
	if (count <= 0 || index < 0 || index >= count ||
	    (direction != -1 && direction != 1))
		return -1;

	int next = index + direction;
	return next >= 0 && next < count ? next : -1;
}

/* --- Layout of the Playing tab --------------------------------------- */

typedef enum {
	PLAYER_CONTROL_NONE = -1,
	PLAYER_CONTROL_PREV,
	PLAYER_CONTROL_PLAY,
	PLAYER_CONTROL_NEXT,
	PLAYER_CONTROL_COUNT
} player_control;

/* Where the track's text starts and the top of the controls. */
#define PLAYER_TEXT_TOP (TAB_H + SP_3)
#define PLAYER_CONTROLS_TOP 100

static inline ui_rect player_control_rect(int control)
{
	int play_x = (SCREEN_BOTTOM_W - BTN_PLAY) / 2;
	int skip_y = PLAYER_CONTROLS_TOP + (BTN_PLAY - BTN_SKIP) / 2;
	ui_rect r;
	switch (control) {
	case PLAYER_CONTROL_PREV:
		r = (ui_rect){ play_x - SP_4 - BTN_SKIP, skip_y, BTN_SKIP,
		               BTN_SKIP };
		break;
	case PLAYER_CONTROL_NEXT:
		r = (ui_rect){ play_x + BTN_PLAY + SP_4, skip_y, BTN_SKIP,
		               BTN_SKIP };
		break;
	default:
		r = (ui_rect){ play_x, PLAYER_CONTROLS_TOP, BTN_PLAY, BTN_PLAY };
		break;
	}
	return r;
}

/* The control under (x, y), or PLAYER_CONTROL_NONE. */
static inline player_control player_hit_test(int x, int y)
{
	for (int i = 0; i < PLAYER_CONTROL_COUNT; i++)
		if (ui_rect_contains(player_control_rect(i), x, y))
			return (player_control)i;
	return PLAYER_CONTROL_NONE;
}

/* The vertical centre of the elapsed and duration labels. */
static inline int player_times_cy(void)
{
	return PLAYER_CONTROLS_TOP + BTN_PLAY + SP_4 - 2;
}

static inline ui_rect player_progress_rect(void)
{
	ui_rect r = { SP_2, player_times_cy() + SP_3 - 2,
	              SCREEN_BOTTOM_W - 2 * SP_2, PROGRESS_H };
	return r;
}

/* `label_w`: the width of "Vol." before it. */
static inline ui_rect player_volume_rect(int label_w)
{
	ui_rect progress = player_progress_rect();
	int x = SP_2 + label_w + SP_2;
	ui_rect r = { x, progress.y + progress.h + SP_3 + 1,
	              SCREEN_BOTTOM_W - SP_2 - x, VOLUME_H };
	return r;
}

/* The notice of a failed track, under the tab bar. */
static inline ui_rect player_notice_rect(void)
{
	ui_rect r = { SP_2, TAB_H + SP_4, SCREEN_BOTTOM_W - 2 * SP_2,
	              4 * SP_6 };
	return r;
}

static inline ui_rect player_hints_rect(void)
{
	ui_rect r = { 0, SCREEN_H - BAR_H, SCREEN_BOTTOM_W, BAR_H };
	return r;
}

/* How much of `total` is `part`, in thousandths, clamped to 0..1000. */
static inline int player_permille(unsigned part, unsigned total)
{
	if (total == 0)
		return 0;
	if (part >= total)
		return 1000;
	return (int)((unsigned long long)part * 1000u / total);
}

/* How many of `width` px a fraction in thousandths fills. */
static inline int player_fill_px(int width, int permille)
{
	if (permille <= 0 || width <= 0)
		return 0;
	if (permille >= 1000)
		return width;
	return width * permille / 1000;
}

/* The progress bar's fill in thousandths: full once the track finished
 * (7.5), else the elapsed share. */
static inline int player_progress_permille(bool finished, unsigned elapsed,
                                           unsigned duration)
{
	return finished ? 1000 : player_permille(elapsed, duration);
}

/* "m:ss", as the player shows the elapsed time and the duration. */
static inline void player_format_time(unsigned seconds, char *out,
                                      size_t len)
{
	snprintf(out, len, "%u:%02u", seconds / 60, seconds % 60);
}

/* 9: the download share is shown with the clock, not every frame. It
 * follows the real value on every `tick` (a new second, phase or track)
 * and otherwise once every PLAYER_DOWNLOAD_FRAMES frames. */
enum { PLAYER_DOWNLOAD_FRAMES = 60 };

typedef struct {
	unsigned shown;
	int frames;
} player_download_sample;

static inline void player_sample_download(player_download_sample *sample,
                                          bool tick, unsigned pct)
{
	if (tick || ++sample->frames >= PLAYER_DOWNLOAD_FRAMES) {
		sample->shown = pct;
		sample->frames = 0;
	}
}

#endif
