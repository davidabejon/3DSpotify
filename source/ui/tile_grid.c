#include "tile_grid.h"

#include "offline_logic.h"

#include "draw.h"
#include "icons.h"
#include "tabbar.h"
#include "theme.h"

#include <string.h>

/* One fitted name per tile. */
#define NAME_GLYPHS    (TILE_GRID_MAX * 64)
#define SCRATCH_GLYPHS UI_FIELD_SIZE

/* --- Lifetime ---------------------------------------------------------- */

bool tile_grid_init(tile_grid *g, grid_geom geom)
{
	memset(g, 0, sizeof(*g));
	g->geom = geom;
	g->parsed_lang = LANG_COUNT;
	g->stale = true;
	g->press_index = -1;
	g->touch.target = -1;
	g->has_focus = true;
	g->buf = C2D_TextBufNew(NAME_GLYPHS);
	g->scratch = C2D_TextBufNew(SCRATCH_GLYPHS);
	if (!g->buf || !g->scratch) {
		tile_grid_exit(g);
		return false;
	}
	return true;
}

void tile_grid_exit(tile_grid *g)
{
	thumbs_client_release(&g->thumbs);
	if (g->buf)
		C2D_TextBufDelete(g->buf);
	if (g->scratch)
		C2D_TextBufDelete(g->scratch);
	g->buf = g->scratch = NULL;
}

void tile_grid_reset_input(tile_grid *g)
{
	press_reset(&g->press);
	g->press_index = -1;
	g->touch.target = -1;
	g->drag.down = g->drag.dragging = false;
	g->edge_frames = 0;
}

int tile_grid_first_row(const tile_grid *g)
{
	return g->scroll / GRID_PITCH;
}

/* The fetch priority follows the scroll: the first visible tile. */
static void follow_window(tile_grid *g)
{
	thumbs_client_window(&g->thumbs,
	                     tile_grid_first_row(g) * grid_cols(&g->geom));
}

void tile_grid_set(tile_grid *g, const sp_playlist_ref *items, int count,
                   int selected)
{
	if (!items)
		count = 0;
	g->items = items;
	g->count = count < 0 ? 0 : count > TILE_GRID_MAX ? TILE_GRID_MAX : count;
	g->selected = list_clamp(selected, g->count);
	g->scroll = grid_align(&g->geom, 0, g->selected, g->count) * GRID_PITCH;
	const char *urls[TILE_GRID_MAX];
	for (int i = 0; i < g->count; i++)
		urls[i] = g->items[i].art;
	thumbs_client_set(&g->thumbs, urls, g->count);
	follow_window(g);
	g->stale = true;
	tile_grid_reset_input(g);
}

void tile_grid_focus(tile_grid *g, int index)
{
	g->selected = list_clamp(index, g->count);
	g->scroll = grid_align(&g->geom, g->scroll, g->selected, g->count) *
	            GRID_PITCH;
	follow_window(g);
}

void tile_grid_tick(tile_grid *g)
{
	press_tick(&g->press);
	if (!press_down(&g->press))
		g->press_index = -1;
	if (g->edge_frames > 0)
		g->edge_frames--;
}

void tile_grid_thumbs_frame(tile_grid *g)
{
	thumbs_client_frame(&g->thumbs);
}

/* --- Text -------------------------------------------------------------- */

bool tile_grid_text_stale(const tile_grid *g)
{
	return g->stale || g->parsed_lang != lang_get();
}

void tile_grid_ensure_parsed(tile_grid *g)
{
	if (!tile_grid_text_stale(g))
		return;
	C2D_TextBufClear(g->buf);
	int cols = grid_cols(&g->geom);
	for (int i = 0; i < g->count; i++) {
		/* Liked Songs and the Downloads are named in the current
		 * language, not by the feed. */
		const char *name = strcmp(g->items[i].uri, SP_LIKED_URI) == 0
		                   ? tr(STR_LIKED_SONGS)
		                   : strcmp(g->items[i].uri, DL_HOME_URI) == 0
		                   ? tr(STR_SETTINGS_DOWNLOADS)
		                   : g->items[i].name[0] ? g->items[i].name
		                                         : tr(STR_UNTITLED);
		ui_parse_fitted(&g->name[i], g->buf, g->scratch, name, TXT_CUERPO,
		                grid_text_width(&g->geom, i % cols));
	}
	g->parsed_lang = lang_get();
	g->stale = false;
}

/* --- State ------------------------------------------------------------- */

tile_grid_look tile_grid_get_look(const tile_grid *g)
{
	bool edge = g->edge_frames > 0;
	return (tile_grid_look){
		.selected = g->has_focus ? g->selected : -1,
		.scroll = g->scroll,
		.pressed = press_down(&g->press) ? g->press_index : -1,
		.edge_dx = edge ? g->edge_dx * EDGE_SHIFT : 0,
		.edge_dy = edge ? g->edge_dy * EDGE_SHIFT : 0,
		.thumbs = g->thumbs.serial,
	};
}

bool tile_grid_same_look(const tile_grid_look *a, const tile_grid_look *b)
{
	return a->selected == b->selected && a->scroll == b->scroll &&
	       a->pressed == b->pressed && a->edge_dx == b->edge_dx &&
	       a->edge_dy == b->edge_dy && a->thumbs == b->thumbs;
}

/* --- Input ------------------------------------------------------------- */

static grid_dir key_dir(u32 down, bool *any)
{
	*any = true;
	if (down & KEY_UP)
		return GRID_UP;
	if (down & KEY_DOWN)
		return GRID_DOWN;
	if (down & KEY_LEFT)
		return GRID_LEFT;
	if (down & KEY_RIGHT)
		return GRID_RIGHT;
	*any = false;
	return GRID_UP;
}

bool tile_grid_can_move(const tile_grid *g, grid_dir dir)
{
	int dx, dy;
	return grid_move(&g->geom, g->count, g->selected, dir, &dx, &dy) !=
	       g->selected;
}

void tile_grid_navigate(tile_grid *g, u32 down)
{
	bool any;
	grid_dir dir = key_dir(down, &any);
	if (!any || g->count == 0)
		return;
	int dx, dy;
	int next = grid_move(&g->geom, g->count, g->selected, dir, &dx, &dy);
	/* Whole rows again, bumped or not (6.6). */
	tile_grid_focus(g, next);
	if (dx || dy) {
		g->edge_dx = dx;
		g->edge_dy = dy;
		g->edge_frames = ANIM_EDGE;
	}
}

bool tile_grid_realign(tile_grid *g, u32 down, u32 confirm_keys)
{
	u32 keys = KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | confirm_keys;
	int row = g->selected / grid_cols(&g->geom);
	if (!(down & keys) || g->count == 0 ||
	    grid_row_whole(&g->geom, row, g->scroll))
		return false;
	tile_grid_focus(g, g->selected);
	return true;
}

press_state *tile_grid_focused_press(tile_grid *g)
{
	g->press_index = g->selected;
	return &g->press;
}

void tile_grid_release(tile_grid *g)
{
	if (g->touch.target < 0)
		press_end(&g->press);
}

tile_grid_touch tile_grid_update_touch(tile_grid *g, u32 down, u32 held,
                                       u32 up)
{
	if (up & KEY_TOUCH) {
		bool was_drag = g->drag.dragging;
		g->drag.down = false;
		if (was_drag) {
			g->drag.dragging = false;
			return TILE_GRID_BUSY;
		}
	} else if (held & KEY_TOUCH) {
		touchPosition pos;
		hidTouchRead(&pos);
		if (down & KEY_TOUCH) {
			if (grid_area_contains(&g->geom, pos.px, pos.py) &&
			    !tabbar_touching())
				list_drag_begin(&g->drag, pos.py, g->scroll);
		} else if (grid_max_scroll(&g->geom, g->count) > 0 &&
		           list_drag_update(&g->drag, pos.py)) {
			/* A drag is not a tap: the tile returns to rest without
			 * acting (6.4, 6.9). */
			if (g->touch.target >= 0) {
				press_cancel(&g->press);
				g->touch.target = -1;
			}
			int scroll = grid_drag_scroll(&g->geom, &g->drag, pos.py,
			                              g->count);
			if (scroll != g->scroll) {
				g->scroll = scroll;
				follow_window(g);
			}
			return TILE_GRID_BUSY;
		}
	}

	int hit = -1;
	if (held & KEY_TOUCH) {
		touchPosition pos;
		hidTouchRead(&pos);
		hit = grid_hit(&g->geom, g->count, pos.px, pos.py, g->scroll);
	}
	int control;
	touch_event event = touch_track_update(&g->touch, (down & KEY_TOUCH) != 0,
	                                       (held & KEY_TOUCH) != 0,
	                                       (up & KEY_TOUCH) != 0, hit,
	                                       &control);
	switch (event) {
	case TOUCH_PRESS:
		press_begin(&g->press);
		g->press_index = control;
		break;
	case TOUCH_CANCEL:
		press_cancel(&g->press);
		break;
	case TOUCH_ACTIVATE:
		press_end(&g->press);
		if (g->has_focus && control == g->selected)
			return TILE_GRID_ACTIVATE;
		/* Another tile, or the grid without the focus: focus it, the
		 * grid stays put. */
		g->selected = control;
		g->has_focus = true;
		return TILE_GRID_FOCUSED;
	case TOUCH_NONE:
		break;
	}
	return g->touch.target >= 0 ? TILE_GRID_BUSY : TILE_GRID_IDLE;
}

/* --- Drawing ----------------------------------------------------------- */

/* 5.16. `index` < 0 draws a skeleton tile. */
static void draw_tile(const tile_grid *g, ui_rect tile, int index,
                      const tile_grid_look *look)
{
	bool focused = index >= 0 && index == look->selected;
	bool pressed = index >= 0 && index == look->pressed;
	if (focused)
		tile = ui_rect_offset(tile, look->edge_dx, look->edge_dy);
	u32 ink, meta_ink;
	tile = ui_item_frame(tile, focused, pressed, &ink, &meta_ink);
	C2D_Image image;
	bool shown = g && thumbs_client_image(&g->thumbs, index, &image);
	bool liked = g && index >= 0 && strcmp(g->items[index].uri,
	                                       SP_LIKED_URI) == 0;
	bool saved = g && index >= 0 && strcmp(g->items[index].uri,
	                                       DL_HOME_URI) == 0;
	ui_art_or(grid_cover_rect(tile), shown ? &image : NULL,
	          liked ? ICON_HEART : saved ? ICON_DOWNLOAD : ICON_NOTE);
	if (index < 0)
		return;
	ui_text(&g->name[index], grid_text_x(tile), tile.y + tile.h / 2,
	        TXT_CUERPO, ink);
}

void tile_grid_draw(const tile_grid *g, const tile_grid_look *look)
{
	const grid_geom *geom = &g->geom;
	int cols = grid_cols(geom);
	/* Mid-drag one more row peeks in at the bottom. The focused tile goes
	 * last, so its nudge draws over its neighbours. */
	int first = (look->scroll / GRID_PITCH) * cols;
	int last = first + (geom->rows + 1) * cols - 1;
	if (last > g->count - 1)
		last = g->count - 1;
	for (int i = first; i <= last; i++)
		if (i != look->selected)
			draw_tile(g, grid_cell_rect(geom, i, look->scroll), i, look);
	if (look->selected >= first && look->selected <= last)
		draw_tile(g, grid_cell_rect(geom, look->selected, look->scroll),
		          look->selected, look);
	/* 5.11: only when the grid does not fit. */
	if (grid_max_scroll(geom, g->count) > 0) {
		ui_fill(grid_scroll_track(geom), COL_CAPA);
		ui_fill(grid_scroll_thumb(geom, look->scroll, g->count),
		        COL_CLOROFILA);
	}
}

void tile_grid_draw_skeleton(grid_geom geom, int count)
{
	tile_grid_look none = { .selected = -1, .pressed = -1 };
	int max = geom.rows * grid_cols(&geom);
	if (count > max)
		count = max;
	for (int i = 0; i < count; i++)
		draw_tile(NULL, grid_cell_rect(&geom, i, 0), -1, &none);
}
