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

/* Line `n` of t_cuerpo under the title, 0 being the artist's. The card
 * of a detail uses up to NOW_LINES of them. */
#define NOW_LINES 4

static inline int now_line_cy(int title_h, int body_h, int n)
{
	return now_artist_cy(title_h, body_h) + n * body_h;
}

static inline int now_album_cy(int title_h, int body_h)
{
	return now_line_cy(title_h, body_h, 1);
}

/* The detail card has no times nor bar: its cover is centred on the
 * screen's height, and its title with `count` lines centred beside it.
 * The text is laid out as on the card, moved down by now_detail_text_dy.
 * Under the strip of kinds (STYLES.md 5.17) the cover goes lower instead,
 * at RESULTS_CARD_Y, so the strip and the card are centred together: the
 * _at forms take the cover's top. */
static inline ui_rect now_detail_art_rect_at(int y)
{
	ui_rect r = { SP_3, y, ART_SIZE, ART_SIZE };
	return r;
}

static inline ui_rect now_detail_art_rect(void)
{
	return now_detail_art_rect_at((SCREEN_H - ART_SIZE) / 2);
}

/* Lines of a detail card's paragraph, t_meta, under its t_cuerpo lines:
 * an artist's biography (STYLES.md 7.12). */
#define ARTIST_BIO_LINES 5

static inline int now_clamp_lines(int n, int max)
{
	return n < 0 ? 0 : n > max ? max : n;
}

/* The card's text block: the title, `count` t_cuerpo lines SP_1 under it,
 * and `meta` t_meta lines SP_2 under those (or under the title, with no
 * t_cuerpo lines). */
static inline int now_detail_block_h(int title_h, int body_h, int count,
                                     int meta_h, int meta)
{
	count = now_clamp_lines(count, NOW_LINES);
	meta = now_clamp_lines(meta, ARTIST_BIO_LINES);
	return title_h + (count > 0 ? SP_1 + count * body_h : 0) +
	       (meta > 0 ? SP_2 + meta * meta_h : 0);
}

/* The centre of t_meta line `n` of that block, moved down by `dy` like
 * the rest of the card (now_detail_text_dy_meta_at). */
static inline int now_detail_meta_cy(int title_h, int body_h, int count,
                                     int meta_h, int n, int dy)
{
	count = now_clamp_lines(count, NOW_LINES);
	int top = SP_3 + title_h + (count > 0 ? SP_1 + count * body_h : 0) +
	          SP_2;
	return top + n * meta_h + meta_h / 2 + dy;
}

/* How far the card's text moves down so the whole block, paragraph and
 * all, is centred on the cover whose top is `art_y`. */
static inline int now_detail_text_dy_meta_at(int title_h, int body_h,
                                             int count, int meta_h, int meta,
                                             int art_y)
{
	int block_h = now_detail_block_h(title_h, body_h, count, meta_h, meta);
	return art_y + (ART_SIZE - block_h) / 2 - SP_3;
}

static inline int now_detail_text_dy_at(int title_h, int body_h, int count,
                                        int art_y)
{
	return now_detail_text_dy_meta_at(title_h, body_h, count, 0, 0, art_y);
}

static inline int now_detail_text_dy(int title_h, int body_h, int count)
{
	return now_detail_text_dy_at(title_h, body_h, count,
	                             now_detail_art_rect().y);
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
