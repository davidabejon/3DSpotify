#include "details.h"

#include "detail_logic.h"
#include "downloads.h"
#include "http3ds.h"
#include "list_logic.h"
#include "lang.h"
#include "worker_priority.h"

#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An http_session, a TLS handshake and a metadata batch: what the
 * thumbnails' worker needs too. */
#define DETAIL_WORKER_STACK (32 * 1024)
/* The most tracks one load names: a playlist page (SP_PLAYLIST_PAGE) or a
 * whole album or artist (DETAIL_MAX_TRACKS). */
#define DETAIL_JOB_MAX (SP_PLAYLIST_PAGE > DETAIL_MAX_TRACKS \
                        ? SP_PLAYLIST_PAGE : DETAIL_MAX_TRACKS)
/* After a playlist page failed, the next try waits this long, so a list
 * resting at its end does not ask again every frame. */
#define DETAIL_MORE_RETRY_MS 5000

/* One load at a time. Everything the worker touches is here, so a load
 * that nobody wants any more can finish on its own without harm. Global,
 * never on a thread's stack: the tracks alone are ~23 KB. */
typedef struct {
	detail_kind kind;
	/* A further page of a playlist already shown, from item `from`; the
	 * first load is a page from 0 that also brings the header. */
	bool page;
	int  from;
	char uri[SP_URI_MAX];
	char host[128];
	char token[1024];
	char username[128];
	int  doc;                     /* the document it is for... */
	unsigned serial;              /* ...as long as it still has this serial */
	sp_album  album;
	sp_artist artist;
	sp_playlist_info playlist;
	const char *uris[DETAIL_JOB_MAX];
	sp_track tracks[DETAIL_JOB_MAX];
	int  count;
	int  result;                  /* 0 or -1 */
	char error[192];
	volatile bool cancel_requested;
	volatile bool done;
} detail_job;

static detail_doc docs[DETAIL_POOL];
/* Liked Songs: the whole list of URIs, newest first, as its first load
 * read it. Pages after it name the next ones from here, with no request
 * for the list again. Written and read by the worker only (one runs at a
 * time); a new first load replaces it. */
static char liked_uris[DETAIL_PLAYLIST_MAX][SP_URI_MAX];
static int liked_count, liked_total;
static detail_stack stacks[DETAIL_STACKS];
static detail_job job;
static Thread worker;

static void secure_clear(void *data, size_t len)
{
	volatile unsigned char *p = data;
	while (len--)
		*p++ = 0;
}

static detail_stack *stack_of(app_tab tab)
{
	int i = detail_tab_stack(tab);
	return i < 0 ? NULL : &stacks[i];
}

static bool on_top(int slot)
{
	for (int i = 0; i < DETAIL_STACKS; i++)
		if (detail_stack_top(&stacks[i]) == slot)
			return true;
	return false;
}

/* A level going away: its load, if running, is not wanted. */
static void forget_load(int slot)
{
	if (worker && job.doc == slot)
		job.cancel_requested = true;
}

/* --------------------------------------------------------------- worker */

static void worker_main(void *arg)
{
	(void)arg;
	int n = 0;
	int rc = -1;
	http_session *sess = http_session_open(job.host);
	if (!sess) {
		snprintf(job.error, sizeof(job.error),
		         tr(STR_ERR_DETAIL_CONNECT_FMT), job.host);
		goto done;
	}
	if (job.kind == DETAIL_ALBUM) {
		rc = sp_album_metadata(sess, job.token, job.uri, &job.album,
		                       job.error, sizeof(job.error));
		n = job.album.track_count;
		for (int i = 0; i < n; i++)
			job.uris[i] = job.album.track_uris[i];
	} else if (job.kind == DETAIL_PLAYLIST &&
	           strcmp(job.uri, SP_LIKED_URI) == 0) {
		/* Liked Songs: the list comes whole, once; a page only names the
		 * next SP_PLAYLIST_PAGE of it. */
		rc = 0;
		memset(&job.playlist, 0, sizeof(job.playlist));
		if (!job.page) {
			int got = sp_liked_s(sess, job.token, job.username, liked_uris,
			                     DETAIL_PLAYLIST_MAX, &liked_total,
			                     job.error, sizeof(job.error));
			liked_count = got < 0 ? 0 : got;
			rc = got < 0 ? -1 : 0;
		}
		int from = job.from < liked_count ? job.from : liked_count;
		n = liked_count - from;
		if (n > SP_PLAYLIST_PAGE)
			n = SP_PLAYLIST_PAGE;
		for (int i = 0; i < n; i++)
			job.uris[i] = liked_uris[from + i];
		job.playlist.items = n;
		job.playlist.total_tracks = liked_total;
	} else if (job.kind == DETAIL_PLAYLIST) {
		/* A page at a time: the service honours the range, so a list of
		 * thousands costs what a short one does. */
		rc = sp_playlist_s(sess, job.token, job.uri, job.from,
		                   SP_PLAYLIST_PAGE, &job.playlist, job.error,
		                   sizeof(job.error));
		n = job.playlist.track_count;
		for (int i = 0; i < n; i++)
			job.uris[i] = job.playlist.track_uris[i];
	} else {
		/* The service already sends the account country's top tracks, so
		 * no country is needed to choose them (plan, phase 0). */
		rc = sp_artist_metadata(sess, job.token, job.uri, NULL, &job.artist,
		                        job.error, sizeof(job.error));
		n = job.artist.track_count;
		for (int i = 0; i < n; i++)
			job.uris[i] = job.artist.track_uris[i];
	}
	/* Its tracks come as bare ids: one more request, on the same link,
	 * gives them names. */
	if (rc == 0 && n > 0 && !job.cancel_requested) {
		int got = sp_track_metadata_batch_s(sess, job.token, job.uris, n,
		                                    job.tracks, job.error,
		                                    sizeof(job.error));
		if (got < 0)
			rc = -1;
		else
			job.count = got;
	}
	http_session_close(sess);
done:
	job.result = rc;
	__sync_synchronize();
	job.done = true;
}

/* A playlist page onto the end of its document, as much as fits. */
static void append_page(detail_doc *d)
{
	int room = d->capacity - d->count;
	int n = job.count < room ? job.count : room;
	if (n > 0)
		memcpy(d->tracks + d->count, job.tracks,
		       (size_t)n * sizeof(d->tracks[0]));
	d->count += n;
	d->next_from += job.playlist.items;
	d->total_tracks = job.playlist.total_tracks;
	/* An empty page means the end, whatever the total said. */
	d->more = job.playlist.items > 0 && d->next_from < d->total_tracks &&
	          d->count < d->capacity;
}

/* The finished load into its document, unless the document moved on. */
static void apply(detail_doc *d)
{
	if (job.result != 0) {
		d->state = DETAIL_FAILED;
		snprintf(d->error, sizeof(d->error), "%s", job.error);
		return;
	}
	if (job.kind == DETAIL_ALBUM) {
		const sp_album *a = &job.album;
		snprintf(d->title, sizeof(d->title), "%s", a->name);
		snprintf(d->subtitle, sizeof(d->subtitle), "%s", a->artist);
		snprintf(d->art, sizeof(d->art), "%s", a->art_large);
		snprintf(d->art_small, sizeof(d->art_small), "%s", a->art_small);
		snprintf(d->label, sizeof(d->label), "%s", a->label);
		d->year = a->year;
		d->album_type = a->type;
		d->total_tracks = a->total_tracks;
	} else if (job.kind == DETAIL_PLAYLIST) {
		const sp_playlist_info *p = &job.playlist;
		snprintf(d->title, sizeof(d->title), "%s", p->name);
		snprintf(d->art, sizeof(d->art), "%s", p->art);
		d->count = 0;
		d->next_from = 0;
		append_page(d);
		d->selected = 0;
		d->first = 0;
		d->state = DETAIL_READY;
		return;
	} else {
		const sp_artist *a = &job.artist;
		snprintf(d->title, sizeof(d->title), "%s", a->name);
		snprintf(d->art, sizeof(d->art), "%s", a->art_large);
		snprintf(d->art_small, sizeof(d->art_small), "%s", a->art_small);
		d->total_tracks = a->track_count;
	}
	d->count = job.count;
	memcpy(d->tracks, job.tracks, (size_t)job.count * sizeof(d->tracks[0]));
	d->selected = 0;
	d->first = 0;
	d->state = DETAIL_READY;
}

/* Joins a load that has returned (it costs nothing then). True when it
 * changed a document on screen. */
static bool reap(void)
{
	if (!worker || !job.done)
		return false;
	__sync_synchronize();
	threadJoin(worker, U64_MAX);
	threadFree(worker);
	worker = NULL;
	secure_clear(job.token, sizeof(job.token));
	secure_clear(job.username, sizeof(job.username));

	detail_doc *d = &docs[job.doc];
	if (job.page) {
		if (job.cancel_requested || d->serial != job.serial ||
		    d->state != DETAIL_READY || !d->loading_more)
			return false;   /* still asked for if wanted: started again */
		d->loading_more = false;
		if (job.result == 0)
			append_page(d);
		else
			d->retry_at = osGetTime() + DETAIL_MORE_RETRY_MS;
		return on_top(job.doc);
	}
	if (job.cancel_requested || d->serial != job.serial ||
	    d->state != DETAIL_LOADING)
		return false;   /* still LOADING if wanted: started again */
	apply(d);
	return on_top(job.doc);
}

static void start(int slot, const char *host, const char *token,
                  const char *username)
{
	detail_doc *d = &docs[slot];
	memset(&job, 0, sizeof(job));
	job.kind = d->kind;
	job.page = d->state == DETAIL_READY;
	job.from = job.page ? d->next_from : 0;
	job.doc = slot;
	job.serial = d->serial;
	snprintf(job.uri, sizeof(job.uri), "%s", d->uri);
	snprintf(job.host, sizeof(job.host), "%s", host);
	snprintf(job.token, sizeof(job.token), "%s", token);
	snprintf(job.username, sizeof(job.username), "%s",
	         username ? username : "");
	/* Below the decoder, like every worker that can overlap playback. */
	worker = threadCreate(worker_main, NULL, DETAIL_WORKER_STACK,
	                      background_priority(), -2, false);
	if (!worker)
		secure_clear(job.username, sizeof(job.username));
	if (!worker && job.page) {
		secure_clear(job.token, sizeof(job.token));
		d->loading_more = false;
		d->retry_at = osGetTime() + DETAIL_MORE_RETRY_MS;
	} else if (!worker) {
		secure_clear(job.token, sizeof(job.token));
		d->state = DETAIL_FAILED;
		snprintf(d->error, sizeof(d->error), "%s",
		         tr(STR_ERR_DETAIL_THREAD));
	}
}

/* The next document waiting for its load: only levels on top, since a
 * buried one is not on screen and loads when it surfaces. */
static int waiting(app_tab active)
{
	int first = detail_tab_stack(active);
	for (int k = 0; k < DETAIL_STACKS; k++) {
		int i = first < 0 ? k : (first + k) % DETAIL_STACKS;
		int top = detail_stack_top(&stacks[i]);
		if (top >= 0 && docs[top].kind != DETAIL_TRACK &&
		    (docs[top].state == DETAIL_LOADING ||
		     (docs[top].state == DETAIL_READY && docs[top].loading_more)))
			return top;
	}
	return -1;
}

/* ----------------------------------------------------------- the stacks */

/* A fresh document on top of `tab`'s stack, or NULL for a tab without
 * details. */
static detail_doc *push(app_tab tab)
{
	detail_stack *s = stack_of(tab);
	if (!s)
		return NULL;
	if (s->count >= DETAIL_DEPTH)
		forget_load(detail_stack_drop_oldest(s));
	int slot = detail_pool_free(stacks);
	if (slot < 0)
		return NULL;   /* cannot happen: see detail_pool_free */
	detail_doc *d = &docs[slot];
	unsigned serial = d->serial + 1;
	/* No stack holds it, and nothing else points at its buffer: the view
	 * re-sets its list before it draws a level again. */
	free(d->owned);
	free(d->rows);
	memset(d, 0, sizeof(*d));
	d->serial = serial;
	d->tracks = d->inline_tracks;
	d->capacity = DETAIL_MAX_TRACKS;
	detail_stack_push(s, slot);
	return d;
}

bool details_active(app_tab tab)
{
	detail_stack *s = stack_of(tab);
	return s && s->count > 0;
}

detail_doc *details_top(app_tab tab)
{
	detail_stack *s = stack_of(tab);
	int top = s ? detail_stack_top(s) : -1;
	return top >= 0 ? &docs[top] : NULL;
}

void details_open_track(app_tab tab, const sp_track *list, int count,
                        int index, bool single)
{
	if (!list || index < 0 || index >= count)
		return;
	/* A longer list keeps a window around the track. */
	int from = 0;
	if (count > DETAIL_MAX_TRACKS) {
		from = index - DETAIL_MAX_TRACKS / 2;
		if (from < 0)
			from = 0;
		if (from > count - DETAIL_MAX_TRACKS)
			from = count - DETAIL_MAX_TRACKS;
		count = DETAIL_MAX_TRACKS;
	}
	detail_doc *d = push(tab);
	if (!d)
		return;
	d->kind = DETAIL_TRACK;
	d->state = DETAIL_READY;
	d->single = single;
	memcpy(d->tracks, list + from, (size_t)count * sizeof(d->tracks[0]));
	d->count = count;
	d->selected = index - from;
	const sp_track *t = &d->tracks[d->selected];
	snprintf(d->uri, sizeof(d->uri), "%s", t->uri);
	snprintf(d->title, sizeof(d->title), "%s", t->name);
	snprintf(d->subtitle, sizeof(d->subtitle), "%s", t->artist);
	snprintf(d->art, sizeof(d->art), "%s", t->art_large);
	snprintf(d->art_small, sizeof(d->art_small), "%s", t->art_small);
	d->year = t->year;
}

static void open_entity(app_tab tab, detail_kind kind, const char *uri)
{
	if (!uri || !uri[0] || !stack_of(tab))
		return;
	/* Whatever loads now is not what the user is after any more; if it is
	 * still on a stack it loads again later. */
	if (worker)
		job.cancel_requested = true;
	detail_doc *d = push(tab);
	if (!d)
		return;
	d->kind = kind;
	d->state = DETAIL_LOADING;
	snprintf(d->uri, sizeof(d->uri), "%s", uri);
	if (kind == DETAIL_PLAYLIST) {
		/* Made once, at its full size: a buffer that grew would move under
		 * the list and the thumbnails pointing into it. Without the memory
		 * the playlist still shows its first DETAIL_MAX_TRACKS. */
		d->owned = malloc(DETAIL_PLAYLIST_MAX * sizeof(sp_track));
		if (d->owned) {
			d->tracks = d->owned;
			d->capacity = DETAIL_PLAYLIST_MAX;
		}
	}
}

void details_open_album(app_tab tab, const char *uri)
{
	open_entity(tab, DETAIL_ALBUM, uri);
}

void details_open_artist(app_tab tab, const char *uri)
{
	open_entity(tab, DETAIL_ARTIST, uri);
}

void details_open_playlist(app_tab tab, const char *uri)
{
	open_entity(tab, DETAIL_PLAYLIST, uri);
}

/* ------------------------------------------------------------- offline */

static bool offline(const detail_doc *d)
{
	return d->kind == DETAIL_DOWNLOADS || d->kind == DETAIL_OFFLINE_COLLECTION;
}

/* A collection as a row of the Downloads list: its name, who it is by, its
 * cover. */
static void collection_as_track(const dl_collection *c, sp_track *t)
{
	memset(t, 0, sizeof(*t));
	snprintf(t->uri, sizeof(t->uri), "%s", c->uri);
	snprintf(t->name, sizeof(t->name), "%s",
	         strcmp(c->uri, SP_LIKED_URI) == 0 ? tr(STR_LIKED_SONGS) : c->name);
	snprintf(t->artist, sizeof(t->artist), "%s", c->subtitle);
	snprintf(t->art_small, sizeof(t->art_small), "%s", c->art);
	snprintf(t->art_large, sizeof(t->art_large), "%s", c->art);
}

/* Fills an offline document from the index. True when what it lists
 * changed; then its serial moves on, so the view and the card take it as
 * new, with the focus kept on the same track or collection. */
static bool fill_offline(detail_doc *d)
{
	static track_row before[DETAIL_PLAYLIST_MAX];
	static offline_item scratch[DL_TRACKS_MAX];
	const dl_index *idx = downloads_index();
	d->dl_generation = downloads_generation();
	if (!d->rows || !d->owned) {
		d->state = DETAIL_READY;
		return false;
	}
	int old_count = d->count;
	memcpy(before, d->rows, (size_t)old_count * sizeof(before[0]));
	char kept[sizeof(d->tracks[0].uri)] = "";
	if (d->count > 0)
		snprintf(kept, sizeof(kept), "%s", d->tracks[d->selected].uri);

	int n = 0;
	if (d->kind == DETAIL_DOWNLOADS) {
		int colls;
		n = offline_home_rows(idx, d->rows, d->capacity, scratch, &colls);
		for (int i = 0; i < n; i++) {
			const track_row *r = &d->rows[i];
			if (r->kind == TRACK_ROW_TRACK)
				downloads_track_to_sp(&idx->tracks[r->index], &d->tracks[i]);
			else
				collection_as_track(&idx->colls[r->index], &d->tracks[i]);
		}
		d->coll_rows = colls;
		d->track_rows = n - colls;
	} else {
		int c = idx ? dl_find_collection(idx, d->uri) : -1;
		n = offline_collection_rows(idx, c, d->rows, d->capacity);
		d->bytes = 0;
		for (int i = 0; i < n; i++) {
			const dl_track *t = &idx->tracks[d->rows[i].index];
			downloads_track_to_sp(t, &d->tracks[i]);
			if (t->state == DL_DONE)
				d->bytes += t->size;
		}
		if (c >= 0) {
			const dl_collection *coll = &idx->colls[c];
			snprintf(d->title, sizeof(d->title), "%s",
			         strcmp(coll->uri, SP_LIKED_URI) == 0
			         ? tr(STR_LIKED_SONGS) : coll->name);
			snprintf(d->subtitle, sizeof(d->subtitle), "%s", coll->subtitle);
			snprintf(d->art, sizeof(d->art), "%s", coll->art);
			snprintf(d->art_small, sizeof(d->art_small), "%s", coll->art);
			d->offline_album = coll->kind == DL_COLL_ALBUM;
		}
		d->total_tracks = n;
	}
	d->count = n;
	d->state = DETAIL_READY;

	int selected = 0;
	for (int i = 0; kept[0] && i < n; i++)
		if (strcmp(d->tracks[i].uri, kept) == 0) {
			selected = i;
			break;
		}
	int on = track_row_first_on(d->rows, n, selected);
	d->selected = on < 0 ? 0 : on;
	if (d->first > d->selected)
		d->first = d->selected;
	bool same = n == old_count &&
	            memcmp(before, d->rows, (size_t)n * sizeof(before[0])) == 0;
	if (!same)
		d->serial++;
	return !same;
}

static void open_offline(app_tab tab, detail_kind kind, const char *uri)
{
	detail_doc *d = push(tab);
	if (!d)
		return;
	d->kind = kind;
	d->state = DETAIL_READY;
	snprintf(d->uri, sizeof(d->uri), "%s", uri);
	/* Made once at their full size, like a playlist's: the list and the
	 * thumbnails point into them. Without the memory the level is empty. */
	d->owned = malloc(DETAIL_PLAYLIST_MAX * sizeof(sp_track));
	d->rows = malloc(DETAIL_PLAYLIST_MAX * sizeof(track_row));
	if (d->owned && d->rows) {
		d->tracks = d->owned;
		d->capacity = DETAIL_PLAYLIST_MAX;
	}
	fill_offline(d);
	d->selected = track_row_first_on(d->rows, d->count, 0);
	if (d->selected < 0)
		d->selected = 0;
	d->first = 0;
}

void details_open_downloads(app_tab tab)
{
	open_offline(tab, DETAIL_DOWNLOADS, DL_HOME_URI);
}

void details_open_offline_collection(app_tab tab, const char *uri)
{
	if (uri && uri[0])
		open_offline(tab, DETAIL_OFFLINE_COLLECTION, uri);
}

/* The downloads changed: every offline level on a stack follows. True
 * when one on top changed. */
static bool follow_downloads(void)
{
	unsigned now = downloads_generation();
	bool changed = false;
	for (int s = 0; s < DETAIL_STACKS; s++)
		for (int i = 0; i < stacks[s].count; i++) {
			int slot = stacks[s].slot[i];
			detail_doc *d = &docs[slot];
			if (offline(d) && d->dl_generation != now &&
			    fill_offline(d) && on_top(slot))
				changed = true;
		}
	return changed;
}

bool details_back(app_tab tab)
{
	detail_stack *s = stack_of(tab);
	int slot = s ? detail_stack_pop(s) : -1;
	if (slot < 0)
		return false;
	forget_load(slot);
	return true;
}

void details_follow(app_tab tab)
{
	detail_doc *d = details_top(tab);
	if (!d || d->kind != DETAIL_PLAYLIST || d->state != DETAIL_READY ||
	    !d->more || d->loading_more || osGetTime() < d->retry_at)
		return;
	if (detail_wants_more(d->first, d->selected, RESULTS_PER_PAGE, d->count,
	                      DETAIL_MORE_AHEAD))
		d->loading_more = true;
}

void details_retry(app_tab tab)
{
	detail_doc *d = details_top(tab);
	if (!d || d->kind == DETAIL_TRACK || d->state != DETAIL_FAILED)
		return;
	d->state = DETAIL_LOADING;
	d->error[0] = '\0';
}

void details_clear(app_tab tab)
{
	detail_stack *s = stack_of(tab);
	if (!s)
		return;
	for (int i = 0; i < s->count; i++)
		forget_load(s->slot[i]);
	detail_stack_clear(s);
}

bool details_poll(app_tab active, const char *host, const char *token,
                  const char *username)
{
	bool changed = reap();
	changed |= follow_downloads();
	if (!worker && host && host[0] && token && token[0]) {
		int slot = waiting(active);
		if (slot >= 0) {
			start(slot, host, token, username);
			changed |= docs[slot].state == DETAIL_FAILED;
		}
	}
	return changed;
}

void details_stop(void)
{
	if (worker) {
		job.cancel_requested = true;
		threadJoin(worker, U64_MAX);
		threadFree(worker);
		worker = NULL;
	}
	secure_clear(job.token, sizeof(job.token));
	secure_clear(job.username, sizeof(job.username));
	secure_clear(liked_uris, sizeof(liked_uris));
	liked_count = liked_total = 0;
	for (int i = 0; i < DETAIL_STACKS; i++)
		detail_stack_clear(&stacks[i]);
	/* Nothing shows them any more: the views re-set their lists on the
	 * next session's first level. */
	for (int i = 0; i < DETAIL_POOL; i++) {
		free(docs[i].owned);
		free(docs[i].rows);
		docs[i].owned = NULL;
		docs[i].rows = NULL;
		docs[i].tracks = docs[i].inline_tracks;
		docs[i].capacity = DETAIL_MAX_TRACKS;
		docs[i].count = 0;
	}
}
