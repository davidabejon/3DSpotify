#include "downloads.h"

#include "cfg3ds.h"
#include "dl_store.h"
#include "http3ds.h"
#include "lang.h"
#include "track.h"
#include "worker_priority.h"

#include <3ds.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A track's job logs in to an access point and opens TLS, as the player's
 * preparation does (PREPARE_STACK in main.c). */
#define TRACK_STACK (64 * 1024)
/* An http_session and a TLS handshake, as for the home feed. */
#define COLL_STACK (32 * 1024)
#define IO_STACK (16 * 1024)
/* How many tracks one collection brings at most, and how many collections
 * can wait to be resolved. */
#define COLL_TRACKS_MAX 500
#define PENDING_MAX 4
/* A changed index is written this long after the first change, so a burst
 * of changes costs one write. */
#define SAVE_DELAY_MS 2000
/* A cover is tens of KB. */
#define ART_LIMIT (512 * 1024)
/* A collection that does not resolve is tried this many times. */
#define COLL_MAX_FAILURES 3
#define COLL_RETRY_MS 15000

typedef enum { JOB_TRACK, JOB_COLLECTION } job_kind;

typedef enum {
	R_DONE,
	R_STOPPED,       /* asked to stop: waits again, from its .part */
	R_KEY_LIMIT,     /* the allowance ran out (0x0002) */
	R_KEY_REFUSED,   /* this track's key, refused for good */
	R_REKEY,         /* the kept key does not open: ask for a new one */
	R_UNAVAILABLE,   /* no file this client can keep */
	R_NET,           /* the network failed: later */
	R_NO_SPACE,
	R_WRITE,
	R_DAMAGED
} job_result;

typedef struct {
	job_kind kind;
	/* In. */
	char host[128];
	char token[1024];
	char username[128];
	downloads_ap_fn open_ap;
	char uri[SP_URI_MAX];
	bool has_key;
	uint8_t wrapped[DL_WRAPPED_LEN];
	uint8_t file_id[20];
	uint8_t salt[DL_SALT_LEN];
	char art_small[80], art_large[80];
	uint64_t free_bytes;
	dl_coll_kind coll_kind;
	/* Out. */
	job_result result;
	char error[160];
	bool key_asked;
	int refused_code;
	bool got_key;
	uint8_t new_wrapped[DL_WRAPPED_LEN];
	uint8_t new_file_id[20];
	int format;
	bool has_meta;
	sp_track meta;
	uint32_t size;
	dl_collection coll;
	dl_track *tracks;            /* JOB_COLLECTION: malloc'd */
	int track_count;
	/* Both ways. */
	volatile uint32_t done_bytes, total_bytes;
	volatile bool stop;
	volatile bool done;
} dl_job;

typedef struct {
	dl_coll_kind kind;
	char uri[SP_URI_MAX];
	char name[128];
	char art[80];
	int failures;        /* -1: removed while it was being resolved */
} pending_coll;

static dl_index *idx;
static bool loaded;          /* idx is the index on the card */
static bool unreadable;      /* the card's index is not this build's */
static Thread load_thread;
static volatile bool load_done;
static int load_rc;

static bool online;
static char host[128];
static char token[1024];
static char username[128];
static downloads_ap_fn open_ap;

static dl_job job;
static Thread job_thread;
static track_stream job_stream;
static sp_track job_batch[SP_PLAYLIST_PAGE];
static char job_uris[COLL_TRACKS_MAX][SP_URI_MAX];
static sp_album job_album;
static sp_playlist_info job_page;

static dl_bucket bucket;
static uint64_t retry_at;
static char failing_uri[SP_URI_MAX];
static int net_failures;
static bool paused, sd_full;

static pending_coll pending[PENDING_MAX];
static int pending_count;

static dl_index *save_buf;
static Thread save_thread;
static volatile bool save_done;
static bool dirty;
static uint64_t save_after;

static unsigned generation;

static void secure_clear(void *data, size_t len)
{
	volatile unsigned char *p = data;
	while (len--)
		*p++ = 0;
}

static uint64_t now_ms(void)
{
	return osGetTime();
}

/* downloads.log beside the config: what the engine did, one line per
 * event, so a download seen on the console can be explained afterwards.
 * URIs and outcomes only; never a key, a token or a signed URL. */
static void dlog(const char *fmt, ...)
{
	char path[300];
	cfg_dir(path, sizeof(path));
	size_t n = strlen(path);
	snprintf(path + n, sizeof(path) - n, "downloads.log");
	FILE *f = fopen(path, "a");
	if (!f)
		return;
	fprintf(f, "[%llu] ", (unsigned long long)now_ms());
	va_list args;
	va_start(args, fmt);
	vfprintf(f, fmt, args);
	va_end(args);
	fputc('\n', f);
	fclose(f);
}

static void changed(void)
{
	if (!dirty)
		save_after = now_ms() + SAVE_DELAY_MS;
	dirty = true;
	generation++;
}

/* The index is shown and used only for the account that made it. The
 * username stays after the session ends: it is not a secret. */
static bool usable(void)
{
	return loaded && dl_owner_matches(idx->owner, username[0] ? username
	                                                          : NULL);
}

/* --- Workers ------------------------------------------------------------- */

static void load_worker(void *arg)
{
	(void)arg;
	char err[96];
	dl_store_init();
	load_rc = dl_store_load(idx, err, sizeof(err));
	if (load_rc == 0)
		dl_store_sweep(idx);
	__sync_synchronize();
	load_done = true;
}

static void save_worker(void *arg)
{
	(void)arg;
	char err[96];
	dl_store_save(save_buf, err, sizeof(err));
	__sync_synchronize();
	save_done = true;
}

static void save_art(const char *url)
{
	if (!url[0] || dl_store_art_has(url))
		return;
	http_reply r;
	if (http_get_url(url, ART_LIMIT, &r) != 0)
		return;
	if (r.status == 200 && r.len > 0)
		dl_store_art_write(url, r.body, r.len);
	http_reply_free(&r);
}

/* Why track_stream_begin failed: a track that came back without a file
 * this client keeps will not get one by asking again; anything else
 * (metadata, storage-resolve) was the network. */
static job_result begin_failure(const track_stream *ts)
{
	if (ts->meta.uri[0] && (!ts->meta.has_gid || !sp_pick_file(&ts->meta)))
		return R_UNAVAILABLE;
	return R_NET;
}

static job_result save_result(int rc, const track_stream *ts)
{
	switch (rc) {
	case TRACK_SAVE_STOPPED:  return R_STOPPED;
	case TRACK_SAVE_NO_SPACE: return R_NO_SPACE;
	case TRACK_SAVE_WRITE:    return R_WRITE;
	default:                  return ts->cb_fatal ? R_UNAVAILABLE : R_NET;
	}
}

static void track_worker(dl_job *j)
{
	track_stream *ts = &job_stream;
	memset(ts, 0, sizeof(*ts));
	ts->dl_restart_at = -1;

	if (!j->has_key) {
		ap_session *ap = j->open_ap(j->error, sizeof(j->error));
		if (!ap) {
			j->result = R_NET;
			goto out;
		}
		int rc = track_stream_begin(j->host, j->token, ap, j->uri, NULL, ts,
		                            j->error, sizeof(j->error));
		ap_close(ap);
		j->key_asked = rc == 0 || rc == AP_KEY_REFUSED;
		if (ts->meta.uri[0] && ts->meta.has_gid) {
			j->meta = ts->meta;
			j->has_meta = true;
		}
		if (rc == AP_KEY_REFUSED) {
			j->refused_code = ts->key_refused_code;
			j->result = ts->key_refused_code == DL_KEY_LIMIT_CODE
			            ? R_KEY_LIMIT : R_KEY_REFUSED;
			goto out;
		}
		if (rc != 0) {
			j->result = begin_failure(ts);
			goto out;
		}
		/* Kept at once: if the download stops, resuming needs no key. */
		if (!dl_store_wrap_key(j->salt, j->uri, ts->key, j->new_wrapped)) {
			snprintf(j->error, sizeof(j->error), "the key could not be kept");
			j->result = R_WRITE;
			goto out;
		}
		memcpy(j->new_file_id, ts->file_id, sizeof(j->new_file_id));
		j->format = ts->format;
		j->got_key = true;
	} else {
		if (!dl_store_unwrap_key(j->salt, j->uri, j->wrapped, ts->key)) {
			j->result = R_REKEY;
			goto out;
		}
		memcpy(ts->file_id, j->file_id, sizeof(ts->file_id));
		snprintf(ts->sp_host, sizeof(ts->sp_host), "%s", j->host);
		snprintf(ts->token, sizeof(ts->token), "%s", j->token);
		if (sp_storage_resolve(j->host, j->token, ts->file_id, &ts->storage,
		                       j->error, sizeof(j->error)) <= 0) {
			j->result = R_NET;
			goto out;
		}
	}
	if (j->stop) {
		j->result = R_STOPPED;
		goto out;
	}

	char part[340];
	dl_store_track_path(j->uri, true, part, sizeof(part));
	int rc = track_save_download(ts, part, &j->stop, &j->done_bytes,
	                             &j->total_bytes, j->free_bytes);
	if (rc != 0) {
		snprintf(j->error, sizeof(j->error), "%.*s",
		         (int)sizeof(j->error) - 1, ts->dl_err);
		j->result = save_result(rc, ts);
		goto out;
	}
	uint32_t size = j->total_bytes;
	if (!track_verify_file(part, size, ts->key)) {
		/* Not the file the key opens: it goes, and so does the key, in
		 * case the key is what was wrong. */
		remove(part);
		j->result = R_DAMAGED;
		goto out;
	}
	if (!dl_store_finish_track(j->uri)) {
		j->result = R_WRITE;
		goto out;
	}
	j->size = size;
	j->result = R_DONE;
	save_art(j->has_meta ? j->meta.art_small : j->art_small);
	save_art(j->has_meta ? j->meta.art_large : j->art_large);
out:
	track_stream_free(ts);
	secure_clear(ts, sizeof(*ts));
}

static void track_from_sp(const sp_track *s, dl_track *d)
{
	memset(d, 0, sizeof(*d));
	/* A track URI is 36 characters: the list's 64 are room to spare. */
	snprintf(d->uri, sizeof(d->uri), "%.*s", (int)sizeof(d->uri) - 1, s->uri);
	snprintf(d->name, sizeof(d->name), "%s", s->name);
	snprintf(d->artist, sizeof(d->artist), "%s", s->artist);
	snprintf(d->album, sizeof(d->album), "%s", s->album);
	snprintf(d->art_small, sizeof(d->art_small), "%s", s->art_small);
	snprintf(d->art_large, sizeof(d->art_large), "%s", s->art_large);
	snprintf(d->album_uri, sizeof(d->album_uri), "%s", s->album_uri);
	snprintf(d->artist_uri, sizeof(d->artist_uri), "%s", s->artist_uri);
	d->duration_ms = s->duration_ms;
	d->track_number = s->track_number;
	d->disc_number = s->disc_number;
	d->year = s->year;
	d->is_explicit = s->is_explicit;
}

/* An album or a playlist into its header and its tracks, named. */
static void coll_worker(dl_job *j)
{
	j->result = R_NET;
	http_session *sess = http_session_open(j->host);
	if (!sess) {
		snprintf(j->error, sizeof(j->error), "no connection to %s", j->host);
		return;
	}
	int count = 0;
	if (strcmp(j->uri, SP_LIKED_URI) == 0) {
		int total = 0;
		count = sp_liked_s(sess, j->token, j->username, job_uris,
		                   COLL_TRACKS_MAX, &total, j->error,
		                   sizeof(j->error));
		j->coll.total = total;
	} else if (j->coll_kind == DL_COLL_ALBUM) {
		if (sp_album_metadata(sess, j->token, j->uri, &job_album, j->error,
		                      sizeof(j->error)) == 0) {
			snprintf(j->coll.name, sizeof(j->coll.name), "%s", job_album.name);
			snprintf(j->coll.subtitle, sizeof(j->coll.subtitle), "%s",
			         job_album.artist);
			snprintf(j->coll.art, sizeof(j->coll.art), "%s",
			         job_album.art_large);
			j->coll.total = job_album.total_tracks;
			for (int i = 0; i < job_album.track_count; i++)
				memcpy(job_uris[count++], job_album.track_uris[i], SP_URI_MAX);
		} else {
			count = -1;
		}
	} else {
		for (int from = 0; count < COLL_TRACKS_MAX && !j->stop; ) {
			if (sp_playlist_s(sess, j->token, j->uri, from, SP_PLAYLIST_PAGE,
			                  &job_page, j->error, sizeof(j->error)) != 0) {
				count = -1;
				break;
			}
			if (from == 0) {
				snprintf(j->coll.name, sizeof(j->coll.name), "%s",
				         job_page.name);
				snprintf(j->coll.art, sizeof(j->coll.art), "%s", job_page.art);
				j->coll.total = job_page.total_tracks;
			}
			for (int i = 0; i < job_page.track_count && count < COLL_TRACKS_MAX;
			     i++)
				memcpy(job_uris[count++], job_page.track_uris[i], SP_URI_MAX);
			if (job_page.items <= 0)
				break;
			from += job_page.items;
			if (from >= job_page.total_tracks)
				break;
		}
	}
	if (count < 0 || j->stop) {
		http_session_close(sess);
		if (j->stop)
			j->result = R_STOPPED;
		return;
	}

	j->tracks = count ? malloc((size_t)count * sizeof(dl_track)) : NULL;
	if (count && !j->tracks) {
		snprintf(j->error, sizeof(j->error), "out of memory");
		http_session_close(sess);
		j->result = R_WRITE;
		return;
	}
	const char *ptrs[SP_PLAYLIST_PAGE];
	for (int from = 0; from < count && !j->stop; from += SP_PLAYLIST_PAGE) {
		int k = count - from < SP_PLAYLIST_PAGE ? count - from
		                                        : SP_PLAYLIST_PAGE;
		for (int i = 0; i < k; i++)
			ptrs[i] = job_uris[from + i];
		int got = sp_track_metadata_batch_s(sess, j->token, ptrs, k,
		                                    job_batch, j->error,
		                                    sizeof(j->error));
		if (got < 0) {
			http_session_close(sess);
			free(j->tracks);
			j->tracks = NULL;
			return;
		}
		/* The batch answers in the order asked, minus what it left out. */
		for (int i = 0; i < got; i++)
			track_from_sp(&job_batch[i], &j->tracks[j->track_count++]);
	}
	http_session_close(sess);
	j->result = j->stop ? R_STOPPED : R_DONE;
}

static void job_worker(void *arg)
{
	dl_job *j = arg;
	if (j->kind == JOB_TRACK)
		track_worker(j);
	else
		coll_worker(j);
	secure_clear(j->token, sizeof(j->token));
	__sync_synchronize();
	j->done = true;
}

/* --- Main thread --------------------------------------------------------- */

void downloads_init(void)
{
	idx = malloc(sizeof(dl_index));
	if (!idx)
		return;
	dl_index_clear(idx);
	dl_bucket_init(&bucket, now_ms());
	/* One run's log: the last session is the one worth reading. */
	char path[300];
	cfg_dir(path, sizeof(path));
	size_t n = strlen(path);
	snprintf(path + n, sizeof(path) - n, "downloads.log");
	remove(path);
	load_thread = threadCreate(load_worker, NULL, IO_STACK,
	                           background_priority(), -2, false);
	if (!load_thread)
		dlog("could not start loading the index");
}

static void reap_load(void)
{
	if (!load_thread || !load_done)
		return;
	__sync_synchronize();
	threadJoin(load_thread, U64_MAX);
	threadFree(load_thread);
	load_thread = NULL;
	if (load_rc == 0) {
		loaded = true;
		dlog("index: %d tracks, %d collections", idx->track_count,
		     idx->coll_count);
	} else {
		unreadable = true;
		dl_index_clear(idx);
		dlog("index unreadable (another version?): downloads off");
	}
	generation++;
}

static void reap_save(void)
{
	if (!save_thread || !save_done)
		return;
	__sync_synchronize();
	threadJoin(save_thread, U64_MAX);
	threadFree(save_thread);
	save_thread = NULL;
}

static void maybe_save(uint64_t now)
{
	reap_save();
	if (!dirty || !loaded || save_thread || now < save_after)
		return;
	if (!save_buf)
		save_buf = malloc(sizeof(dl_index));
	if (!save_buf)
		return;
	memcpy(save_buf, idx, sizeof(dl_index));
	dirty = false;
	save_done = false;
	save_thread = threadCreate(save_worker, NULL, IO_STACK,
	                           background_priority(), -2, false);
	if (!save_thread)
		dirty = true;
}

static const char *result_text(job_result r)
{
	static const char *const NAMES[] = {
		"done", "stopped", "key limit", "key refused", "key does not open",
		"unavailable", "network", "SD full", "SD write", "damaged",
	};
	return NAMES[r];
}

static void apply_track(uint64_t now, bool stopping)
{
	if (job.key_asked) {
		dl_bucket_take(&bucket, now);
		if (job.result == R_KEY_LIMIT)
			dl_bucket_refused(&bucket, now);
	}
	dlog("%s: %s%s%s (%u of %u bytes; allowance %d)", job.uri,
	     result_text(job.result), job.error[0] ? ": " : "", job.error,
	     (unsigned)job.done_bytes, (unsigned)job.total_bytes, bucket.tokens);
	int t = dl_find_track(idx, job.uri);
	if (t < 0) {
		/* Removed while it downloaded: what it wrote goes too. */
		dl_store_remove_track(job.uri);
		return;
	}
	dl_track *d = &idx->tracks[t];
	if (job.has_meta) {
		/* The service's names are fresher than the list's. */
		dl_track fresh;
		track_from_sp(&job.meta, &fresh);
		snprintf(d->name, sizeof(d->name), "%s", fresh.name);
		snprintf(d->artist, sizeof(d->artist), "%s", fresh.artist);
		snprintf(d->album, sizeof(d->album), "%s", fresh.album);
		if (fresh.art_small[0])
			snprintf(d->art_small, sizeof(d->art_small), "%s",
			         fresh.art_small);
		if (fresh.art_large[0])
			snprintf(d->art_large, sizeof(d->art_large), "%s",
			         fresh.art_large);
		d->duration_ms = fresh.duration_ms;
	}
	if (job.got_key)
		dl_set_key(idx, t, job.new_wrapped, job.new_file_id, job.format);
	bool same = strcmp(failing_uri, job.uri) == 0;
	switch (job.result) {
	case R_DONE:
		dl_mark_done(idx, t, job.size);
		break;
	case R_STOPPED:
	case R_KEY_LIMIT:
		dl_mark_paused(idx, t);
		break;
	case R_REKEY:
		d->has_key = false;
		dl_mark_paused(idx, t);
		break;
	case R_KEY_REFUSED:
		dl_mark_failed(idx, t, tr(STR_ERR_KEY_REFUSED));
		break;
	case R_UNAVAILABLE:
		dl_mark_failed(idx, t, tr(STR_DL_ERR_UNAVAILABLE));
		break;
	case R_NET: {
		if (stopping) {
			/* The TLS abort of a session ending, not the network. */
			dl_mark_paused(idx, t);
			break;
		}
		net_failures = same ? net_failures + 1 : 1;
		snprintf(failing_uri, sizeof(failing_uri), "%s", job.uri);
		uint32_t wait = dl_net_retry_ms(net_failures);
		if (wait) {
			dl_mark_paused(idx, t);
			retry_at = now + wait;
		} else {
			dl_mark_failed(idx, t, tr(STR_DL_ERR_NETWORK));
			net_failures = 0;
			failing_uri[0] = '\0';
		}
		break;
	}
	case R_NO_SPACE:
		dl_mark_paused(idx, t);
		sd_full = true;
		break;
	case R_WRITE:
		dl_mark_failed(idx, t, tr(STR_DL_ERR_WRITE));
		break;
	case R_DAMAGED:
		d->has_key = false;
		dl_mark_failed(idx, t, tr(STR_DL_ERR_DAMAGED));
		break;
	}
	if (job.result != R_NET && same) {
		net_failures = 0;
		failing_uri[0] = '\0';
	}
}

static void apply_collection(uint64_t now, bool stopping)
{
	dlog("%s: %s, %d tracks%s%s", job.uri, result_text(job.result),
	     job.track_count, job.error[0] ? ": " : "", job.error);
	/* The first one waiting is the one that ran. */
	pending_coll p = pending[0];
	if (p.failures < 0) {
		/* Removed meanwhile: what it resolved is not wanted. */
	} else if (job.result != R_DONE) {
		if (stopping || job.result == R_STOPPED ||
		    ++p.failures < COLL_MAX_FAILURES) {
			pending[0] = p;
			if (!stopping)
				retry_at = now + COLL_RETRY_MS;
			return;
		}
	} else {
		dl_collection c = job.coll;
		snprintf(c.uri, sizeof(c.uri), "%s", p.uri);
		c.kind = p.kind;
		if (!c.name[0])
			snprintf(c.name, sizeof(c.name), "%s", p.name);
		if (!c.art[0])
			snprintf(c.art, sizeof(c.art), "%s", p.art);
		int ci = dl_add_collection(idx, &c, (uint32_t)time(NULL));
		for (int i = 0; ci >= 0 && i < job.track_count; i++) {
			int t = dl_add_track(idx, &job.tracks[i], false,
			                     (uint32_t)time(NULL));
			if (t >= 0 && !dl_add_ref(idx, ci, t))
				break;
		}
	}
	memmove(&pending[0], &pending[1],
	        (size_t)(pending_count - 1) * sizeof(pending[0]));
	pending_count--;
}

static void reap_job(bool stopping)
{
	if (!job_thread || !job.done)
		return;
	__sync_synchronize();
	threadJoin(job_thread, U64_MAX);
	threadFree(job_thread);
	job_thread = NULL;
	uint64_t now = now_ms();
	if (job.kind == JOB_TRACK)
		apply_track(now, stopping);
	else
		apply_collection(now, stopping);
	free(job.tracks);
	secure_clear(&job, sizeof(job));
	changed();
}

static bool start_job(void)
{
	job.done = false;
	job.stop = false;
	snprintf(job.host, sizeof(job.host), "%s", host);
	snprintf(job.token, sizeof(job.token), "%s", token);
	snprintf(job.username, sizeof(job.username), "%s", username);
	job.open_ap = open_ap;
	job_thread = threadCreate(job_worker, &job,
	                          job.kind == JOB_TRACK ? TRACK_STACK : COLL_STACK,
	                          background_priority(), -2, false);
	if (!job_thread) {
		secure_clear(&job, sizeof(job));
		return false;
	}
	return true;
}

static void start_next(uint64_t now)
{
	if (now < retry_at)
		return;
	if (pending_count > 0) {
		memset(&job, 0, sizeof(job));
		job.kind = JOB_COLLECTION;
		job.coll_kind = pending[0].kind;
		snprintf(job.uri, sizeof(job.uri), "%s", pending[0].uri);
		if (start_job())
			dlog("%s: resolving", job.uri);
		return;
	}
	int t = dl_next_queued(idx);
	if (t < 0)
		return;
	const dl_track *d = &idx->tracks[t];
	if (!d->has_key && !dl_bucket_engine_may_take(&bucket, now))
		return;
	if (!dl_store_ensure_salt(idx))
		return;
	memset(&job, 0, sizeof(job));
	job.kind = JOB_TRACK;
	snprintf(job.uri, sizeof(job.uri), "%s", d->uri);
	job.has_key = d->has_key;
	memcpy(job.wrapped, d->wrapped_key, sizeof(job.wrapped));
	memcpy(job.file_id, d->file_id, sizeof(job.file_id));
	memcpy(job.salt, idx->salt, sizeof(job.salt));
	snprintf(job.art_small, sizeof(job.art_small), "%s", d->art_small);
	snprintf(job.art_large, sizeof(job.art_large), "%s", d->art_large);
	job.free_bytes = dl_store_free_bytes();
	if (!start_job())
		return;
	dlog("%s: start (%s, allowance %d)", job.uri,
	     job.has_key ? "key kept" : "new key", bucket.tokens);
	dl_mark_active(idx, t);
	changed();
}

bool downloads_poll(bool player_busy)
{
	unsigned before = generation;
	uint64_t now = now_ms();
	reap_load();
	reap_job(false);
	if (job_thread && job.kind == JOB_TRACK && (player_busy || paused))
		job.stop = true;
	if (usable() && online && !job_thread && !paused && !sd_full &&
	    !player_busy)
		start_next(now);
	maybe_save(now);
	return generation != before;
}

bool downloads_busy(void)
{
	return job_thread && job.kind == JOB_TRACK;
}

void downloads_note_player_key(int rc, int refused_code)
{
	uint64_t now = now_ms();
	if (rc == 0 || rc == AP_KEY_REFUSED)
		dl_bucket_take(&bucket, now);
	if (rc == AP_KEY_REFUSED && refused_code == DL_KEY_LIMIT_CODE) {
		dl_bucket_refused(&bucket, now);
		dlog("player: key limit");
	}
}

void downloads_start(const char *h, const char *tok, const char *user,
                     downloads_ap_fn ap_fn)
{
	snprintf(host, sizeof(host), "%s", h ? h : "");
	snprintf(token, sizeof(token), "%.*s", (int)sizeof(token) - 1,
	         tok ? tok : "");
	snprintf(username, sizeof(username), "%s", user ? user : "");
	open_ap = ap_fn;
	online = host[0] && token[0] && open_ap;
	sd_full = false;
	retry_at = 0;
	dlog("session: %s, index %s", online ? "online" : "incomplete",
	     unreadable ? "unreadable" : loaded ? "loaded" : "loading");
	generation++;
}

void downloads_stop(void)
{
	if (job_thread) {
		job.stop = true;
		while (!job.done)
			svcSleepThread(10 * 1000000LL);
		reap_job(true);
	}
	secure_clear(token, sizeof(token));
	host[0] = '\0';
	open_ap = NULL;
	online = false;
	generation++;
}

void downloads_exit(void)
{
	downloads_stop();
	if (load_thread) {
		threadJoin(load_thread, U64_MAX);
		threadFree(load_thread);
		load_thread = NULL;
	}
	if (save_thread) {
		threadJoin(save_thread, U64_MAX);
		threadFree(save_thread);
		save_thread = NULL;
	}
	if (dirty && loaded) {
		char err[96];
		dl_store_save(idx, err, sizeof(err));
	}
	free(save_buf);
	free(idx);
	save_buf = NULL;
	idx = NULL;
	loaded = false;
}

/* Whether something may be added, and if not, why (a downloads.h text).
 * The first download makes the index this account's. */
static str_id claim(void)
{
	str_id why = STR_COUNT;
	if (unreadable)
		why = STR_DL_UNREADABLE;
	else if (!loaded || !online)
		why = STR_DL_NOT_READY;
	else if (!usable())
		why = STR_DL_OTHER_ACCOUNT;
	if (why != STR_COUNT) {
		dlog("refused: %s (loaded %d, online %d, owner %s)",
		     why == STR_DL_UNREADABLE ? "index unreadable"
		     : why == STR_DL_NOT_READY ? "not ready" : "another account",
		     loaded, online, idx && idx->owner[0] ? "set" : "none");
		return why;
	}
	if (!idx->owner[0]) {
		snprintf(idx->owner, sizeof(idx->owner), "%s", username);
		changed();
	}
	return STR_COUNT;
}

str_id downloads_add_track(const sp_track *t)
{
	if (!t)
		return STR_DL_NOT_READY;
	str_id why = claim();
	if (why != STR_COUNT)
		return why;
	int had = dl_find_track(idx, t->uri);
	if (had >= 0 && idx->tracks[had].state != DL_FAILED) {
		idx->tracks[had].standalone = true;
		dlog("%s: already in (%d)", t->uri, idx->tracks[had].state);
		changed();
		return STR_DL_ALREADY;
	}
	dl_track d;
	track_from_sp(t, &d);
	int at = dl_add_track(idx, &d, true, (uint32_t)time(NULL));
	if (at < 0) {
		dlog("%s: refused, the index is full", d.uri);
		return STR_DL_FULL;
	}
	if (idx->tracks[at].state == DL_FAILED)
		dl_requeue(idx, at);
	dlog("%s: queued", d.uri);
	changed();
	return STR_DL_ADDED;
}

str_id downloads_add_track_keyed(const sp_track *t, const uint8_t key[16],
                                 const uint8_t file_id[20], int format)
{
	str_id said = downloads_add_track(t);
	if (said != STR_DL_ADDED && said != STR_DL_ALREADY)
		return said;
	int at = dl_find_track(idx, t->uri);
	if (at < 0 || idx->tracks[at].has_key || !dl_store_ensure_salt(idx))
		return said;
	uint8_t wrapped[DL_WRAPPED_LEN];
	if (dl_store_wrap_key(idx->salt, t->uri, key, wrapped)) {
		dl_set_key(idx, at, wrapped, file_id, format);
		dlog("%s: with the player's key", t->uri);
	}
	secure_clear(wrapped, sizeof(wrapped));
	return said;
}

str_id downloads_add_collection(dl_coll_kind kind, const char *uri,
                                const char *name, const char *art)
{
	if (!uri || !uri[0])
		return STR_DL_NOT_READY;
	str_id why = claim();
	if (why != STR_COUNT)
		return why;
	for (int i = 0; i < pending_count; i++)
		if (strcmp(pending[i].uri, uri) == 0)
			return STR_DL_ALREADY;
	if (pending_count >= PENDING_MAX) {
		dlog("%s: refused, %d collections wait", uri, pending_count);
		return STR_DL_FULL;
	}
	pending_coll *p = &pending[pending_count++];
	memset(p, 0, sizeof(*p));
	p->kind = kind;
	snprintf(p->uri, sizeof(p->uri), "%s", uri);
	snprintf(p->name, sizeof(p->name), "%s", name ? name : "");
	snprintf(p->art, sizeof(p->art), "%s", art ? art : "");
	dlog("%s: collection queued", uri);
	generation++;
	return STR_DL_ADDED;
}

bool downloads_track_state(const char *uri, dl_state *state, int *pct,
                           bool *standalone)
{
	if (!usable())
		return false;
	int t = dl_find_track(idx, uri);
	if (t < 0)
		return false;
	if (state)
		*state = idx->tracks[t].state;
	if (standalone)
		*standalone = idx->tracks[t].standalone;
	if (pct) {
		*pct = 0;
		/* The worker writes both as it goes: read each once. */
		uint32_t total = job.total_bytes, got = job.done_bytes;
		if (idx->tracks[t].state == DL_ACTIVE && job_thread &&
		    job.kind == JOB_TRACK && strcmp(job.uri, uri) == 0 && total)
			*pct = got >= total ? 100
			                    : (int)((uint64_t)got * 100 / total);
	}
	return true;
}

bool downloads_collection_state(const char *uri, bool *pending_out,
                                int *done, int *count)
{
	*pending_out = false;
	*done = *count = 0;
	if (!usable() || !uri)
		return false;
	for (int i = 0; i < pending_count; i++)
		if (strcmp(pending[i].uri, uri) == 0 && pending[i].failures >= 0) {
			*pending_out = true;
			return true;
		}
	int c = dl_find_collection(idx, uri);
	if (c < 0)
		return false;
	dl_collection_progress(idx, c, done, count);
	return true;
}

bool downloads_local_track(const char *uri, downloads_local *out)
{
	if (!usable() || !uri || !idx->has_salt)
		return false;
	int t = dl_find_track(idx, uri);
	if (t < 0 || idx->tracks[t].state != DL_DONE || !idx->tracks[t].has_key)
		return false;
	dl_store_track_path(uri, false, out->path, sizeof(out->path));
	memcpy(out->wrapped, idx->tracks[t].wrapped_key, sizeof(out->wrapped));
	memcpy(out->salt, idx->salt, sizeof(out->salt));
	out->format = idx->tracks[t].format;
	return true;
}

const dl_index *downloads_index(void)
{
	return usable() ? idx : NULL;
}

unsigned downloads_generation(void)
{
	return generation;
}

/* The worker is on `uri`: it stops, and reap_job deletes what it wrote
 * once the track is gone from the index. */
static void stop_if_running(const char *uri)
{
	if (job_thread && strcmp(job.uri, uri) == 0)
		job.stop = true;
}

void downloads_remove(const char *uri)
{
	if (!usable() || !uri)
		return;
	for (int i = 0; i < pending_count; i++)
		if (strcmp(pending[i].uri, uri) == 0) {
			stop_if_running(uri);
			if (i == 0 && job_thread && job.kind == JOB_COLLECTION) {
				pending[0].failures = -1;   /* reap_job takes it off */
				break;
			}
			memmove(&pending[i], &pending[i + 1],
			        (size_t)(pending_count - i - 1) * sizeof(pending[0]));
			pending_count--;
			break;
		}
	int c = dl_find_collection(idx, uri);
	if (c >= 0) {
		static char orphans[COLL_TRACKS_MAX][SP_URI_MAX];
		int n = dl_remove_collection(idx, c, orphans, COLL_TRACKS_MAX);
		for (int i = 0; i < n && i < COLL_TRACKS_MAX; i++) {
			stop_if_running(orphans[i]);
			if (!(job_thread && strcmp(job.uri, orphans[i]) == 0))
				dl_store_remove_track(orphans[i]);
		}
		dlog("%s: collection removed, %d tracks with it", uri, n);
		sd_full = false;
		changed();
		return;
	}
	int t = dl_find_track(idx, uri);
	if (t >= 0 && dl_unwant_track(idx, t)) {
		stop_if_running(uri);
		if (!(job_thread && strcmp(job.uri, uri) == 0))
			dl_store_remove_track(uri);
		dlog("%s: removed", uri);
		sd_full = false;
	}
	changed();
}

void downloads_cancel(const char *uri)
{
	downloads_remove(uri);
}

void downloads_retry(const char *uri)
{
	if (!usable() || !uri)
		return;
	int t = dl_find_track(idx, uri);
	if (t < 0 || idx->tracks[t].state != DL_FAILED)
		return;
	dl_requeue(idx, t);
	net_failures = 0;
	failing_uri[0] = '\0';
	retry_at = 0;
	sd_full = false;
	changed();
}

void downloads_set_paused(bool p)
{
	paused = p;
	if (!p)
		sd_full = false;
	generation++;
}

bool downloads_paused(void)
{
	return paused;
}

bool downloads_sd_full(void)
{
	return sd_full;
}
