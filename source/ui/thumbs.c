#include "thumbs.h"

#include "art_url.h"
#include "http3ds.h"
#include "image_util.h"
#include "list_logic.h"
#include "thumb_queue.h"
#include "worker_priority.h"

#include <3ds.h>
#include <citro3d.h>

#include <stdio.h>
#include <string.h>

/* The implementation lives in artwork.c; this only needs the API. */
#include "../../include/stb_image.h"

#define THUMB_TEX_SIZE 64
#define THUMB_TEXEL_BYTES (THUMB_TEX_SIZE * THUMB_TEX_SIZE * 4)
/* art_small is a 64 px JPEG of a few KB: generous caps, far below the
 * artwork's. */
#define THUMB_MAX_BYTES (128 * 1024)
#define THUMB_MAX_DIMENSION 640
#define THUMB_URL_SIZE 80

/* Shared with the worker, under `lock`. */
static LightLock lock;
static LightEvent wake;
static thumb_state states[THUMBS_MAX];
static char urls[THUMBS_MAX][THUMB_URL_SIZE];
static int sizes[THUMBS_MAX][2];      /* fitted width and height */
static int count;
static int window_first;
static int generation;
static bool stop_requested;
/* Written by the worker only while the slot is THUMB_LOADING, read by the
 * main thread only once it is THUMB_DECODED. */
static u8 staging[THUMBS_MAX][THUMB_TEXEL_BYTES];

/* Main thread only. */
static Thread worker;
static C3D_Tex textures[THUMBS_MAX];
static Tex3DS_SubTexture subtex[THUMBS_MAX];
static int pool_size;   /* textures made by thumbs_init */

bool thumbs_init(void)
{
	LightLock_Init(&lock);
	LightEvent_Init(&wake, RESET_ONESHOT);
	for (pool_size = 0; pool_size < THUMBS_MAX; pool_size++) {
		if (!C3D_TexInit(&textures[pool_size], THUMB_TEX_SIZE,
		                 THUMB_TEX_SIZE, GPU_RGBA8))
			break;
		C3D_TexSetFilter(&textures[pool_size], GPU_LINEAR, GPU_LINEAR);
	}
	if (pool_size < THUMBS_MAX) {
		/* All or nothing: a partial pool would show some covers only. */
		for (int i = 0; i < pool_size; i++)
			C3D_TexDelete(&textures[i]);
		pool_size = 0;
		return false;
	}
	return true;
}

void thumbs_exit(void)
{
	thumbs_stop();
	for (int i = 0; i < pool_size; i++)
		C3D_TexDelete(&textures[i]);
	pool_size = 0;
}

/* --- Worker ------------------------------------------------------------ */

/* Downloads and decodes one cover into `texels`. Pure CPU and network. */
static bool fetch(http_session **session, const char *url, u8 *texels,
                  int *fit_w, int *fit_h)
{
	const char *path = art_url_path(url);
	if (!path)
		return false;
	if (!*session)
		*session = http_session_open(ART_HOST);
	if (!*session)
		return false;

	http_reply reply = {0};
	int rc = http_session_request(*session, path, "GET", NULL, NULL, NULL,
	                              NULL, 0, THUMB_MAX_BYTES, &reply);
	if (rc != 0) {
		/* The session already retried once; start afresh next time. */
		http_session_close(*session);
		*session = NULL;
	}
	bool ok = false;
	int w = 0, h = 0, channels = 0;
	if (rc == 0 && reply.status == 200 && reply.body && reply.len > 0 &&
	    reply.len <= THUMB_MAX_BYTES &&
	    stbi_info_from_memory((const stbi_uc *)reply.body, (int)reply.len,
	                          &w, &h, &channels) &&
	    w > 0 && h > 0 && w <= THUMB_MAX_DIMENSION &&
	    h <= THUMB_MAX_DIMENSION) {
		stbi_uc *pixels = stbi_load_from_memory(
			(const stbi_uc *)reply.body, (int)reply.len, &w, &h,
			&channels, STBI_rgb_alpha);
		if (pixels) {
			memset(texels, 0, THUMB_TEXEL_BYTES);
			image_fit(w, h, THUMB_TEX_SIZE, fit_w, fit_h);
			image_to_texture(pixels, w, h, texels, THUMB_TEX_SIZE,
			                 *fit_w, *fit_h);
			stbi_image_free(pixels);
			ok = true;
		}
	}
	http_reply_free(&reply);
	return ok;
}

static void worker_main(void *arg)
{
	(void)arg;
	http_session *session = NULL;
	for (;;) {
		LightLock_Lock(&lock);
		int pick = -1;
		while (!stop_requested &&
		       (pick = thumb_pick(states, count, window_first,
		                          RESULTS_PER_PAGE)) < 0) {
			LightLock_Unlock(&lock);
			LightEvent_Wait(&wake);
			LightLock_Lock(&lock);
		}
		if (stop_requested) {
			LightLock_Unlock(&lock);
			break;
		}
		char url[THUMB_URL_SIZE];
		memcpy(url, urls[pick], sizeof(url));
		int gen = generation;
		states[pick] = THUMB_LOADING;
		LightLock_Unlock(&lock);

		int w = 0, h = 0;
		bool ok = fetch(&session, url, staging[pick], &w, &h);

		LightLock_Lock(&lock);
		/* A new list while downloading: this result is stale. */
		if (gen == generation && states[pick] == THUMB_LOADING) {
			states[pick] = ok ? THUMB_DECODED : THUMB_FAILED;
			sizes[pick][0] = w;
			sizes[pick][1] = h;
		}
		LightLock_Unlock(&lock);
	}
	if (session)
		http_session_close(session);
}

/* --- Main thread ------------------------------------------------------- */

static void start_worker(void)
{
	if (worker || pool_size == 0)
		return;
	stop_requested = false;
	worker = threadCreate(worker_main, NULL, 32 * 1024,
	                      background_priority(), -2, false);
}

void thumbs_set_tracks(const sp_track *tracks, int n)
{
	if (pool_size == 0)
		return;
	if (n < 0)
		n = 0;
	if (n > THUMBS_MAX)
		n = THUMBS_MAX;
	bool work = false;
	LightLock_Lock(&lock);
	generation++;
	for (int i = 0; i < THUMBS_MAX; i++) {
		const char *url = i < n ? tracks[i].art_small : "";
		bool same = i < count && strcmp(urls[i], url) == 0;
		if (i < n && thumb_keep(states[i], same))
			continue;
		snprintf(urls[i], sizeof(urls[i]), "%s", url);
		bool fetchable = i < n && art_url_allowed(url) &&
		                 strlen(url) < sizeof(urls[i]);
		states[i] = fetchable ? THUMB_WAITING : THUMB_NONE;
		work |= fetchable;
	}
	count = n;
	window_first = 0;
	LightLock_Unlock(&lock);
	if (work) {
		start_worker();
		LightEvent_Signal(&wake);
	}
}

void thumbs_set_window(int first)
{
	LightLock_Lock(&lock);
	bool changed = window_first != first;
	window_first = first;
	LightLock_Unlock(&lock);
	if (changed)
		LightEvent_Signal(&wake);
}

bool thumbs_poll(void)
{
	if (pool_size == 0)
		return false;
	bool changed = false;
	LightLock_Lock(&lock);
	for (int i = 0; i < count; i++) {
		if (states[i] != THUMB_DECODED)
			continue;
		/* 16 KB: cheap enough to copy under the lock. */
		memcpy(textures[i].data, staging[i], THUMB_TEXEL_BYTES);
		C3D_TexFlush(&textures[i]);
		const float size = THUMB_TEX_SIZE;
		subtex[i].width = (u16)sizes[i][0];
		subtex[i].height = (u16)sizes[i][1];
		subtex[i].left = 0.0f;
		subtex[i].top = 1.0f;
		subtex[i].right = sizes[i][0] / size;
		subtex[i].bottom = 1.0f - sizes[i][1] / size;
		states[i] = THUMB_READY;
		changed = true;
	}
	LightLock_Unlock(&lock);
	return changed;
}

bool thumbs_image(int index, C2D_Image *out)
{
	if (pool_size == 0 || index < 0 || index >= THUMBS_MAX)
		return false;
	LightLock_Lock(&lock);
	bool ready = index < count && states[index] == THUMB_READY;
	LightLock_Unlock(&lock);
	if (ready) {
		out->tex = &textures[index];
		out->subtex = &subtex[index];
	}
	return ready;
}

void thumbs_stop(void)
{
	if (!worker)
		return;
	LightLock_Lock(&lock);
	stop_requested = true;
	/* The fetch in flight is dropped when it ends (its slot is no longer
	 * LOADING) and starts again with the next list. */
	for (int i = 0; i < count; i++)
		if (states[i] == THUMB_LOADING)
			states[i] = THUMB_WAITING;
	LightLock_Unlock(&lock);
	LightEvent_Signal(&wake);
	threadJoin(worker, U64_MAX);
	threadFree(worker);
	worker = NULL;
	stop_requested = false;
}
