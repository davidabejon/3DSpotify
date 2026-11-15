#include "queue_feed.h"

#include "details.h"
#include "http3ds.h"
#include "worker_priority.h"

#include <3ds.h>

#include <stdio.h>
#include <string.h>

/* An http_session, a TLS handshake and a metadata batch, as the details'
 * worker. */
#define QUEUE_FEED_STACK (32 * 1024)
#define QUEUE_FEED_MAX (SP_PLAYLIST_PAGE > QUEUE_RADIO_BATCH \
                        ? SP_PLAYLIST_PAGE : QUEUE_RADIO_BATCH)

typedef enum { FEED_PLAYLIST, FEED_RADIO } feed_kind;

/* Everything the worker touches. Global, never on a thread's stack: the
 * tracks alone are ~50 KB. */
typedef struct {
	feed_kind kind;
	unsigned tag;
	int  from;
	char uri[SP_URI_MAX];
	char host[128];
	char token[1024];
	char username[128];
	sp_playlist_info playlist;
	char radio_uris[QUEUE_RADIO_BATCH][SP_URI_MAX];
	const char *uris[QUEUE_FEED_MAX];
	sp_track tracks[QUEUE_FEED_MAX];
	int  count;
	int  items, total;
	int  result;                  /* 0 or -1 */
	char error[192];
	volatile bool cancel_requested;
	volatile bool done;
} feed_job;

static feed_job job;
static Thread worker;
/* Liked Songs, as the first page of a tag read them: the next pages name
 * theirs from here. Worker only, one load at a time. */
static char liked_uris[DETAIL_PLAYLIST_MAX][SP_URI_MAX];
static int liked_count, liked_total;
static unsigned liked_tag;
static bool liked_valid;

static void secure_clear(void *data, size_t len)
{
	volatile unsigned char *p = data;
	while (len--)
		*p++ = 0;
}

/* --------------------------------------------------------------- worker */

/* The radio's tracks by URI, then named over one connection. */
static int load_radio(void)
{
	int got = sp_radio(job.host, job.token, job.uri, QUEUE_RADIO_BATCH,
	                   job.tracks, QUEUE_RADIO_BATCH, job.error,
	                   sizeof(job.error));
	if (got <= 0 || job.cancel_requested)
		return got < 0 ? -1 : 0;
	/* The batch writes over job.tracks: the URIs it asks for are copied
	 * out first. */
	for (int i = 0; i < got; i++) {
		snprintf(job.radio_uris[i], SP_URI_MAX, "%s", job.tracks[i].uri);
		job.uris[i] = job.radio_uris[i];
	}
	return sp_track_metadata_batch(job.host, job.token, job.uris, got,
	                               job.tracks, job.error, sizeof(job.error));
}

/* One page of a playlist, or of Liked Songs, named on the same link. */
static int load_page(void)
{
	http_session *sess = http_session_open(job.host);
	if (!sess) {
		snprintf(job.error, sizeof(job.error), "queue: no connection");
		return -1;
	}
	int n = -1;
	if (strcmp(job.uri, SP_LIKED_URI) == 0) {
		if (!liked_valid || liked_tag != job.tag) {
			int got = sp_liked_s(sess, job.token, job.username, liked_uris,
			                     DETAIL_PLAYLIST_MAX, &liked_total,
			                     job.error, sizeof(job.error));
			liked_valid = got >= 0;
			liked_count = got < 0 ? 0 : got;
			liked_tag = job.tag;
		}
		if (liked_valid) {
			int from = job.from < liked_count ? job.from : liked_count;
			n = liked_count - from;
			if (n > SP_PLAYLIST_PAGE)
				n = SP_PLAYLIST_PAGE;
			for (int i = 0; i < n; i++)
				job.uris[i] = liked_uris[from + i];
			job.items = n;
			job.total = liked_total;
		}
	} else if (sp_playlist_s(sess, job.token, job.uri, job.from,
	                         SP_PLAYLIST_PAGE, &job.playlist, job.error,
	                         sizeof(job.error)) == 0) {
		n = job.playlist.track_count;
		for (int i = 0; i < n; i++)
			job.uris[i] = job.playlist.track_uris[i];
		job.items = job.playlist.items;
		job.total = job.playlist.total_tracks;
	}
	if (n > 0 && !job.cancel_requested)
		n = sp_track_metadata_batch_s(sess, job.token, job.uris, n,
		                              job.tracks, job.error,
		                              sizeof(job.error));
	http_session_close(sess);
	return n;
}

static void worker_main(void *arg)
{
	(void)arg;
	int n = job.kind == FEED_RADIO ? load_radio() : load_page();
	job.count = n < 0 ? 0 : n;
	job.result = n < 0 ? -1 : 0;
	__sync_synchronize();
	job.done = true;
}

/* ------------------------------------------------------------ main side */

static void reap(void)
{
	__sync_synchronize();
	threadJoin(worker, U64_MAX);
	threadFree(worker);
	worker = NULL;
	secure_clear(job.token, sizeof(job.token));
	secure_clear(job.username, sizeof(job.username));
}

bool queue_feed_idle(void)
{
	return !worker;
}

static bool start(feed_kind kind, const char *host, const char *token,
                  const char *username, const char *uri, int from,
                  unsigned tag)
{
	if (worker || !host || !host[0] || !token || !token[0] || !uri ||
	    !uri[0])
		return false;
	/* Not memset: the job is ~60 KB, and the worker fills what it uses. */
	job.kind = kind;
	job.tag = tag;
	job.from = from;
	job.count = job.items = job.total = 0;
	job.result = -1;
	job.error[0] = '\0';
	job.cancel_requested = false;
	job.done = false;
	snprintf(job.uri, sizeof(job.uri), "%s", uri);
	snprintf(job.host, sizeof(job.host), "%s", host);
	snprintf(job.token, sizeof(job.token), "%s", token);
	snprintf(job.username, sizeof(job.username), "%s",
	         username ? username : "");
	/* Below the decoder, like every worker that can overlap playback. */
	worker = threadCreate(worker_main, NULL, QUEUE_FEED_STACK,
	                      background_priority(), -2, false);
	if (!worker) {
		secure_clear(job.token, sizeof(job.token));
		secure_clear(job.username, sizeof(job.username));
		return false;
	}
	return true;
}

bool queue_feed_playlist(const char *host, const char *token,
                         const char *username, const char *uri, int from,
                         unsigned tag)
{
	return start(FEED_PLAYLIST, host, token, username, uri, from, tag);
}

bool queue_feed_radio(const char *host, const char *token,
                      const char *seed_uri, unsigned tag)
{
	return start(FEED_RADIO, host, token, NULL, seed_uri, 0, tag);
}

void queue_feed_cancel(void)
{
	if (worker)
		job.cancel_requested = true;
}

bool queue_feed_poll(queue_feed_result *out)
{
	if (!worker || !job.done)
		return false;
	reap();
	if (job.cancel_requested)
		return false;
	out->tag = job.tag;
	out->count = job.result == 0 ? job.count : -1;
	out->tracks = job.tracks;
	out->items = job.items;
	out->total = job.total;
	return true;
}

void queue_feed_stop(void)
{
	if (worker) {
		job.cancel_requested = true;
		reap();
	}
	secure_clear(job.token, sizeof(job.token));
	secure_clear(job.username, sizeof(job.username));
	secure_clear(liked_uris, sizeof(liked_uris));
	liked_count = liked_total = 0;
	liked_valid = false;
}
