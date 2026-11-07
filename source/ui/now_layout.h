/* Layout of the top screen while music plays (docs/STYLES.md 7.5): the
 * "now playing" card on Home, Search and Settings, the big cover on the
 * Playing tab. Pure C with no 3DS headers, tested in
 * tests/native_logic_test.c. Whole pixels throughout.
 *
 * Top screen, 400 x 240, card (system font, line heights 23/15/14):
 *   cover     12..172 x 12..172, its hard shadow to 175
 *   title     x 188..388, from y 12        t_titulo
 *   artist    4 px below the title         t_cuerpo
 *   album     right below the artist       t_cuerpo (left out if empty)
 *   times     188..202   elapsed on the left, duration on the right
 *   progress  205..213   full width inside e3, its handle 202..216
 *
 * Big cover: 216 x 216 at (92, 12), e3 above and below. */
#ifndef NOW_LAYOUT_H
#define NOW_LAYOUT_H

#include "layout.h"
#include "theme.h"

static inline ui_rect now_art_rect(void)
{
	ui_rect r = { SP_3, SP_3, ART_SIZE, ART_SIZE };
	return r;
}

/* The text column, right of the cover. */
static inline int now_text_x(void)
{
	return SP_3 + ART_SIZE + SP_4;
}

static inline int now_text_w(void)
{
	return SCREEN_TOP_W - SP_3 - now_text_x();
}

/* Line centres, from the line heights of t_titulo and t_cuerpo. */
static inline int now_title_cy(int title_h)
{
	return SP_3 + title_h / 2;
}

static inline int now_artist_cy(int title_h, int body_h)
{
	return SP_3 + title_h + SP_1 + body_h / 2;
}

static inline int now_album_cy(int title_h, int body_h)
{
	return now_artist_cy(title_h, body_h) + body_h;
}

/* The times line, e4 under the cover; `meta_h` is t_meta's line. */
static inline int now_times_top(void)
{
	return SP_3 + ART_SIZE + SP_4;
}

static inline int now_times_cy(int meta_h)
{
	return now_times_top() + meta_h / 2;
}

/* Under the times, so the handle (5.4) just clears their line. */
static inline ui_rect now_progress_rect(int meta_h)
{
	ui_rect r = { SP_3,
	              now_times_top() + meta_h + (HANDLE_H - PROGRESS_H) / 2,
	              SCREEN_TOP_W - 2 * SP_3, PROGRESS_H };
	return r;
}

static inline ui_rect now_big_art_rect(void)
{
	ui_rect r = { (SCREEN_TOP_W - ART_BIG) / 2, (SCREEN_H - ART_BIG) / 2,
	              ART_BIG, ART_BIG };
	return r;
}

#endif
