#include "view_downloads.h"

#include "dl_store.h"
#include "downloads.h"
#include "downloads_view_logic.h"
#include "draw.h"
#include "icons.h"
#include "lang.h"
#include "list_logic.h"
#include "press_logic.h"
#include "tabbar.h"
#include "theme.h"
#include "thumbs.h"
#include "ui_font.h"

#include <citro2d.h>

#include <stdio.h>
#include <string.h>

/* The entries kept ready at once, text and covers: the thumbnail pool. */
#define WINDOW         THUMBS_MAX
#define ROW_GLYPHS     3200
#define STATIC_GLYPHS  640
#define TOP_GLYPHS     384
#define SCRATCH_GLYPHS UI_FIELD_SIZE
/* The share of the download under way, and the free space, are read again
 * this often (9). */
#define SHARE_FRAMES   60
#define FREE_FRAMES    600
/* "Press A again" waits this long for that press. */
#define CONFIRM_FRAMES (2 * BUBBLE_SHOW)

/* The button bar (STYLES.md 5.6). */
enum { GLYPH_A, GLYPH_X, GLYPH_Y, GLYPH_B, GLYPH_COUNT };
static const char *const glyph_letters[GLYPH_COUNT] = { "A", "X", "Y", "B" };
static const u32 glyph_colors[GLYPH_COUNT] = {
	COL_BTN_A, COL_BTN_X, COL_BTN_Y, COL_BTN_B
};

/* The list, rebuilt when the downloads change. */
static dlv_list list;
static dlv_done_item scratch_items[DL_TRACKS_MAX + DL_COLLS_MAX];
static const dl_index *idx;
static unsigned built = ~0u;
static int focus = -1, scroll;
/* The window of entries with their text and covers: from `base`. */
static int base;
static bool rows_stale = true;
static char urls[WINDOW][80];
static thumbs_client thumbs;

/* Text. */
static C2D_TextBuf static_buf, row_buf, top_buf, scratch_buf;
static C2D_Text letter_text[GLYPH_COUNT];
static C2D_Text hint_cancel, hint_retry, hint_open, hint_remove, hint_pause,
                hint_resume, hint_back;
static C2D_Text section_text[DLV_SEC_COUNT], empty_text, notice_text;
static C2D_Text title_text[WINDOW], sub_text[WINDOW];
static C2D_Text share_text;
static C2D_Text top_title, top_summary, top_used, top_free, top_state;
static lang_id parsed_lang = LANG_COUNT;
static lang_id rows_lang = LANG_COUNT;
static int parsed_share = -1;
static str_id parsed_notice = STR_COUNT;

/* Interaction (STYLES.md 6). */
static press_state press_row;
static int press_index = -1;
static press_state press_glyph[GLYPH_COUNT];
static touch_track touch = { -1 };
static list_drag drag;
static int edge_frames, edge_dir;
static dlv_action pending = DLV_ACTION_NONE;
static const press_state *pending_press;
/* The bubble (5.9), and the second press it may be waiting for: of
 * `armed_key` on the entry named `armed_uri`. */
static int bubble_frames = -1;
static str_id notice_id = STR_COUNT;
static bool notice_ok;
static u32 armed_key;
static char armed_uri[SP_URI_MAX];
/* What DLV_ACTION_OPEN_* opens. */
static char target_uri[SP_URI_MAX];
static int target_track = -1;
static dl_coll_kind target_kind;

/* The share of the track downloading, as shown; the free space. */
static int share, share_frames;
static uint64_t free_bytes;
static int free_frames;

/* The top screen's card, as parsed and as drawn. */
typedef struct {
	dlv_summary s;
	uint64_t free_bytes;
	str_id state;
} top_card;
static top_card card, parsed_card, drawn_card;
static bool card_parsed, card_drawn;

typedef struct {
	int focus, scroll, pressed, edge, share, thumbs;
	unsigned built;
	bool glyph[GLYPH_COUNT];
	str_id bubble;
	bool paused;
} dlv_look;
static dlv_look drawn;
static bool drawn_valid;

/* --- Lifetime ---------------------------------------------------------- */

bool view_downloads_init(void)
{
	static_buf = C2D_TextBufNew(STATIC_GLYPHS);
	row_buf = C2D_TextBufNew(ROW_GLYPHS);
	top_buf = C2D_TextBufNew(TOP_GLYPHS);
	scratch_buf = C2D_TextBufNew(SCRATCH_GLYPHS);
	if (!static_buf || !row_buf || !top_buf || !scratch_buf) {
		view_downloads_exit();
		return false;
	}
	return true;
}

void view_downloads_exit(void)
{
	thumbs_client_release(&thumbs);
	C2D_TextBuf *bufs[] = { &static_buf, &row_buf, &top_buf, &scratch_buf };
	for (size_t i = 0; i < sizeof(bufs) / sizeof(bufs[0]); i++) {
		if (*bufs[i])
			C2D_TextBufDelete(*bufs[i]);
		*bufs[i] = NULL;
	}
}

/* --- The list ---------------------------------------------------------- */

/* The URI an entry stands for, or "". */
static const char *entry_uri(int i)
{
	if (!idx || i < 0 || i >= list.count)
		return "";
	const dlv_entry *e = &list.entries[i];
	switch (e->kind) {
	case DLV_TRACK:
		return idx->tracks[e->index].uri;
	case DLV_COLL:
		return idx->colls[e->index].uri;
	case DLV_PENDING: {
		const char *uri, *name, *art;
		dl_coll_kind kind;
		return downloads_pending_info(e->index, &uri, &name, &art, &kind)
		       ? uri : "";
	}
	default:
		return "";
	}
}

/* The window follows the entries on screen, centred on them when they
 * near either end of it. */
static void follow_window(void)
{
	int from, to;
	dlv_visible(&list, scroll, &from, &to);
	int next = base;
	if (list.count <= WINDOW)
		next = 0;
	else if (from < base + 2 || to > base + WINDOW - 3)
		next = from - (WINDOW - (to - from + 1)) / 2;
	if (next > list.count - WINDOW)
		next = list.count - WINDOW;
	if (next < 0)
		next = 0;
	if (next != base || rows_stale) {
		base = next;
		rows_stale = true;
	}
	thumbs_client_window(&thumbs, from - base);
}

/* Reads the index again; the focus stays on the same download if it is
 * still listed. */
static void rebuild(void)
{
	char kept[SP_URI_MAX];
	snprintf(kept, sizeof(kept), "%s", entry_uri(focus));
	int old = focus;
	idx = downloads_index();
	dlv_build(&list, idx, downloads_pending_count(), scratch_items);
	built = downloads_generation();
	focus = -1;
	for (int i = 0; kept[0] && i < list.count; i++)
		if (list.entries[i].kind != DLV_HEADER &&
		    strcmp(entry_uri(i), kept) == 0) {
			focus = i;
			break;
		}
	if (focus < 0)
		focus = dlv_clamp_focus(&list, old < 0 ? 0 : old);
	scroll = dlv_clamp_scroll(&list, scroll);
	if (focus >= 0 && !drag.dragging)
		scroll = dlv_scroll_to(&list, focus, scroll);
	rows_stale = true;
	follow_window();
	card.s = dlv_summarize(idx);
}

/* --- Text -------------------------------------------------------------- */

static void parse(C2D_Text *text, C2D_TextBuf buf, const char *string)
{
	ui_parse(text, buf, string);
	C2D_TextOptimize(text);
}

static void parse_static(void)
{
	C2D_TextBufClear(static_buf);
	for (int i = 0; i < GLYPH_COUNT; i++)
		parse(&letter_text[i], static_buf, glyph_letters[i]);
	parse(&hint_cancel, static_buf, tr(STR_HINT_CANCEL));
	parse(&hint_retry, static_buf, tr(STR_HINT_RETRY));
	parse(&hint_open, static_buf, tr(STR_HINT_OPEN));
	parse(&hint_remove, static_buf, tr(STR_HINT_REMOVE));
	parse(&hint_pause, static_buf, tr(STR_HINT_PAUSE_DOWNLOADS));
	parse(&hint_resume, static_buf, tr(STR_HINT_RESUME_DOWNLOADS));
	parse(&hint_back, static_buf, tr(STR_HINT_BACK));
	parse(&empty_text, static_buf, tr(STR_DLV_EMPTY));
	if (notice_id != STR_COUNT)
		parse(&notice_text, static_buf, tr(notice_id));
	char text[48];
	snprintf(text, sizeof(text), "%d%%", share);
	parse(&share_text, static_buf, text);
	parsed_lang = lang_get();
	parsed_notice = notice_id;
	parsed_share = share;
}

static int text_width(void)
{
	ui_rect row = list_row_rect(0);
	return row.x + row.w - SP_2 - list_text_x(row);
}

/* Room at the right of a row for its state: an icon, or "100%". */
#define STATE_W (ICON_S + SP_2)
#define SHARE_W 32

static void parse_rows(void)
{
	C2D_TextBufClear(row_buf);
	int width = text_width();
	static const str_id headers[DLV_SEC_COUNT] = {
		STR_DLV_SECTION_ACTIVE, STR_DLV_SECTION_QUEUED_FMT,
		STR_DLV_SECTION_FAILED_FMT, STR_DLV_SECTION_DONE_FMT
	};
	for (int s = 0; s < DLV_SEC_COUNT; s++) {
		char text[64];
		snprintf(text, sizeof(text), tr(headers[s]), list.rows[s]);
		parse(&section_text[s], row_buf, text);
	}
	const char *url_ptrs[WINDOW];
	int n = list.count - base;
	if (n > WINDOW)
		n = WINDOW;
	for (int w = 0; w < n; w++) {
		const dlv_entry *e = &list.entries[base + w];
		const char *title = "", *sub = "", *art = "";
		char line[128];
		line[0] = '\0';
		switch (e->kind) {
		case DLV_TRACK: {
			const dl_track *d = &idx->tracks[e->index];
			title = d->name[0] ? d->name : tr(STR_UNTITLED);
			sub = d->state == DL_FAILED && d->error[0] ? d->error
			      : d->artist[0] ? d->artist : tr(STR_UNKNOWN_ARTIST);
			art = d->art_small;
			break;
		}
		case DLV_COLL: {
			const dl_collection *c = &idx->colls[e->index];
			int done, count;
			dl_collection_progress(idx, e->index, &done, &count);
			title = c->name;
			if (e->section == DLV_SEC_QUEUED)
				snprintf(line, sizeof(line), tr(STR_DL_COLL_PROGRESS_FMT),
				         done, count);
			else
				snprintf(line, sizeof(line), tr(STR_DLV_TRACKS_FMT), done);
			sub = line;
			art = c->art;
			break;
		}
		case DLV_PENDING: {
			const char *uri, *name;
			dl_coll_kind kind;
			if (downloads_pending_info(e->index, &uri, &name, &art, &kind))
				title = name;
			sub = tr(STR_DL_COLL_WAITING);
			break;
		}
		default:
			break;
		}
		snprintf(urls[w], sizeof(urls[w]), "%s", art ? art : "");
		url_ptrs[w] = urls[w];
		if (e->kind == DLV_HEADER)
			continue;
		int side = e->section == DLV_SEC_ACTIVE ? SHARE_W : STATE_W;
		ui_parse_fitted(&title_text[w], row_buf, scratch_buf, title,
		                TXT_CABEZA, width - side);
		/* The download under way shares its second line with a bar. */
		int sub_w = e->section == DLV_SEC_ACTIVE ? width / 2 : width;
		ui_parse_fitted(&sub_text[w], row_buf, scratch_buf, sub, TXT_CUERPO,
		                sub_w);
	}
	thumbs_client_replace(&thumbs, url_ptrs, n);
	rows_stale = false;
	rows_lang = lang_get();
}

static void ensure_parsed(void)
{
	if (parsed_lang != lang_get() || parsed_notice != notice_id ||
	    parsed_share != share)
		parse_static();
	if (rows_stale || rows_lang != lang_get())
		parse_rows();
}

/* --- State ------------------------------------------------------------- */

static bool armed(u32 key, const char *uri)
{
	return bubble_frames >= 0 && armed_key == key &&
	       strcmp(armed_uri, uri) == 0;
}

static void notice(str_id text, bool ok)
{
	notice_id = text;
	notice_ok = ok;
	bubble_frames = 0;
	armed_key = 0;
}

/* The bubble that asks for a second press of `key` on `uri`. */
static void ask(str_id text, u32 key, const char *uri)
{
	notice(text, true);
	armed_key = key;
	snprintf(armed_uri, sizeof(armed_uri), "%s", uri);
}

static dlv_look current_look(void)
{
	dlv_look look = {
		.focus = focus,
		.scroll = scroll,
		.pressed = press_down(&press_row) ? press_index : -1,
		.edge = edge_frames > 0 ? edge_dir * EDGE_SHIFT : 0,
		.share = share,
		.thumbs = thumbs.serial,
		.built = built,
		.bubble = bubble_frames >= 0 ? notice_id : STR_COUNT,
		.paused = downloads_paused(),
	};
	for (int i = 0; i < GLYPH_COUNT; i++)
		look.glyph[i] = press_down(&press_glyph[i]);
	return look;
}

static bool same_look(const dlv_look *a, const dlv_look *b)
{
	if (a->focus != b->focus || a->scroll != b->scroll ||
	    a->pressed != b->pressed || a->edge != b->edge ||
	    a->share != b->share || a->thumbs != b->thumbs ||
	    a->built != b->built || a->bubble != b->bubble ||
	    a->paused != b->paused)
		return false;
	for (int i = 0; i < GLYPH_COUNT; i++)
		if (a->glyph[i] != b->glyph[i])
			return false;
	return true;
}

static str_id queue_state(void)
{
	if (downloads_sd_full())
		return STR_DLV_STATE_SD_FULL;
	if (downloads_paused())
		return STR_DLV_STATE_PAUSED;
	if (!downloads_online())
		return STR_DLV_STATE_OFFLINE;
	return downloads_waiting() > 0 ? STR_DLV_STATE_RUNNING
	                               : STR_DLV_STATE_IDLE;
}

static void tick(void)
{
	press_tick(&press_row);
	if (!press_down(&press_row))
		press_index = -1;
	for (int i = 0; i < GLYPH_COUNT; i++)
		press_tick(&press_glyph[i]);
	if (edge_frames > 0)
		edge_frames--;
	int shown = armed_key ? CONFIRM_FRAMES : BUBBLE_SHOW;
	if (bubble_frames >= 0 && ++bubble_frames >= shown) {
		bubble_frames = -1;
		armed_key = 0;
	}
	if (++share_frames >= SHARE_FRAMES) {
		share_frames = 0;
		share = downloads_active_share();
	}
	if (++free_frames >= FREE_FRAMES) {
		free_frames = 0;
		free_bytes = dl_store_free_bytes();
	}
	card.free_bytes = free_bytes;
	card.state = queue_state();
}

void view_downloads_enter(void)
{
	press_reset(&press_row);
	press_index = -1;
	for (int i = 0; i < GLYPH_COUNT; i++)
		press_reset(&press_glyph[i]);
	touch.target = -1;
	drag.down = drag.dragging = false;
	edge_frames = 0;
	pending = DLV_ACTION_NONE;
	pending_press = NULL;
	bubble_frames = -1;
	armed_key = 0;
	share = downloads_active_share();
	free_bytes = dl_store_free_bytes();
	free_frames = share_frames = 0;
	built = ~0u;
	rebuild();
	if (focus < 0 || !dlv_row_whole(&list, focus, scroll)) {
		focus = dlv_first_row(&list);
		scroll = 0;
		follow_window();
	}
	drawn_valid = false;
	card_drawn = false;
}

/* --- Input ------------------------------------------------------------- */

static void queue(dlv_action action, const press_state *press)
{
	pending = action;
	pending_press = press;
}

static void focus_row(int i)
{
	focus = i;
	scroll = dlv_scroll_to(&list, focus, scroll);
	follow_window();
}

static void move_focus(int delta)
{
	bool bumped = false;
	int next = focus;
	int steps = delta < 0 ? -delta : delta;
	for (int s = 0; s < steps; s++) {
		bool b;
		int n = dlv_move(&list, next, delta < 0 ? -1 : 1, &b);
		if (b) {
			bumped = s == 0;
			break;
		}
		next = n;
	}
	focus_row(next);
	if (bumped) {
		edge_dir = delta < 0 ? -1 : 1;
		edge_frames = ANIM_EDGE;
	}
}

/* A on the focused row (or a tap on it): cancel, retry or open. Returns
 * the action for the caller, if any. */
static dlv_action activate(void)
{
	if (focus < 0 || !idx)
		return DLV_ACTION_NONE;
	const dlv_entry *e = &list.entries[focus];
	char uri[SP_URI_MAX];
	snprintf(uri, sizeof(uri), "%s", entry_uri(focus));
	if (!uri[0])
		return DLV_ACTION_NONE;
	switch (e->section) {
	case DLV_SEC_ACTIVE:
	case DLV_SEC_QUEUED:
		if (!armed(KEY_A, uri)) {
			ask(STR_DLV_CANCEL_CONFIRM, KEY_A, uri);
			return DLV_ACTION_NONE;
		}
		downloads_cancel(uri);
		notice(STR_DLV_CANCELLED, true);
		return DLV_ACTION_NONE;
	case DLV_SEC_FAILED:
		downloads_retry(uri);
		notice(STR_DL_ADDED, true);
		return DLV_ACTION_NONE;
	default:
		snprintf(target_uri, sizeof(target_uri), "%s", uri);
		if (e->kind == DLV_COLL) {
			target_track = -1;
			target_kind = idx->colls[e->index].kind;
			return DLV_ACTION_OPEN_COLLECTION;
		}
		target_track = e->index;
		return DLV_ACTION_OPEN_TRACK;
	}
}

/* X: off the card, or out of the queue, after a second X. */
static void remove_focused(void)
{
	char uri[SP_URI_MAX];
	snprintf(uri, sizeof(uri), "%s", entry_uri(focus));
	if (!uri[0])
		return;
	if (!armed(KEY_X, uri)) {
		ask(STR_DLV_REMOVE_CONFIRM, KEY_X, uri);
		return;
	}
	bool done = list.entries[focus].section == DLV_SEC_DONE ||
	            list.entries[focus].section == DLV_SEC_FAILED;
	downloads_remove(uri);
	notice(done ? STR_DL_REMOVED : STR_DLV_CANCELLED, true);
}

static void release_keys(u32 up)
{
	if (up & KEY_A) {
		press_end(&press_glyph[GLYPH_A]);
		if (touch.target < 0)
			press_end(&press_row);
	}
	if (up & KEY_X)
		press_end(&press_glyph[GLYPH_X]);
	if (up & KEY_Y)
		press_end(&press_glyph[GLYPH_Y]);
	if (up & KEY_B)
		press_end(&press_glyph[GLYPH_B]);
}

/* The stylus: tapping a row focuses it, tapping the focused one acts,
 * dragging scrolls (6.4, 6.9). True while it owns the input. */
static bool handle_touch(u32 down, u32 held, u32 up)
{
	touchPosition pos;
	if (up & KEY_TOUCH) {
		bool was = drag.dragging;
		drag.down = drag.dragging = false;
		if (was)
			return true;
	} else if (held & KEY_TOUCH) {
		hidTouchRead(&pos);
		if (down & KEY_TOUCH) {
			if (pos.py >= dlv_view_top() && pos.py < SCREEN_H - BAR_H &&
			    !tabbar_touching())
				list_drag_begin(&drag, pos.py, scroll);
		} else if (list_drag_update(&drag, pos.py)) {
			if (touch.target >= 0) {
				press_cancel(&press_row);
				touch.target = -1;
			}
			scroll = dlv_clamp_scroll(&list, drag.start_scroll -
			                                 (pos.py - drag.start_y));
			follow_window();
			return true;
		}
	}
	int hit = -1;
	if (held & KEY_TOUCH) {
		hidTouchRead(&pos);
		hit = dlv_hit_row(&list, pos.px, pos.py, scroll);
	}
	int control;
	touch_event event = touch_track_update(&touch, (down & KEY_TOUCH) != 0,
	                                       (held & KEY_TOUCH) != 0,
	                                       (up & KEY_TOUCH) != 0, hit,
	                                       &control);
	switch (event) {
	case TOUCH_PRESS:
		press_begin(&press_row);
		press_index = control;
		break;
	case TOUCH_CANCEL:
		press_cancel(&press_row);
		break;
	case TOUCH_ACTIVATE:
		press_end(&press_row);
		if (control == focus) {
			dlv_action action = activate();
			if (action != DLV_ACTION_NONE)
				queue(action, &press_row);
		} else {
			focus = control;   /* the list stays where it is */
		}
		break;
	case TOUCH_NONE:
		break;
	}
	return touch.target >= 0;
}

dlv_action view_downloads_update(u32 down)
{
	u32 held = hidKeysHeld();
	u32 up = hidKeysUp();
	if (built != downloads_generation())
		rebuild();
	tick();
	release_keys(up);
	ensure_parsed();
	thumbs_client_frame(&thumbs);

	if (pending != DLV_ACTION_NONE) {
		if (!press_shown(pending_press))
			return DLV_ACTION_NONE;
		dlv_action action = pending;
		pending = DLV_ACTION_NONE;
		pending_press = NULL;
		return action;
	}

	if (handle_touch(down, held, up) ||
	    pending != DLV_ACTION_NONE)
		return DLV_ACTION_NONE;

	/* 6.6: with the focus dragged out of view, the first press only brings
	 * the list back to it. */
	u32 keys = KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_X;
	if ((down & keys) && focus >= 0 && !dlv_row_whole(&list, focus, scroll)) {
		focus_row(focus);
		if (down & KEY_A)
			press_begin(&press_glyph[GLYPH_A]);
		if (down & KEY_X)
			press_begin(&press_glyph[GLYPH_X]);
		return DLV_ACTION_NONE;
	}
	if (focus >= 0) {
		if (down & KEY_UP)
			move_focus(-1);
		else if (down & KEY_DOWN)
			move_focus(1);
		else if (down & KEY_LEFT)
			move_focus(-RESULTS_PER_PAGE);
		else if (down & KEY_RIGHT)
			move_focus(RESULTS_PER_PAGE);
	}

	if ((down & KEY_A) && focus >= 0) {
		press_begin(&press_glyph[GLYPH_A]);
		press_begin(&press_row);
		press_index = focus;
		dlv_action action = activate();
		if (action != DLV_ACTION_NONE)
			queue(action, &press_row);
	} else if ((down & KEY_X) && focus >= 0) {
		press_begin(&press_glyph[GLYPH_X]);
		remove_focused();
	} else if (down & KEY_Y) {
		press_begin(&press_glyph[GLYPH_Y]);
		bool pause = !downloads_paused();
		downloads_set_paused(pause);
		notice(pause ? STR_DLV_PAUSED_NOTICE : STR_DLV_RESUMED_NOTICE, true);
	} else if (down & KEY_B) {
		press_begin(&press_glyph[GLYPH_B]);
		queue(DLV_ACTION_BACK, &press_glyph[GLYPH_B]);
	} else if (down & KEY_SELECT) {
		return DLV_ACTION_LANGUAGE;
	}
	return DLV_ACTION_NONE;
}

bool view_downloads_target_track(sp_track *out)
{
	if (!idx || target_track < 0 || target_track >= idx->track_count ||
	    strcmp(idx->tracks[target_track].uri, target_uri) != 0)
		return false;
	downloads_track_to_sp(&idx->tracks[target_track], out);
	return true;
}

bool view_downloads_target_collection(char *uri, size_t len,
                                      dl_coll_kind *kind)
{
	if (!target_uri[0] || target_track >= 0)
		return false;
	snprintf(uri, len, "%s", target_uri);
	*kind = target_kind;
	return true;
}

bool view_downloads_dirty(void)
{
	if (!drawn_valid || parsed_lang != lang_get() || rows_stale ||
	    built != downloads_generation())
		return true;
	dlv_look look = current_look();
	return !same_look(&look, &drawn);
}

/* --- Drawing ----------------------------------------------------------- */

static icon_id state_icon(const dlv_entry *e)
{
	switch (e->section) {
	case DLV_SEC_QUEUED:
		return ICON_QUEUED;
	case DLV_SEC_FAILED:
		return ICON_ERROR;
	default:
		return ICON_DOWNLOADED;
	}
}

static void draw_entry(int i, const dlv_look *look)
{
	const dlv_entry *e = &list.entries[i];
	ui_rect r = dlv_entry_rect(&list, i, look->scroll);
	int w = i - base;
	if (e->kind == DLV_HEADER) {
		ui_text(&section_text[e->index], r.x, r.y + r.h / 2, TXT_META,
		        COL_TEXTO_2);
		return;
	}
	bool focused = i == look->focus;
	bool pressed = i == look->pressed;
	if (focused)
		r = ui_rect_offset(r, 0, look->edge);
	u32 ink, meta;
	r = ui_item_frame(r, focused, pressed, &ink, &meta);

	C2D_Image image;
	bool shown = w >= 0 && w < WINDOW &&
	             thumbs_client_image(&thumbs, w, &image);
	/* The note where a cover is missing, as everywhere; Liked Songs'
	 * heart. */
	int placeholder = strcmp(entry_uri(i), SP_LIKED_URI) == 0 ? ICON_HEART
	                                                          : ICON_NOTE;
	ui_art_or(list_thumb_rect(r), shown ? &image : NULL, placeholder);
	if (w < 0 || w >= WINDOW)
		return;

	int x = list_text_x(r);
	int right = r.x + r.w - SP_2;
	int title_cy = r.y + SP_1 + ui_line_height(TXT_CABEZA) / 2;
	int sub_cy = r.y + r.h - SP_1 - ui_line_height(TXT_CUERPO) / 2;
	ui_text(&title_text[w], x, title_cy, TXT_CABEZA, ink);
	ui_text(&sub_text[w], x, sub_cy, TXT_CUERPO, meta);
	if (e->section == DLV_SEC_ACTIVE) {
		/* The share, and a thin bar of it beside the artist: the volume
		 * bar's look, outlined, so it reads on a focused row too. */
		int tw = ui_px_ceil(ui_text_width(&share_text, TXT_META));
		ui_text(&share_text, right - tw, title_cy, TXT_META, meta);
		int bar_x = x + text_width() / 2 + SP_2;
		ui_rect bar = { bar_x, sub_cy - VOLUME_H / 2, right - bar_x,
		                VOLUME_H };
		ui_progress(bar, -1, look->share * 10, UI_HANDLE_NONE);
		return;
	}
	C2D_Image icon = icon_image(state_icon(e));
	ui_icon(&icon, right - ICON_S, title_cy - ICON_S / 2, meta);
}

static ui_hint hint(int glyph, const C2D_Text *label, const dlv_look *look)
{
	return (ui_hint){ &letter_text[glyph], label, glyph_colors[glyph],
	                  look->glyph[glyph], NULL };
}

static void draw_bar(const dlv_look *look)
{
	ui_hint hints[GLYPH_COUNT];
	int n = 0;
	if (look->focus >= 0) {
		const C2D_Text *a;
		switch (list.entries[look->focus].section) {
		case DLV_SEC_ACTIVE:
		case DLV_SEC_QUEUED:
			a = &hint_cancel;
			break;
		case DLV_SEC_FAILED:
			a = &hint_retry;
			break;
		default:
			a = &hint_open;
			break;
		}
		hints[n++] = hint(GLYPH_A, a, look);
		hints[n++] = hint(GLYPH_X, &hint_remove, look);
	}
	hints[n++] = hint(GLYPH_Y, look->paused ? &hint_resume : &hint_pause,
	                  look);
	hints[n++] = hint(GLYPH_B, &hint_back, look);
	ui_button_bar(list_hints_rect(), hints, n);
}

void view_downloads_draw_bottom(void)
{
	if (built != downloads_generation())
		rebuild();
	ensure_parsed();
	dlv_look look = current_look();

	if (list.count == 0) {
		ui_text_centered(&empty_text, SCREEN_BOTTOM_W / 2,
		                 dlv_view_top() + dlv_view_h() / 2, TXT_CUERPO,
		                 COL_TEXTO);
	} else {
		int from, to;
		dlv_visible(&list, look.scroll, &from, &to);
		for (int i = from; i <= to; i++)
			if (i != look.focus)
				draw_entry(i, &look);
		/* The focused row last: its nudge goes over its neighbours. */
		if (look.focus >= from && look.focus <= to)
			draw_entry(look.focus, &look);
		if (dlv_max_scroll(&list) > 0) {
			ui_fill(dlv_scroll_track(), COL_CAPA);
			ui_fill(dlv_scroll_thumb(&list, look.scroll), COL_CLOROFILA);
		}
	}
	/* The bar after the rows: dragged rows slide under it, and under the
	 * tab bar, drawn by the caller. */
	draw_bar(&look);
	if (look.bubble != STR_COUNT) {
		C2D_Image icon = icon_image(notice_ok ? ICON_NOTE : ICON_ERROR);
		ui_bubble(list_bubble_rect(ui_bubble_content_width(&notice_text)),
		          &icon, &notice_text);
	}
	drawn = look;
	drawn_valid = true;
}

/* --- Top screen -------------------------------------------------------- */

static bool same_card(const top_card *a, const top_card *b)
{
	return a->s.tracks == b->s.tracks && a->s.albums == b->s.albums &&
	       a->s.playlists == b->s.playlists && a->s.used == b->s.used &&
	       a->s.waiting == b->s.waiting &&
	       a->free_bytes / (1024 * 1024) == b->free_bytes / (1024 * 1024) &&
	       a->state == b->state;
}

static lang_id card_lang = LANG_COUNT;

static void parse_card(void)
{
	C2D_TextBufClear(top_buf);
	char decimal = lang_get() == LANG_ES ? ',' : '.';
	char size[32], text[128];
	parse(&top_title, top_buf, tr(STR_SETTINGS_DOWNLOADS));
	snprintf(text, sizeof(text), tr(STR_DLV_SUMMARY_FMT), card.s.tracks,
	         card.s.albums, card.s.playlists);
	parse(&top_summary, top_buf, text);
	dlv_format_size(card.s.used, decimal, size, sizeof(size));
	snprintf(text, sizeof(text), tr(STR_DLV_USED_FMT), size);
	parse(&top_used, top_buf, text);
	dlv_format_size(card.free_bytes, decimal, size, sizeof(size));
	snprintf(text, sizeof(text), tr(STR_DLV_FREE_FMT), size);
	parse(&top_free, top_buf, text);
	parse(&top_state, top_buf, tr(card.state));
	parsed_card = card;
	card_parsed = true;
	card_lang = lang_get();
}

bool view_downloads_top_dirty(void)
{
	return !card_drawn || card_lang != lang_get() ||
	       !same_card(&card, &drawn_card);
}

/* A `capa` panel with the title and four lines (STYLES.md 7.13), shorter
 * over the mini player (5.18). */
void view_downloads_draw_top(bool mini)
{
	if (!card_parsed || card_lang != lang_get() ||
	    !same_card(&card, &parsed_card))
		parse_card();
	ui_rect panel = dlv_top_panel_rect(mini);
	ui_fill(panel, COL_TINTA);
	ui_fill(ui_rect_inset(panel, OUTLINE_W), COL_CAPA);
	int x = panel.x + SP_4;
	int y = panel.y + SP_4 + ui_line_height(TXT_TITULO) / 2;
	ui_text(&top_title, x, y, TXT_TITULO, COL_TEXTO);
	y += ui_line_height(TXT_TITULO) / 2 + SP_4;
	const C2D_Text *lines[] = { &top_summary, &top_used, &top_free };
	for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
		y += ui_line_height(TXT_CUERPO) / 2;
		ui_text(lines[i], x, y, TXT_CUERPO, COL_TEXTO);
		y += ui_line_height(TXT_CUERPO) / 2 + SP_2;
	}
	y += SP_2 + ui_line_height(TXT_CABEZA) / 2;
	ui_text(&top_state, x, y, TXT_CABEZA, COL_TEXTO_2);
	drawn_card = card;
	card_drawn = true;
}
