/* The downloads manager in the Settings tab: what it lists, where each
 * entry goes and where the focus can go. Pure C with no 3DS headers,
 * tested in tests/native_logic_test.c.
 *
 * One list, in sections, each with a header that takes no focus:
 *   In progress   the track downloading now, with its share
 *   Queued        albums and playlists still being resolved, then the
 *                 ones with tracks waiting ("8 of 12"), then the tracks
 *                 waiting on their own, in queue order
 *   Failed        every track that failed, in queue order
 *   Downloaded    albums and playlists with nothing left waiting (whole,
 *                 or short of the tracks that failed), and the tracks on
 *                 the card that none of those holds, newest first
 * A section with nothing in it is left out, header and all.
 *
 * Bottom screen, between the tab bar (32) and the button bar (216): the
 * entries one under the other, a header DLV_HEADER_H high, a row ROW_H
 * with SP_1 under it as in the results (5.3). The list scrolls by pixels:
 * the buttons keep the focused row in view, with its section's header when
 * the row is the first of it; the stylus drags it (6.9).
 *
 * Top screen: the manager's card, a panel e4 from the edges (16, 16, 368,
 * 208), whose bottom edge ends e2 above the mini player while it shows
 * (5.18): 16..176, the mini player on the same right edge. Its texts hang
 * from the top and do not move. */
#ifndef DOWNLOADS_VIEW_LOGIC_H
#define DOWNLOADS_VIEW_LOGIC_H

#include "downloads_logic.h"
#include "layout.h"
#include "list_logic.h"
#include "now_layout.h"
#include "theme.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Albums and playlists waiting to be resolved: downloads.c's queue. */
#define DLV_PENDING_MAX 4
#define DLV_ENTRIES_MAX \
	(DL_TRACKS_MAX + DL_COLLS_MAX + DLV_PENDING_MAX + DLV_SEC_COUNT)
/* A section header: one t_meta line with room around it. */
#define DLV_HEADER_H SP_6

typedef enum {
	DLV_SEC_ACTIVE,
	DLV_SEC_QUEUED,
	DLV_SEC_FAILED,
	DLV_SEC_DONE,
	DLV_SEC_COUNT
} dlv_section;

typedef enum {
	DLV_HEADER,      /* `index` is the section */
	DLV_TRACK,       /* index into idx->tracks */
	DLV_COLL,        /* index into idx->colls */
	DLV_PENDING      /* index into downloads.c's queue of collections */
} dlv_kind;

typedef struct {
	uint8_t kind;    /* dlv_kind */
	uint8_t section; /* dlv_section */
	int16_t index;
	int16_t y;       /* top, from the top of the list, px */
} dlv_entry;

typedef struct {
	int count;
	int rows[DLV_SEC_COUNT];       /* entries in each section */
	int height;                    /* of the whole list, px */
	dlv_entry entries[DLV_ENTRIES_MAX];
} dlv_list;

/* ------------------------------------------------------------- building */

typedef struct {
	uint32_t added_at;
	uint8_t kind;
	int16_t index;
} dlv_done_item;

/* Newest first; the same moment keeps the index's order. */
static inline int dlv_done_cmp(const void *pa, const void *pb)
{
	const dlv_done_item *a = (const dlv_done_item *)pa;
	const dlv_done_item *b = (const dlv_done_item *)pb;
	if (a->added_at != b->added_at)
		return a->added_at < b->added_at ? 1 : -1;
	if (a->kind != b->kind)
		return a->kind < b->kind ? 1 : -1;   /* collections first */
	return a->index - b->index;
}

static inline int dlv_entry_height(const dlv_entry *e)
{
	return e->kind == DLV_HEADER ? DLV_HEADER_H : LIST_PITCH;
}

static inline void dlv_push(dlv_list *l, dlv_kind kind, dlv_section section,
                            int index)
{
	if (l->count >= DLV_ENTRIES_MAX)
		return;
	dlv_entry *e = &l->entries[l->count++];
	e->kind = (uint8_t)kind;
	e->section = (uint8_t)section;
	e->index = (int16_t)index;
	e->y = (int16_t)l->height;
	l->height += dlv_entry_height(e);
	if (kind != DLV_HEADER)
		l->rows[section]++;
}

/* Tracks in `state` that are rows of their own, lowest seq first. `own`
 * says which are. */
static inline void dlv_push_tracks(dlv_list *l, const dl_index *idx,
                                   dl_state state, const bool *own,
                                   dlv_section section)
{
	/* Selection by seq: queues hold a few hundred at most. */
	uint32_t last = 0;
	bool first = true;
	for (;;) {
		int best = -1;
		for (int t = 0; t < idx->track_count; t++) {
			const dl_track *d = &idx->tracks[t];
			if (d->state != state || (own && !own[t]))
				continue;
			if (!first && d->seq <= last)
				continue;
			if (best < 0 || d->seq < idx->tracks[best].seq)
				best = t;
		}
		if (best < 0)
			return;
		dlv_push(l, DLV_TRACK, section, best);
		last = idx->tracks[best].seq;
		first = false;
	}
}

/* Rebuilds `l` from the index and the `pending` collections waiting to be
 * resolved. `scratch` holds DL_TRACKS_MAX + DL_COLLS_MAX items for the
 * history's order. */
static inline void dlv_build(dlv_list *l, const dl_index *idx, int pending,
                             dlv_done_item *scratch)
{
	l->count = 0;
	l->height = 0;
	for (int s = 0; s < DLV_SEC_COUNT; s++)
		l->rows[s] = 0;
	if (!idx)
		return;

	/* Each collection: with tracks still waiting (a Queued row), or not
	 * (a Downloaded row). */
	bool waiting[DL_COLLS_MAX];
	for (int c = 0; c < idx->coll_count; c++)
		waiting[c] = false;
	for (int i = 0; i < idx->ref_count; i++) {
		int c = idx->refs[i].coll, t = idx->refs[i].track;
		if (c >= idx->coll_count || t >= idx->track_count)
			continue;
		dl_state s = idx->tracks[t].state;
		if (s == DL_QUEUED || s == DL_ACTIVE)
			waiting[c] = true;
	}
	/* Which tracks a row of a collection already stands for. */
	static bool in_done[DL_TRACKS_MAX], in_waiting[DL_TRACKS_MAX];
	static bool own[DL_TRACKS_MAX];
	for (int t = 0; t < idx->track_count; t++)
		in_done[t] = in_waiting[t] = false;
	for (int i = 0; i < idx->ref_count; i++) {
		int c = idx->refs[i].coll, t = idx->refs[i].track;
		if (c >= idx->coll_count || t >= idx->track_count)
			continue;
		if (waiting[c])
			in_waiting[t] = true;
		else
			in_done[t] = true;
	}

	/* In progress. */
	int active = -1;
	for (int t = 0; t < idx->track_count; t++)
		if (idx->tracks[t].state == DL_ACTIVE)
			active = t;
	if (active >= 0) {
		dlv_push(l, DLV_HEADER, DLV_SEC_ACTIVE, DLV_SEC_ACTIVE);
		dlv_push(l, DLV_TRACK, DLV_SEC_ACTIVE, active);
	}

	/* Queued. */
	for (int t = 0; t < idx->track_count; t++)
		own[t] = !in_waiting[t];
	int queued_colls = 0;
	for (int c = 0; c < idx->coll_count; c++)
		queued_colls += waiting[c] ? 1 : 0;
	int queued_tracks = 0;
	for (int t = 0; t < idx->track_count; t++)
		queued_tracks += idx->tracks[t].state == DL_QUEUED && own[t];
	if (pending > DLV_PENDING_MAX)
		pending = DLV_PENDING_MAX;
	if (pending > 0 || queued_colls > 0 || queued_tracks > 0) {
		dlv_push(l, DLV_HEADER, DLV_SEC_QUEUED, DLV_SEC_QUEUED);
		for (int p = 0; p < pending; p++)
			dlv_push(l, DLV_PENDING, DLV_SEC_QUEUED, p);
		for (int c = 0; c < idx->coll_count; c++)
			if (waiting[c])
				dlv_push(l, DLV_COLL, DLV_SEC_QUEUED, c);
		dlv_push_tracks(l, idx, DL_QUEUED, own, DLV_SEC_QUEUED);
	}

	/* Failed: every one, so each can be tried again. */
	int failed = 0;
	for (int t = 0; t < idx->track_count; t++)
		failed += idx->tracks[t].state == DL_FAILED;
	if (failed > 0) {
		dlv_push(l, DLV_HEADER, DLV_SEC_FAILED, DLV_SEC_FAILED);
		dlv_push_tracks(l, idx, DL_FAILED, NULL, DLV_SEC_FAILED);
	}

	/* Downloaded, newest first. */
	int n = 0;
	for (int c = 0; c < idx->coll_count; c++)
		if (!waiting[c])
			scratch[n++] = (dlv_done_item){ idx->colls[c].added_at,
			                                DLV_COLL, (int16_t)c };
	for (int t = 0; t < idx->track_count; t++)
		if (idx->tracks[t].state == DL_DONE && !in_done[t])
			scratch[n++] = (dlv_done_item){ idx->tracks[t].added_at,
			                                DLV_TRACK, (int16_t)t };
	if (n > 0) {
		qsort(scratch, (size_t)n, sizeof(scratch[0]), dlv_done_cmp);
		dlv_push(l, DLV_HEADER, DLV_SEC_DONE, DLV_SEC_DONE);
		for (int i = 0; i < n; i++)
			dlv_push(l, (dlv_kind)scratch[i].kind, DLV_SEC_DONE,
			         scratch[i].index);
	}
}

/* ----------------------------------------------------------------- focus */

/* The row nearest to `from` going `delta` (-1 or +1), headers skipped;
 * `from` itself when there is none that way. `*bumped` then. */
static inline int dlv_move(const dlv_list *l, int from, int delta,
                           bool *bumped)
{
	*bumped = false;
	for (int i = from + delta; i >= 0 && i < l->count; i += delta)
		if (l->entries[i].kind != DLV_HEADER)
			return i;
	*bumped = true;
	return from;
}

/* The first row, or -1 for an empty list. */
static inline int dlv_first_row(const dlv_list *l)
{
	bool bumped;
	int i = dlv_move(l, -1, 1, &bumped);
	return bumped ? -1 : i;
}

/* `focus` made a row: itself, else the next one down, else up. */
static inline int dlv_clamp_focus(const dlv_list *l, int focus)
{
	if (l->count <= 0)
		return -1;
	if (focus < 0)
		focus = 0;
	if (focus >= l->count)
		focus = l->count - 1;
	if (l->entries[focus].kind != DLV_HEADER)
		return focus;
	bool bumped;
	int down = dlv_move(l, focus, 1, &bumped);
	if (!bumped)
		return down;
	int up = dlv_move(l, focus, -1, &bumped);
	return bumped ? -1 : up;
}

/* ---------------------------------------------------------------- layout */

static inline int dlv_view_top(void)
{
	return TAB_H;
}

/* The height between the bars. */
static inline int dlv_view_h(void)
{
	return SCREEN_H - BAR_H - TAB_H;
}

/* The list starts SP_1 under the tab bar and ends SP_1 over the button
 * bar, as the results do. */
static inline int dlv_max_scroll(const dlv_list *l)
{
	int over = SP_1 + l->height - dlv_view_h();
	return over > 0 ? over : 0;
}

static inline int dlv_clamp_scroll(const dlv_list *l, int scroll)
{
	int max = dlv_max_scroll(l);
	return scroll < 0 ? 0 : scroll > max ? max : scroll;
}

/* Entry `i` on screen with the list at `scroll`: rows are ROW_H high
 * (their SP_1 gap below is not theirs), headers DLV_HEADER_H. */
static inline ui_rect dlv_entry_rect(const dlv_list *l, int i, int scroll)
{
	const dlv_entry *e = &l->entries[i];
	int h = e->kind == DLV_HEADER ? DLV_HEADER_H : ROW_H;
	ui_rect r = { SP_2, dlv_view_top() + SP_1 + e->y - scroll,
	              SCREEN_BOTTOM_W - 2 * SP_2, h };
	return r;
}

/* The least scroll that shows row `focus` whole; a section's first row
 * brings its header along. */
static inline int dlv_scroll_to(const dlv_list *l, int focus, int scroll)
{
	if (focus < 0 || focus >= l->count)
		return dlv_clamp_scroll(l, scroll);
	const dlv_entry *e = &l->entries[focus];
	int top = e->y;
	if (focus > 0 && l->entries[focus - 1].kind == DLV_HEADER)
		top = l->entries[focus - 1].y;
	int bottom = e->y + ROW_H;
	int view = dlv_view_h() - 2 * SP_1;
	if (top < scroll)
		scroll = top;
	if (bottom > scroll + view)
		scroll = bottom - view;
	return dlv_clamp_scroll(l, scroll);
}

/* Whether row `i` is wholly between the bars at `scroll`. */
static inline bool dlv_row_whole(const dlv_list *l, int i, int scroll)
{
	if (i < 0 || i >= l->count)
		return false;
	ui_rect r = dlv_entry_rect(l, i, scroll);
	return r.y >= dlv_view_top() && r.y + r.h <= SCREEN_H - BAR_H;
}

/* The row under (x, y), or -1: between the bars, rows only. */
static inline int dlv_hit_row(const dlv_list *l, int x, int y, int scroll)
{
	if (y < dlv_view_top() || y >= SCREEN_H - BAR_H)
		return -1;
	for (int i = 0; i < l->count; i++) {
		if (l->entries[i].kind == DLV_HEADER)
			continue;
		if (ui_rect_contains(dlv_entry_rect(l, i, scroll), x, y))
			return i;
	}
	return -1;
}

/* The entries at least partly between the bars at `scroll`. */
static inline void dlv_visible(const dlv_list *l, int scroll, int *from,
                               int *to)
{
	*from = 0;
	*to = -1;
	bool any = false;
	for (int i = 0; i < l->count; i++) {
		ui_rect r = dlv_entry_rect(l, i, scroll);
		if (r.y + r.h <= dlv_view_top() || r.y >= SCREEN_H - BAR_H)
			continue;
		if (!any)
			*from = i;
		*to = i;
		any = true;
	}
}

/* The scroll indicator (5.11), as list_scroll_thumb. */
static inline ui_rect dlv_scroll_track(void)
{
	ui_rect r = { SCREEN_BOTTOM_W - SCROLL_W, dlv_view_top(), SCROLL_W,
	              dlv_view_h() };
	return r;
}

static inline ui_rect dlv_scroll_thumb(const dlv_list *l, int scroll)
{
	ui_rect track = dlv_scroll_track();
	int max = dlv_max_scroll(l);
	if (max <= 0)
		return track;
	int h = track.h * track.h / (l->height + 2 * SP_1);
	if (h < SCROLL_MIN_H)
		h = SCROLL_MIN_H;
	scroll = dlv_clamp_scroll(l, scroll);
	int travel = track.h - h;
	ui_rect r = { track.x, track.y + (travel * scroll + max / 2) / max,
	              track.w, h };
	return r;
}

/* --------------------------------------------------------------- summary */

/* The top screen's card: what is on the card and how the queue is. */
typedef struct {
	int tracks;              /* on the card */
	int albums, playlists;   /* in the index */
	uint64_t used;           /* bytes of the tracks on the card */
	int waiting;             /* tracks queued or downloading */
} dlv_summary;

static inline dlv_summary dlv_summarize(const dl_index *idx)
{
	dlv_summary s = { 0, 0, 0, 0, 0 };
	if (!idx)
		return s;
	for (int t = 0; t < idx->track_count; t++) {
		const dl_track *d = &idx->tracks[t];
		if (d->state == DL_DONE) {
			s.tracks++;
			s.used += d->size;
		} else if (d->state == DL_QUEUED || d->state == DL_ACTIVE) {
			s.waiting++;
		}
	}
	for (int c = 0; c < idx->coll_count; c++) {
		if (idx->colls[c].kind == DL_COLL_PLAYLIST)
			s.playlists++;
		else
			s.albums++;
	}
	return s;
}

/* "312 MB", "5,1 GB" (with `decimal` as the separator), "0 MB". */
static inline void dlv_format_size(uint64_t bytes, char decimal, char *out,
                                   size_t len)
{
	const uint64_t mb = 1024u * 1024u, gb = mb * 1024u;
	if (bytes >= gb) {
		uint64_t tenths = (bytes * 10 + gb / 2) / gb;
		snprintf(out, len, "%u%c%u GB", (unsigned)(tenths / 10), decimal,
		         (unsigned)(tenths % 10));
	} else {
		snprintf(out, len, "%u MB", (unsigned)((bytes + mb / 2) / mb));
	}
}

/* The manager's card on the top screen (STYLES.md 7.13): e4 from the
 * edges, or ending e2 above the mini player while it shows (5.18). */
static inline ui_rect dlv_top_panel_rect(bool mini)
{
	int bottom = mini ? now_mini_clear_bottom(now_mini_panel_rect())
	                  : SCREEN_H - SP_4;
	ui_rect r = { SP_4, SP_4, SCREEN_TOP_W - 2 * SP_4, bottom - SP_4 };
	return r;
}

#endif
