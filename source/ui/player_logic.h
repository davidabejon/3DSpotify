/* The player's phases and the layout of the Playing tab
 * (docs/STYLES.md 5.4, 5.5 and 7.5). Pure C with no 3DS headers, tested
 * in tests/native_logic_test.c. Whole pixels throughout.
 *
 * Bottom screen, 320 x 240:
 *   tabs       0..32   the tab bar (tabs_logic.h)
 *   track     36..68   link zone: the title, "3/24" on the right
 *   artist    72..104  link zone, x 8..140  } 132 px each,
 *   album     72..104  link zone, x 144..276} SP_1 apart
 *   download  72..104  x 280..312: where the track is in the downloads,
 *                      a TOUCH_MIN square, SP_1 right of the album
 *   controls 108..156  previous 40x40, play/pause 48x48, next 40x40,
 *                      centred, SP_4 apart
 *   times    163..177  elapsed on the left, duration on the right
 *   progress 180..188  progress bar, its handle 177..191
 *   volume   201..207  "Vol." and the volume bar
 *   hints    216..240  button bar
 *
 * The links (STYLES.md 5.14) are touch only, TOUCH_MIN
 * high (STYLES.md 4.4): they open the track's, the artist's or the album's
 * detail. At rest only their text shows; there is no focus state, since the
 * D-pad already skips tracks and sets the volume. */
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

/* --- Layout of the Playing tab --------------------------------------- */

typedef enum {
	PLAYER_CONTROL_NONE = -1,
	PLAYER_CONTROL_PREV,
	PLAYER_CONTROL_PLAY,
	PLAYER_CONTROL_NEXT,
	PLAYER_CONTROL_COUNT
} player_control;

/* The top of the controls, under the links. */
#define PLAYER_CONTROLS_TOP 108

typedef enum {
	PLAYER_LINK_NONE = -1,
	PLAYER_LINK_TRACK,
	PLAYER_LINK_ARTIST,
	PLAYER_LINK_ALBUM,
	PLAYER_LINK_COUNT
} player_link;

/* The links show a track: while it loads, plays and once finished, the
 * phases that draw its text. Idle and failed have none. */
static inline bool player_links_enabled(player_phase phase)
{
	return phase == PLAYER_PHASE_LOADING || phase == PLAYER_PHASE_PLAYING ||
	       phase == PLAYER_PHASE_FINISHED;
}

static inline ui_rect player_link_rect(int link)
{
	int top = TAB_H + SP_1;
	int width = SCREEN_BOTTOM_W - 2 * SP_2;
	/* The second row leaves the download square at its right end. */
	int half = (width - TOUCH_MIN - SP_1 - SP_1) / 2;
	int second = top + TOUCH_MIN + SP_1;
	switch (link) {
	case PLAYER_LINK_ARTIST:
		return (ui_rect){ SP_2, second, half, TOUCH_MIN };
	case PLAYER_LINK_ALBUM:
		return (ui_rect){ SP_2 + half + SP_1, second, half, TOUCH_MIN };
	default:
		return (ui_rect){ SP_2, top, width, TOUCH_MIN };
	}
}

/* The track's download: an icon of where it is, touched like X. */
static inline ui_rect player_download_rect(void)
{
	ui_rect r = { SCREEN_BOTTOM_W - SP_2 - TOUCH_MIN,
	              TAB_H + SP_1 + TOUCH_MIN + SP_1, TOUCH_MIN, TOUCH_MIN };
	return r;
}

/* The link under (x, y) among the `enabled` ones, or PLAYER_LINK_NONE. */
static inline player_link player_link_hit(int x, int y,
                                          const bool enabled[PLAYER_LINK_COUNT])
{
	for (int i = 0; i < PLAYER_LINK_COUNT; i++)
		if (enabled[i] && ui_rect_contains(player_link_rect(i), x, y))
			return (player_link)i;
	return PLAYER_LINK_NONE;
}

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

/* Where the stylus takes the bars: wider and taller than they are drawn,
 * the whole width, split halfway between them. The progress bar's zone
 * starts under the times; the volume's ends at the button bar. */
static inline int player_bars_split(int label_w)
{
	ui_rect progress = player_progress_rect();
	ui_rect volume = player_volume_rect(label_w);
	return (progress.y + progress.h + volume.y) / 2;
}

static inline ui_rect player_seek_zone(int label_w)
{
	ui_rect progress = player_progress_rect();
	int top = progress.y - SP_2;
	ui_rect r = { 0, top, SCREEN_BOTTOM_W,
	              player_bars_split(label_w) - top };
	return r;
}

static inline ui_rect player_volume_zone(int label_w)
{
	ui_rect volume = player_volume_rect(label_w);
	int top = player_bars_split(label_w);
	ui_rect r = { volume.x - SP_2, top, SCREEN_BOTTOM_W - (volume.x - SP_2),
	              SCREEN_H - BAR_H - top };
	return r;
}

/* The share of bar `bar` at x, in thousandths, clamped: past either end
 * is that end. */
static inline int player_bar_permille(ui_rect bar, int x)
{
	if (bar.w <= 0 || x <= bar.x)
		return 0;
	if (x >= bar.x + bar.w)
		return 1000;
	return (x - bar.x) * 1000 / bar.w;
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

/* Without a connection only downloaded tracks play: from position `pos`
 * of the play order, the next one `direction` (-1 or +1) away whose track
 * `playable` says can play, or -1. `order` maps positions to the queue's
 * indexes, as playback_order does. Like queue_step, it never wraps around
 * the ends: a queue played through stops instead of starting over. */
static inline int playback_next_playable(int pos, int count, int direction,
                                         const int *order,
                                         const bool *playable)
{
	if (count <= 0 || pos < -1 || pos > count ||
	    (direction != -1 && direction != 1))
		return -1;
	for (int p = pos + direction; p >= 0 && p < count; p += direction)
		if (playable[order[p]])
			return p;
	return -1;
}

#endif
