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
 * Big cover: 216 x 216 at (92, 12), e3 above and below.
 *
 * Mini player (STYLES.md 5.18), 200 x 40, in the bottom right corner
 * while the top screen shows something else, its corner on what it sits
 * by: the detail card's cover bottom (188..388 x 160..200, or 176..216
 * under the strip of kinds), the manager's panel edges (184..384 x
 * 184..224). Inside:
 *   cover     SP_1 in, 32 x 32
 *   texts     e2 right of it, 124 wide     t_cuerpo / t_meta
 *   icon      16 x 16, e2 from the right, centred on the cover
 *   band      2 px over the bottom outline */
#ifndef NOW_LAYOUT_H
#define NOW_LAYOUT_H

#include "layout.h"
#include "player_logic.h"
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
 * screen's height, and its title with `count` lines centred beside it --
 * or, over the mini player (STYLES.md 5.18), hanging from the cover's top
 * edge, leaving the bottom corner to it. The text is laid out as on the
 * card, moved down by now_detail_text_dy or now_detail_text_dy_top_at.
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

/* Over the mini player: how far the card's text moves down so the top of
 * its title's line is the top of the cover whose top is `art_y`. */
static inline int now_detail_text_dy_top_at(int art_y)
{
	return art_y - SP_3;
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

/* The mini player (STYLES.md 5.18): the track that plays, in the bottom
 * right corner, while the top screen shows something else. Its cover is a
 * touch target's size, SP_1 inside the panel on three sides; the fourth
 * row of SP_1 under it holds the progress band. As wide as the detail
 * card's text column, wherever it goes. */
#define NOW_MINI_COVER TOUCH_MIN                        /* 32 */
#define NOW_MINI_H (2 * SP_1 + NOW_MINI_COVER)          /* 40 */
#define NOW_MINI_W (SCREEN_TOP_W - SP_3 - now_text_x()) /* 200 */

/* The panel by its bottom right corner. */
static inline ui_rect now_mini_rect_at(int right, int bottom)
{
	ui_rect r = { right - NOW_MINI_W, bottom - NOW_MINI_H, NOW_MINI_W,
	              NOW_MINI_H };
	return r;
}

/* Over a detail's card, whose cover's top is `art_y`: in the text column,
 * its bottom on the cover's (188, 160 alone; 188, 176 under the strip). */
static inline ui_rect now_mini_card_rect(int art_y)
{
	return now_mini_rect_at(SCREEN_TOP_W - SP_3, art_y + ART_SIZE);
}

/* Under the downloads manager's card, on its panel's right and bottom
 * edges, e4 from the screen's: 184, 184. */
static inline ui_rect now_mini_panel_rect(void)
{
	return now_mini_rect_at(SCREEN_TOP_W - SP_4, SCREEN_H - SP_4);
}

/* What goes inside the panel `p`. */
static inline ui_rect now_mini_cover_rect(ui_rect p)
{
	ui_rect r = { p.x + SP_1, p.y + SP_1, NOW_MINI_COVER, NOW_MINI_COVER };
	return r;
}

/* The state icon, centred on the cover's height, e2 from the right. */
static inline ui_rect now_mini_icon_rect(ui_rect p)
{
	ui_rect c = now_mini_cover_rect(p);
	ui_rect r = { p.x + p.w - SP_2 - ICON_S, c.y + (c.h - ICON_S) / 2,
	              ICON_S, ICON_S };
	return r;
}

/* The texts: e2 right of the cover, up to e2 before the icon. */
static inline int now_mini_text_x(ui_rect p)
{
	ui_rect c = now_mini_cover_rect(p);
	return c.x + c.w + SP_2;
}

/* The same wherever the panel goes: 124 px. */
static inline int now_mini_text_w(void)
{
	ui_rect p = now_mini_rect_at(NOW_MINI_W, NOW_MINI_H);
	return now_mini_icon_rect(p).x - SP_2 - now_mini_text_x(p);
}

/* The title (t_cuerpo) and the line under it (t_meta), centred as a block
 * on the cover's height: 12 and 27 px under the panel's top with lines of
 * 15 and 14. */
static inline int now_mini_title_cy(ui_rect p, int body_h, int meta_h)
{
	ui_rect c = now_mini_cover_rect(p);
	return c.y + (c.h - body_h - meta_h) / 2 + body_h / 2;
}

static inline int now_mini_line_cy(ui_rect p, int body_h, int meta_h)
{
	ui_rect c = now_mini_cover_rect(p);
	return c.y + (c.h - body_h - meta_h) / 2 + body_h + meta_h / 2;
}

/* The progress band, a bevel's height, on the panel's bottom inner rows:
 * the row between it and the cover stays empty. */
static inline ui_rect now_mini_band_rect(ui_rect p)
{
	ui_rect r = { p.x + OUTLINE_W, p.y + p.h - OUTLINE_W - BEVEL_W,
	              p.w - 2 * OUTLINE_W, BEVEL_W };
	return r;
}

/* The band's progress in whole pixels: full once the track finished. */
static inline int now_mini_band_px(bool finished, unsigned elapsed,
                                   unsigned duration)
{
	return player_fill_px(NOW_MINI_W - 2 * OUTLINE_W,
	                      player_progress_permille(finished, elapsed,
	                                               duration));
}

/* Where whatever shares the screen with the panel `p` must end: e2 above
 * it. */
static inline int now_mini_clear_bottom(ui_rect p)
{
	return p.y - SP_2;
}

/* Lines of an artist's biography on the card: one fewer over the mini
 * player, so the block (listeners and four lines, 106 px) ends e2 above
 * it. */
#define NOW_MINI_BIO_LINES (ARTIST_BIO_LINES - 1)

static inline int now_detail_bio_lines(bool mini)
{
	return mini ? NOW_MINI_BIO_LINES : ARTIST_BIO_LINES;
}

#endif
