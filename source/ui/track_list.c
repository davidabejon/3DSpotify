#include "track_list.h"

#include "draw.h"
#include "tabbar.h"
#include "thumbs.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

/* Title, artist and duration of every row, plus the numbers. */
#define ROW_GLYPHS     3200
#define SCRATCH_GLYPHS UI_FIELD_SIZE

void track_list_thumbs_frame(track_list *l)
{
	if (l->style == TRACK_LIST_THUMBS)
		thumbs_client_frame(&l->thumbs);
}

void track_list_offer_thumbs(track_list *l)
{
	if (l->style == TRACK_LIST_THUMBS)
		thumbs_client_offer(&l->thumbs);
}

void track_list_allow_thumbs(bool allow)
{
	thumbs_allow(allow);
}

/* --- Lifetime ---------------------------------------------------------- */

bool track_list_init(track_list *l, track_list_style style)
{
	memset(l, 0, sizeof(*l));
	l->style = style;
	l->parsed_lang = LANG_COUNT;
	l->stale = true;
	l->press_index = -1;
	l->touch.target = -1;
	l->buf = C2D_TextBufNew(ROW_GLYPHS);
	l->scratch = C2D_TextBufNew(SCRATCH_GLYPHS);
	if (!l->buf || !l->scratch) {
		track_list_exit(l);
		return false;
	}
	return true;
}

void track_list_exit(track_list *l)
{
	thumbs_client_release(&l->thumbs);
	if (l->buf)
		C2D_TextBufDelete(l->buf);
	if (l->scratch)
		C2D_TextBufDelete(l->scratch);
	l->buf = l->scratch = NULL;
}

void track_list_reset_input(track_list *l)
{
	press_reset(&l->press);
	l->press_index = -1;
	l->touch.target = -1;
	l->drag.down = l->drag.dragging = false;
	l->edge_frames = 0;
}

/* Hands thumbs.c the covers of the window. Keeping ownership, so the
 * rows on screen keep theirs while the window moves. */
static void offer_window(track_list *l, bool replace)
{
	const char *urls[TRACK_LIST_WINDOW];
	int n = l->count - l->base;
	if (n > TRACK_LIST_WINDOW)
		n = TRACK_LIST_WINDOW;
	if (n < 0)
		n = 0;
	for (int i = 0; i < n; i++)
		urls[i] = l->tracks[l->base + i].art_small;
	if (replace)
		thumbs_client_replace(&l->thumbs, urls, n);
	else
		thumbs_client_set(&l->thumbs, urls, n);
}

/* The window follows the rows on screen; the fetch priority follows the
 * scroll. */
static void follow_window(track_list *l)
{
	int base = list_window_base(l->base, l->first, l->count,
	                            TRACK_LIST_WINDOW);
	if (base != l->base) {
		l->base = base;
		l->stale = true;
		if (l->style == TRACK_LIST_THUMBS)
			offer_window(l, true);
	}
	if (l->style == TRACK_LIST_THUMBS)
		thumbs_client_window(&l->thumbs, l->first - l->base);
}

void track_list_set(track_list *l, const sp_track *tracks, int count,
                    int selected)
{
	if (!tracks)
		count = 0;
	l->tracks = tracks;
	l->count = count < 0 ? 0 : count > TRACK_LIST_MAX ? TRACK_LIST_MAX
	                                                  : count;
	l->selected = list_clamp(selected, l->count);
	l->first = list_scroll(0, l->selected, l->count, RESULTS_PER_PAGE);
	l->scroll = l->first * LIST_PITCH;
	l->base = list_window_base(0, l->first, l->count, TRACK_LIST_WINDOW);
	/* New tracks: thumbs.c is handed their covers by the next claim. */
	offer_window(l, false);
	thumbs_client_window(&l->thumbs, l->first - l->base);
	l->stale = true;
	track_list_reset_input(l);
}

void track_list_scroll_to(track_list *l, int first)
{
	int last = list_max_scroll(l->count) / LIST_PITCH;
	l->first = first < 0 ? 0 : first > last ? last : first;
	l->scroll = l->first * LIST_PITCH;
	/* The focus is never left out of view by this. */
	if (!list_row_whole(l->selected, l->scroll, l->count))
		l->first = list_align(l->scroll, l->selected, l->count);
	l->scroll = l->first * LIST_PITCH;
	follow_window(l);
}

void track_list_extend(track_list *l, int count)
{
	int old = l->count;
	l->count = count < old ? old : count > TRACK_LIST_MAX ? TRACK_LIST_MAX
	                                                      : count;
	if (l->count == old)
		return;
	/* New rows inside the window need their text and covers. */
	if (old < l->base + TRACK_LIST_WINDOW) {
		l->stale = true;
		if (l->style == TRACK_LIST_THUMBS)
			offer_window(l, true);
	}
	follow_window(l);
}

/* --- Text -------------------------------------------------------------- */

static void parse(C2D_Text *text, C2D_TextBuf buf, const char *string)
{
	C2D_TextParse(text, buf, string);
	C2D_TextOptimize(text);
}

static const char *track_name(const sp_track *t)
{
	return t->name[0] ? t->name : tr(STR_UNTITLED);
}

static const char *track_artist(const sp_track *t)
{
	return t->artist[0] ? t->artist : tr(STR_UNKNOWN_ARTIST);
}

/* Width of a row's text column. */
static int row_text_width(void)
{
	ui_rect row = list_row_rect(0);
	return row.x + row.w - SP_2 - list_text_x(row);
}

bool track_list_text_stale(const track_list *l)
{
	return l->stale || l->parsed_lang != lang_get();
}

void track_list_ensure_parsed(track_list *l)
{
	if (!track_list_text_stale(l))
		return;
	C2D_TextBufClear(l->buf);
	int width = row_text_width();
	int n = l->count - l->base;
	if (n > TRACK_LIST_WINDOW)
		n = TRACK_LIST_WINDOW;
	for (int i = 0; i < n; i++) {
		const sp_track *t = &l->tracks[l->base + i];
		char text[16];
		list_format_duration(t->duration_ms, text, sizeof(text));
		parse(&l->duration[i], l->buf, text);
		l->duration_width[i] = ui_text_width(&l->duration[i], TXT_META);
		/* The duration is never cut (5.3): the title makes room. */
		int title_width = width - ui_px_ceil(l->duration_width[i]) - SP_2;
		ui_parse_fitted(&l->title[i], l->buf, l->scratch, track_name(t),
		                TXT_CABEZA, title_width);
		ui_parse_fitted(&l->artist[i], l->buf, l->scratch, track_artist(t),
		                TXT_CUERPO, width);
		if (l->style == TRACK_LIST_NUMBERS) {
			snprintf(text, sizeof(text), "%d", l->base + i + 1);
			parse(&l->number[i], l->buf, text);
		}
	}
	l->parsed_lang = lang_get();
	l->stale = false;
}

/* --- State ------------------------------------------------------------- */

track_list_look track_list_get_look(const track_list *l)
{
	return (track_list_look){
		.selected = l->selected,
		.first = l->first,
		.scroll = l->scroll,
		.pressed = press_down(&l->press) ? l->press_index : -1,
		.edge = l->edge_frames > 0 ? l->edge_dir * EDGE_SHIFT : 0,
		.thumbs = l->thumbs.serial,
	};
}

bool track_list_same_look(const track_list_look *a, const track_list_look *b)
{
	return a->selected == b->selected && a->first == b->first &&
	       a->scroll == b->scroll && a->pressed == b->pressed &&
	       a->edge == b->edge && a->thumbs == b->thumbs;
}

void track_list_tick(track_list *l)
{
	press_tick(&l->press);
	if (!press_down(&l->press))
		l->press_index = -1;
	if (l->edge_frames > 0)
		l->edge_frames--;
}

/* --- Input ------------------------------------------------------------- */

/* The buttons: focus `index` and bring the list to whole rows with it in
 * view (6.6), even when a drag left it cut. */
void track_list_focus(track_list *l, int index)
{
	l->selected = list_clamp(index, l->count);
	l->first = list_align(l->scroll, l->selected, l->count);
	l->scroll = l->first * LIST_PITCH;
	follow_window(l);
}

/* 6.9: the list follows the stylus pixel for pixel. */
static void drag_to(track_list *l, int y)
{
	l->scroll = list_drag_scroll(&l->drag, y, l->count);
	int now_first = l->scroll / LIST_PITCH;
	if (now_first != l->first) {
		l->first = now_first;
		follow_window(l);
	}
}

/* 6.9: on lifting the stylus the list stays where it is, rows cut or not,
 * and the focus stays on its row even if that is now out of view. */
static void drag_end(track_list *l)
{
	l->drag.down = l->drag.dragging = false;
}

/* 6.6: the focus moves in the same frame; at an end, the focused row
 * nudges towards it instead. */
static void move_focus(track_list *l, int delta)
{
	bool bumped;
	int next = list_move(l->selected, delta, l->count, &bumped);
	/* Whole rows again, bumped or not. */
	track_list_focus(l, next);
	if (bumped) {
		l->edge_dir = delta < 0 ? -1 : 1;
		l->edge_frames = ANIM_EDGE;
	}
}

track_list_touch track_list_update_touch(track_list *l, u32 down, u32 held,
                                         u32 up)
{
	if (up & KEY_TOUCH) {
		bool was_drag = l->drag.dragging;
		l->drag.down = false;
		if (was_drag) {
			drag_end(l);
			return TRACK_LIST_BUSY;
		}
	} else if (held & KEY_TOUCH) {
		touchPosition pos;
		hidTouchRead(&pos);
		if (down & KEY_TOUCH) {
			/* Only from the list itself: the tab bar keeps its own
			 * touches. */
			if (list_area_contains(pos.px, pos.py) && !tabbar_touching())
				list_drag_begin(&l->drag, pos.py, l->scroll);
		} else if (list_drag_update(&l->drag, pos.py)) {
			/* A drag is not a tap: the row returns to rest without
			 * acting (6.4, 6.9). */
			if (l->touch.target >= 0) {
				press_cancel(&l->press);
				l->touch.target = -1;
			}
			drag_to(l, pos.py);
			return TRACK_LIST_BUSY;
		}
	}

	int hit = -1;
	if (held & KEY_TOUCH) {
		touchPosition pos;
		hidTouchRead(&pos);
		hit = list_hit_row(pos.px, pos.py, l->scroll, l->count);
	}
	int control;
	touch_event event = touch_track_update(&l->touch, (down & KEY_TOUCH) != 0,
	                                       (held & KEY_TOUCH) != 0,
	                                       (up & KEY_TOUCH) != 0, hit,
	                                       &control);
	switch (event) {
	case TOUCH_PRESS:
		press_begin(&l->press);
		l->press_index = control;
		break;
	case TOUCH_CANCEL:
		press_cancel(&l->press);
		break;
	case TOUCH_ACTIVATE:
		press_end(&l->press);
		if (control == l->selected)
			return TRACK_LIST_ACTIVATE;   /* tap the focused row */
		l->selected = control;  /* tap another: focus, list stays put */
		break;
	case TOUCH_NONE:
		break;
	}
	return l->touch.target >= 0 ? TRACK_LIST_BUSY : TRACK_LIST_IDLE;
}

bool track_list_realign(track_list *l, u32 down, u32 confirm_keys)
{
	u32 keys = KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | confirm_keys;
	if (!(down & keys) || list_row_whole(l->selected, l->scroll, l->count))
		return false;
	track_list_focus(l, l->selected);
	return true;
}

void track_list_navigate(track_list *l, u32 down)
{
	if (down & KEY_UP)
		move_focus(l, -1);
	else if (down & KEY_DOWN)
		move_focus(l, 1);
	else if (down & KEY_LEFT)
		move_focus(l, -RESULTS_PER_PAGE);
	else if (down & KEY_RIGHT)
		move_focus(l, RESULTS_PER_PAGE);
}

press_state *track_list_focused_press(track_list *l)
{
	l->press_index = l->selected;
	return &l->press;
}

void track_list_release(track_list *l)
{
	if (l->touch.target < 0)
		press_end(&l->press);
}

/* --- Drawing ----------------------------------------------------------- */

/* 5.3: the cover inside a 1 px `tinta` outline, or, while it loads or when
 * there is none, `capa_alta` with the note icon. Same size either way, so
 * nothing moves when the image arrives. `index` < 0: a skeleton row. */
static void draw_thumb(const track_list *l, ui_rect row, int index)
{
	C2D_Image image;
	bool shown = l && thumbs_client_image(&l->thumbs, index - l->base,
	                                      &image);
	ui_art(list_thumb_rect(row), shown ? &image : NULL);
}

/* The numbered variant of the square: same box and outline, `capa_alta`,
 * the position in `t_meta` (an album: every row shares the cover). */
static void draw_number(const track_list *l, ui_rect row, int index)
{
	ui_rect box = list_thumb_rect(row);
	ui_fill(box, COL_TINTA);
	ui_fill(ui_rect_inset(box, OUTLINE_W), COL_CAPA_ALTA);
	ui_text_centered(&l->number[index - l->base], box.x + box.w / 2,
	                 box.y + box.h / 2, TXT_META, COL_TEXTO_2);
}

/* 5.3. `index` < 0 draws an empty skeleton row. */
static void draw_row(const track_list *l, ui_rect row, int index,
                     const track_list_look *look)
{
	bool focused = index >= 0 && index == look->selected;
	bool pressed = index >= 0 && index == look->pressed;
	if (focused)
		row = ui_rect_offset(row, 0, look->edge);
	u32 title_ink, meta_ink;
	row = ui_item_frame(row, focused, pressed, &title_ink, &meta_ink);
	if (index >= 0 && l->style == TRACK_LIST_NUMBERS)
		draw_number(l, row, index);
	else
		draw_thumb(l, row, index);
	if (index < 0)
		return;

	/* Rows on screen are always inside the window (list_window_base). */
	int w = index - l->base;
	if (w < 0 || w >= TRACK_LIST_WINDOW)
		return;
	int x = list_text_x(row);
	int title_cy = row.y + SP_1 + ui_line_height(TXT_CABEZA) / 2;
	int artist_cy = row.y + row.h - SP_1 - ui_line_height(TXT_CUERPO) / 2;
	ui_text(&l->title[w], x, title_cy, TXT_CABEZA, title_ink);
	ui_text(&l->artist[w], x, artist_cy, TXT_CUERPO, meta_ink);
	int duration_x = row.x + row.w - SP_2 - ui_px_ceil(l->duration_width[w]);
	ui_text(&l->duration[w], duration_x, title_cy, TXT_META, meta_ink);
}

void track_list_draw(const track_list *l, const track_list_look *look)
{
	/* Mid-drag one more row peeks in at the bottom. The focused row goes
	 * last, so its nudge draws over its neighbours. */
	int last = look->first + RESULTS_PER_PAGE;
	if (last > l->count - 1)
		last = l->count - 1;
	for (int index = look->first; index <= last; index++)
		if (index != look->selected)
			draw_row(l, list_row_at(index, look->scroll), index, look);
	if (look->selected >= look->first && look->selected <= last)
		draw_row(l, list_row_at(look->selected, look->scroll),
		         look->selected, look);
	/* 5.11: only when the list does not fit. */
	if (l->count > RESULTS_PER_PAGE) {
		ui_fill(list_scroll_track(), COL_CAPA);
		ui_fill(list_scroll_thumb(look->scroll, l->count), COL_CLOROFILA);
	}
}

void track_list_draw_skeleton(void)
{
	track_list_look none = { .selected = -1, .pressed = -1 };
	for (int slot = 0; slot < RESULTS_PER_PAGE; slot++)
		draw_row(NULL, list_row_rect(slot), -1, &none);
}
