/* Layout, focus, scrolling and hit testing of a grid of tiles
 * (docs/STYLES.md 5.16, 6.6 and 6.9). Pure C with no 3DS headers, tested in
 * tests/native_logic_test.c. Whole pixels throughout.
 *
 * The tiles fill the bottom screen's width less SP_2 each side (304 px) in
 * 1, 2 or 3 columns, GRID_GAP apart both ways; the remainder of the
 * division goes to the first columns:
 *   1 column   304            x 8
 *   2 columns  150 150        x 8 162   (the player's halves, 7.5)
 *   3 columns  99 99 98       x 8 111 214
 * Rows are TILE_H high, GRID_PITCH from one to the next. A grid shows
 * `rows` of them from `top`; its area (where a drag may start, and what the
 * bars around it cover) runs from SP_1 above the first row to SP_1 below
 * the last. Five rows from y 36 fill the 184 px between the bars exactly,
 * as the four result rows do.
 *
 * `scroll` is the pixel offset of the grid; the first (possibly cut)
 * visible row is scroll / GRID_PITCH. A drag may leave rows cut; the
 * buttons bring the grid back to whole rows (6.6, 6.9). */
#ifndef GRID_LOGIC_H
#define GRID_LOGIC_H

#include "layout.h"
#include "list_logic.h"
#include "theme.h"

#include <stdbool.h>

#define GRID_MAX_COLS 3
#define GRID_WIDTH (SCREEN_BOTTOM_W - 2 * SP_2)
#define GRID_PITCH (TILE_H + GRID_GAP)

typedef struct {
	int cols;              /* 1..GRID_MAX_COLS */
	int top;               /* y of the first row at scroll 0 */
	int rows;              /* rows on screen */
} grid_geom;

static inline int grid_cols(const grid_geom *g)
{
	return g->cols < 1 ? 1 : g->cols > GRID_MAX_COLS ? GRID_MAX_COLS : g->cols;
}

/* Rows needed for `count` tiles. */
static inline int grid_rows(const grid_geom *g, int count)
{
	int cols = grid_cols(g);
	return count > 0 ? (count + cols - 1) / cols : 0;
}

static inline int grid_col_width(const grid_geom *g, int col)
{
	int cols = grid_cols(g);
	int avail = GRID_WIDTH - (cols - 1) * GRID_GAP;
	return avail / cols + (col < avail % cols ? 1 : 0);
}

static inline int grid_col_x(const grid_geom *g, int col)
{
	int x = SP_2;
	for (int i = 0; i < col; i++)
		x += grid_col_width(g, i) + GRID_GAP;
	return x;
}

/* Where a drag may start and what the bars leave visible. */
static inline ui_rect grid_area(const grid_geom *g)
{
	ui_rect r = { 0, g->top - SP_1, SCREEN_BOTTOM_W, g->rows * GRID_PITCH };
	return r;
}

static inline bool grid_area_contains(const grid_geom *g, int x, int y)
{
	return ui_rect_contains(grid_area(g), x, y);
}

/* Tile `index` with the grid scrolled by `scroll` px. It may lie partly or
 * wholly outside the area. */
static inline ui_rect grid_cell_rect(const grid_geom *g, int index, int scroll)
{
	int cols = grid_cols(g);
	int row = index / cols, col = index % cols;
	ui_rect r = { grid_col_x(g, col), g->top + row * GRID_PITCH - scroll,
	              grid_col_width(g, col), TILE_H };
	return r;
}

/* The tile under (x, y), or -1: inside the area, tiles only, not the gaps. */
static inline int grid_hit(const grid_geom *g, int count, int x, int y,
                           int scroll)
{
	if (!grid_area_contains(g, x, y))
		return -1;
	int offset = y - g->top + scroll;
	if (offset < 0)
		return -1;
	int row = offset / GRID_PITCH;
	int cols = grid_cols(g);
	for (int col = 0; col < cols; col++) {
		int index = row * cols + col;
		if (index >= count)
			return -1;
		if (ui_rect_contains(grid_cell_rect(g, index, scroll), x, y))
			return index;
	}
	return -1;
}

/* The furthest the grid scrolls: its last row at the bottom. */
static inline int grid_max_scroll(const grid_geom *g, int count)
{
	int rows = grid_rows(g, count);
	return rows > g->rows ? (rows - g->rows) * GRID_PITCH : 0;
}

/* The whole row nearest to `scroll`. */
static inline int grid_snap(const grid_geom *g, int scroll, int count)
{
	int first = (scroll + GRID_PITCH / 2) / GRID_PITCH;
	int max_first = grid_max_scroll(g, count) / GRID_PITCH;
	if (first > max_first)
		first = max_first;
	return first < 0 ? 0 : first;
}

/* Whether row `row` is wholly inside the area at `scroll`. */
static inline bool grid_row_whole(const grid_geom *g, int row, int scroll)
{
	int y = g->top + row * GRID_PITCH - scroll;
	ui_rect area = grid_area(g);
	return y >= area.y && y + TILE_H <= area.y + area.h;
}

/* The first visible row once tile `selected` has to be whole on screen,
 * from a grid left anywhere (cut by a drag too): the least movement, on
 * whole rows (6.6). */
static inline int grid_align(const grid_geom *g, int scroll, int selected,
                             int count)
{
	int row = selected / grid_cols(g);
	return list_scroll(grid_snap(g, scroll, count), row, grid_rows(g, count),
	                   g->rows);
}

/* The D-pad on a grid (6.6). Up/down change rows and keep the column; when
 * the row below is shorter, the focus lands on its last tile. Left/right
 * change columns within the row; with one column they jump a page, as in
 * the results. Nothing wraps: at an edge the focus stays and `*dx`/`*dy`
 * say which way the tile nudges (-1, 0 or 1). */
typedef enum { GRID_UP, GRID_DOWN, GRID_LEFT, GRID_RIGHT } grid_dir;

static inline int grid_move(const grid_geom *g, int count, int selected,
                            grid_dir dir, int *dx, int *dy)
{
	*dx = *dy = 0;
	if (count <= 0)
		return 0;
	int cols = grid_cols(g);
	selected = list_clamp(selected, count);
	int row = selected / cols, col = selected % cols;
	int last_row = (count - 1) / cols;
	int next = selected;
	switch (dir) {
	case GRID_UP:
		if (row > 0)
			next = selected - cols;
		else
			*dy = -1;
		break;
	case GRID_DOWN:
		if (row < last_row) {
			next = selected + cols;
			if (next > count - 1)
				next = count - 1;
		} else {
			*dy = 1;
		}
		break;
	case GRID_LEFT:
	case GRID_RIGHT: {
		int sign = dir == GRID_LEFT ? -1 : 1;
		if (cols == 1) {
			bool bumped;
			next = list_move(selected, sign * g->rows, count, &bumped);
			if (bumped)
				*dy = sign;
		} else if (dir == GRID_LEFT ? col > 0
		                            : col < cols - 1 && selected + 1 < count) {
			next = selected + sign;
		} else {
			*dx = sign;
		}
		break;
	}
	}
	return next;
}

/* The scroll for the stylus at `y` in a drag begun with list_drag_begin
 * (6.9): the grid moves with it, never past either end. */
static inline int grid_drag_scroll(const grid_geom *g, const list_drag *d,
                                   int y, int count)
{
	int scroll = d->start_scroll - (y - d->start_y);
	int max = grid_max_scroll(g, count);
	if (scroll > max)
		scroll = max;
	return scroll < 0 ? 0 : scroll;
}

/* The scroll indicator (5.11): against the right edge, along the area. */
static inline ui_rect grid_scroll_track(const grid_geom *g)
{
	ui_rect area = grid_area(g);
	ui_rect r = { SCREEN_BOTTOM_W - SCROLL_W, area.y, SCROLL_W, area.h };
	return r;
}

static inline ui_rect grid_scroll_thumb(const grid_geom *g, int scroll,
                                        int count)
{
	ui_rect track = grid_scroll_track(g);
	int rows = grid_rows(g, count);
	if (rows <= g->rows)
		return track;
	int h = track.h * g->rows / rows;
	if (h < SCROLL_MIN_H)
		h = SCROLL_MIN_H;
	int max = grid_max_scroll(g, count);
	if (scroll < 0)
		scroll = 0;
	if (scroll > max)
		scroll = max;
	int travel = track.h - h;
	ui_rect r = { track.x, track.y + (travel * scroll + max / 2) / max,
	              track.w, h };
	return r;
}

/* A tile's cover: TILE_COVER square, SP_1 from its left, top and bottom. */
static inline ui_rect grid_cover_rect(ui_rect tile)
{
	ui_rect r = { tile.x + SP_1, tile.y + SP_1, TILE_COVER, TILE_COVER };
	return r;
}

/* A tile's text: SP_2 right of the cover, SP_2 from the right edge. */
static inline int grid_text_x(ui_rect tile)
{
	return tile.x + SP_1 + TILE_COVER + SP_2;
}

static inline int grid_text_width(const grid_geom *g, int col)
{
	return grid_col_width(g, col) - SP_1 - TILE_COVER - 2 * SP_2;
}

#endif
