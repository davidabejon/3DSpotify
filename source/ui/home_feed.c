#include "home_feed.h"

#include "home_feed_logic.h"
#include "http3ds.h"
#include "lang.h"
#include "worker_priority.h"

#include <3ds.h>

#include <stdio.h>
#include <string.h>

/* An http_session and a TLS handshake, as for the details. */
#define HOME_WORKER_STACK (32 * 1024)
/* The folder entries read: the merge needs them all to place the recent
 * ones, and an account rarely has more. Beyond, the rest still count in
 * the total. */
#define HOME_ROOT_MAX 64

typedef struct {
	char host[128];
	char token[1024];
	char username[128];
	sp_playlist_ref root[HOME_ROOT_MAX];
	char recent[HOME_MAX_PLAYLISTS][SP_URI_MAX];
	sp_playlist_info header;
	sp_playlist_ref out[HOME_MAX_PLAYLISTS];
	bool missing[HOME_MAX_PLAYLISTS];
	int  count, total;
	bool is_recent;
	int  result;                  /* 0 or -1 */
	char error[192];
	volatile bool cancel_requested;
	volatile bool done;
} home_job;

static home_job job;
static Thread worker;
static home_feed_state state;
static unsigned generation;
static sp_playlist_ref items[HOME_MAX_PLAYLISTS];
static int item_count, item_total;
static bool items_recent;
static char error[192];

static void secure_clear(void *data, size_t len)
{
	volatile unsigned char *p = data;
	while (len--)
		*p++ = 0;
}

static void worker_main(void *arg)
{
	(void)arg;
	int rc = -1;
	http_session *sess = http_session_open(job.host);
	if (!sess) {
		snprintf(job.error, sizeof(job.error),
		         tr(STR_ERR_DETAIL_CONNECT_FMT), job.host);
		goto done;
	}

	/* The history first: without it the folder alone still makes a home,
	 * so its failure only costs the order. */
	char detail[192];
	int nrecent = sp_recent_playlists_s(sess, job.token, job.username,
	                                    job.recent, HOME_MAX_PLAYLISTS,
	                                    detail, sizeof(detail));
	if (nrecent < 0)
		nrecent = 0;
	job.is_recent = nrecent > 0;

	int root_total = 0;
	int nroot = job.cancel_requested
	            ? -1
	            : sp_rootlist_s(sess, job.token, job.username, job.root,
	                            HOME_ROOT_MAX, &root_total, job.error,
	                            sizeof(job.error));
	if (nroot < 0) {
		if (nrecent == 0 || job.cancel_requested)
			goto close;
		nroot = 0;                /* the history alone, then */
	}

	job.count = home_merge((const char (*)[SP_URI_MAX])job.recent, nrecent,
	                       job.root, nroot, root_total, job.out, job.missing,
	                       HOME_MAX_PLAYLISTS, &job.total);

	/* Recent playlists outside the folder: their own header names them.
	 * One that does not answer (deleted, private) is left out. */
	for (int i = 0; i < job.count && !job.cancel_requested; ) {
		if (!job.missing[i]) {
			i++;
			continue;
		}
		if (sp_playlist_s(sess, job.token, job.out[i].uri, 0, &job.header,
		                  detail, sizeof(detail)) == 0) {
			snprintf(job.out[i].name, sizeof(job.out[i].name), "%s",
			         job.header.name);
			snprintf(job.out[i].art, sizeof(job.out[i].art), "%s",
			         job.header.art);
			job.out[i].length = job.header.total_tracks;
			job.missing[i] = false;
			i++;
		} else {
			job.count = home_drop(job.out, job.missing, job.count, i);
			job.total--;
		}
	}
	rc = job.cancel_requested ? -1 : 0;
close:
	http_session_close(sess);
done:
	job.result = rc;
	__sync_synchronize();
	job.done = true;
}

static void start(const char *host, const char *token, const char *username)
{
	if (worker || !host || !host[0] || !token || !token[0] || !username ||
	    !username[0])
		return;
	memset(&job, 0, sizeof(job));
	snprintf(job.host, sizeof(job.host), "%s", host);
	snprintf(job.token, sizeof(job.token), "%.*s",
	         (int)sizeof(job.token) - 1, token);
	snprintf(job.username, sizeof(job.username), "%s", username);
	error[0] = '\0';
	state = HOME_FEED_LOADING;
	generation++;
	/* Below the decoder, like every worker that can overlap playback. */
	worker = threadCreate(worker_main, NULL, HOME_WORKER_STACK,
	                      background_priority(), -2, false);
	if (!worker) {
		secure_clear(job.token, sizeof(job.token));
		secure_clear(job.username, sizeof(job.username));
		snprintf(error, sizeof(error), "%s", tr(STR_ERR_DETAIL_THREAD));
		state = HOME_FEED_FAILED;
		generation++;
	}
}

void home_feed_start(const char *host, const char *token,
                     const char *username)
{
	if (state == HOME_FEED_IDLE)
		start(host, token, username);
}

void home_feed_retry(const char *host, const char *token,
                     const char *username)
{
	if (state == HOME_FEED_FAILED)
		start(host, token, username);
}

bool home_feed_poll(void)
{
	if (!worker || !job.done)
		return false;
	__sync_synchronize();
	threadJoin(worker, U64_MAX);
	threadFree(worker);
	worker = NULL;
	secure_clear(job.token, sizeof(job.token));
	secure_clear(job.username, sizeof(job.username));
	if (job.result != 0) {
		snprintf(error, sizeof(error), "%s", job.error);
		state = HOME_FEED_FAILED;
		generation++;
		return true;
	}
	memcpy(items, job.out, (size_t)job.count * sizeof(items[0]));
	item_count = job.count;
	item_total = job.total > job.count ? job.total : job.count;
	items_recent = job.is_recent;
	state = HOME_FEED_READY;
	generation++;
	return true;
}

void home_feed_stop(void)
{
	if (worker) {
		job.cancel_requested = true;
		threadJoin(worker, U64_MAX);
		threadFree(worker);
		worker = NULL;
	}
	secure_clear(job.token, sizeof(job.token));
	secure_clear(job.username, sizeof(job.username));
	state = HOME_FEED_IDLE;
	generation++;
	item_count = item_total = 0;
	error[0] = '\0';
}

home_feed_state home_feed_get_state(void)
{
	return state;
}

unsigned home_feed_generation(void)
{
	return generation;
}

const sp_playlist_ref *home_feed_items(int *count)
{
	if (count)
		*count = state == HOME_FEED_READY ? item_count : 0;
	return items;
}

int home_feed_total(void)
{
	return state == HOME_FEED_READY ? item_total : 0;
}

bool home_feed_is_recent(void)
{
	return items_recent;
}

const char *home_feed_error(void)
{
	return error;
}
