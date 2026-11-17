#include "artwork.h"

#include "art_cache.h"
#include "art_cache_logic.h"
#include "art_url.h"
#include "dl_store.h"
#include "draw.h"
#include "http3ds.h"
#include "icons.h"
#include "image_util.h"
#include "thumbs.h"
#include "ui.h"
#include "worker_priority.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARTWORK_MAX_BYTES (1024 * 1024)
#define ARTWORK_MAX_DIMENSION 1024
#define STBI_MAX_DIMENSIONS ARTWORK_MAX_DIMENSION
#define STB_IMAGE_IMPLEMENTATION
#include "../../include/stb_image.h"

#define TEXEL_BYTES ((size_t)ARTWORK_TEX_SIZE * ARTWORK_TEX_SIZE * 4)

/* Decodes a cover's bytes into the worker's output; false when they are
 * not an image it can show. */
static bool decode(artwork_state *artwork, const void *body, size_t len)
{
	if (!body || len == 0 || len > ARTWORK_MAX_BYTES)
		return false;
	int width = 0;
	int height = 0;
	int channels = 0;
	stbi_uc *pixels = stbi_load_from_memory((const stbi_uc *)body, (int)len,
	                                        &width, &height, &channels,
	                                        STBI_rgb_alpha);
	bool ok = false;
	if (pixels && width > 0 && height > 0 &&
	    width <= ARTWORK_MAX_DIMENSION && height <= ARTWORK_MAX_DIMENSION) {
		u8 *texels = calloc(1, TEXEL_BYTES);
		if (texels) {
			int fit_width = 0;
			int fit_height = 0;
			image_fit(width, height, ARTWORK_TEX_SIZE, &fit_width,
			          &fit_height);
			image_to_texture(pixels, width, height, texels,
			                 ARTWORK_TEX_SIZE, fit_width, fit_height);
			artwork->worker_texels = texels;
			artwork->worker_width = fit_width;
			artwork->worker_height = fit_height;
			ok = true;
		}
	}
	if (pixels)
		stbi_image_free(pixels);
	return ok;
}

/* Pure CPU work only: no citro3d and no linearAlloc off the main thread.
 * The cover cache first (art_cache.h): the thumbnails have often fetched
 * the same file already. */
static void artwork_worker(void *arg)
{
	artwork_state *artwork = arg;
	size_t cached_len = 0;
	void *cached = art_cache_get(artwork->url, &cached_len);
	bool ok = cached && decode(artwork, cached, cached_len);
	free(cached);
	/* Then a download's cover, kept on the card: it shows without the
	 * network. */
	if (!ok) {
		void *kept = dl_store_art_read(artwork->url, &cached_len);
		ok = kept && decode(artwork, kept, cached_len);
		if (ok)
			art_cache_put(artwork->url, kept, cached_len);
		free(kept);
	}
	if (!ok) {
		http_reply reply = {0};
		int rc = http_get_url(artwork->url, ARTWORK_MAX_BYTES, &reply);
		if (rc == 0 && reply.status == 200 &&
		    decode(artwork, reply.body, reply.len))
			art_cache_put(artwork->url, reply.body, reply.len);
		http_reply_free(&reply);
	}
	__sync_synchronize();
	artwork->done = true;
}

/* Joins a worker that has returned (free then) and drops its result. */
static void drop_finished_worker(artwork_state *artwork)
{
	__sync_synchronize();
	threadJoin(artwork->thread, U64_MAX);
	threadFree(artwork->thread);
	artwork->thread = NULL;
	free(artwork->worker_texels);
	artwork->worker_texels = NULL;
	artwork->worker_width = artwork->worker_height = 0;
	artwork->done = false;
}

static void start_fetch(artwork_state *artwork, const char *url)
{
	if (!art_url_allowed(url) || strlen(url) >= sizeof(artwork->url))
		return;
	snprintf(artwork->url, sizeof(artwork->url), "%s", url);
	artwork->done = false;
	artwork->thread = threadCreate(artwork_worker, artwork, 32 * 1024,
	                               background_priority(), -2, false);
	if (!artwork->thread)
		artwork->url[0] = '\0';
}

/* --- Covers kept decoded ------------------------------------------------
 *
 * The last ARTWORK_KEEP covers shown, as textures, whichever artwork
 * showed them: going back to one shows it at once, without decoding it
 * again. A texture belongs either to an artwork_state or to this list,
 * and moves between them. Main thread only. */
typedef struct {
	char url[sizeof(((artwork_state *)0)->shown_url)];
	C3D_Tex tex;
	Tex3DS_SubTexture subtex;
} kept_cover;

static kept_cover kept[ARTWORK_KEEP];
static bool kept_used[ARTWORK_KEEP];
static unsigned kept_stamps[ARTWORK_KEEP];
static unsigned kept_clock;

static void kept_drop(int i)
{
	C3D_TexDelete(&kept[i].tex);
	kept_used[i] = false;
	kept[i].url[0] = '\0';
}

/* Frees the least recently kept cover; false with none left. */
static bool kept_drop_oldest(void)
{
	int i = art_cache_oldest(kept_used, kept_stamps, ARTWORK_KEEP);
	if (i < 0)
		return false;
	kept_drop(i);
	return true;
}

/* The artwork's cover goes to the list instead of being deleted. */
static void keep_texture(artwork_state *artwork)
{
	if (!artwork->has_texture)
		return;
	artwork->has_texture = false;
	if (!artwork->shown_url[0]) {
		C3D_TexDelete(&artwork->tex);
		return;
	}
	for (int i = 0; i < ARTWORK_KEEP; i++)
		if (kept_used[i] && strcmp(kept[i].url, artwork->shown_url) == 0)
			kept_drop(i);
	int slot = art_cache_victim(kept_used, kept_stamps, ARTWORK_KEEP);
	if (kept_used[slot])
		kept_drop(slot);
	memcpy(kept[slot].url, artwork->shown_url, sizeof(kept[slot].url));
	kept[slot].tex = artwork->tex;
	kept[slot].subtex = artwork->subtex;
	kept_used[slot] = true;
	kept_stamps[slot] = ++kept_clock;
	artwork->shown_url[0] = '\0';
}

/* Takes `url`'s cover back from the list; false when it is not there. */
static bool take_kept(artwork_state *artwork, const char *url)
{
	for (int i = 0; i < ARTWORK_KEEP; i++) {
		if (!kept_used[i] || strcmp(kept[i].url, url) != 0)
			continue;
		artwork->tex = kept[i].tex;
		artwork->subtex = kept[i].subtex;
		artwork->has_texture = true;
		snprintf(artwork->shown_url, sizeof(artwork->shown_url), "%s", url);
		kept_used[i] = false;
		kept[i].url[0] = '\0';
		return true;
	}
	return false;
}

void artwork_forget_kept(void)
{
	for (int i = 0; i < ARTWORK_KEEP; i++)
		if (kept_used[i])
			kept_drop(i);
}

void artwork_request(artwork_state *artwork, const char *url)
{
	if (!url)
		url = "";
	snprintf(artwork->want_url, sizeof(artwork->want_url), "%s", url);
	artwork->preview_url[0] = '\0';
	if (artwork->has_texture && url[0] &&
	    strcmp(artwork->shown_url, url) == 0) {
		/* Already showing it: only a download for another cover, if
		 * any, is no longer wanted. */
		if (artwork->thread && !artwork->done) {
			artwork->next_url[0] = '\0';
			artwork->has_next = true;
		}
		return;
	}
	/* The old cover goes now, whatever the worker is doing, into the
	 * list of covers kept decoded. */
	keep_texture(artwork);
	artwork->dirty = true;
	artwork->has_next = false;
	if (url[0] && take_kept(artwork, url)) {
		/* Shown at once. A download still running for another cover
		 * must not replace it: its result is dropped. */
		if (artwork->thread && !artwork->done) {
			artwork->next_url[0] = '\0';
			artwork->has_next = true;
		} else if (artwork->thread) {
			drop_finished_worker(artwork);
		}
		return;
	}
	if (artwork->thread && !artwork->done) {
		/* Joining would block the main thread, and the input with it,
		 * for the whole download: the request waits its turn. */
		snprintf(artwork->next_url, sizeof(artwork->next_url), "%s", url);
		artwork->has_next = true;
		return;
	}
	if (artwork->thread)
		drop_finished_worker(artwork);
	start_fetch(artwork, url);
}

bool artwork_pending(const artwork_state *artwork)
{
	return (artwork->thread && !artwork->done) || artwork->has_next;
}

static void upload(artwork_state *artwork)
{
	if (!artwork->worker_texels)
		return;
	/* Short of linear memory, the covers kept give theirs back first. */
	while (!C3D_TexInit(&artwork->tex, ARTWORK_TEX_SIZE, ARTWORK_TEX_SIZE,
	                    GPU_RGBA8))
		if (!kept_drop_oldest())
			return; /* out of linear memory: keep the plain background */
	memcpy(artwork->tex.data, artwork->worker_texels, TEXEL_BYTES);
	C3D_TexFlush(&artwork->tex);
	C3D_TexSetFilter(&artwork->tex, GPU_LINEAR, GPU_LINEAR);
	const float size = ARTWORK_TEX_SIZE;
	artwork->subtex.width = (u16)artwork->worker_width;
	artwork->subtex.height = (u16)artwork->worker_height;
	artwork->subtex.left = 0.0f;
	artwork->subtex.top = 1.0f;
	artwork->subtex.right = artwork->worker_width / size;
	artwork->subtex.bottom = 1.0f - artwork->worker_height / size;
	artwork->has_texture = true;
}

void artwork_poll(artwork_state *artwork)
{
	if (!artwork->thread || !artwork->done)
		return;
	if (artwork->has_next) {
		/* A newer request was waiting for this download to end. */
		drop_finished_worker(artwork);
		artwork->has_next = false;
		start_fetch(artwork, artwork->next_url);
		return;
	}

	__sync_synchronize();
	threadJoin(artwork->thread, U64_MAX);
	threadFree(artwork->thread);
	artwork->thread = NULL;
	keep_texture(artwork);
	upload(artwork);
	if (artwork->has_texture)
		snprintf(artwork->shown_url, sizeof(artwork->shown_url), "%s",
		         artwork->url);
	free(artwork->worker_texels);
	artwork->worker_texels = NULL;
	artwork->worker_width = 0;
	artwork->worker_height = 0;
	artwork->url[0] = '\0';
	artwork->done = false;
	artwork->dirty = true;
}

void artwork_set_preview(artwork_state *artwork, const char *url)
{
	snprintf(artwork->preview_url, sizeof(artwork->preview_url), "%s",
	         url ? url : "");
	artwork->dirty = true;
}

/* Without the cover, its thumbnail when the pool has it (thumbs_find):
 * the cover's own URL's, else the smaller copy's. */
static bool preview(const artwork_state *artwork, C2D_Image *out)
{
	return !artwork->has_texture &&
	       (thumbs_find(artwork->want_url, out) ||
	        thumbs_find(artwork->preview_url, out));
}

bool artwork_dirty(const artwork_state *artwork)
{
	if (artwork->dirty)
		return true;
	C2D_Image image;
	return preview(artwork, &image) != artwork->drawn_preview;
}

void artwork_draw_in(artwork_state *artwork, ui_rect box, int shadow)
{
	artwork_draw_or(artwork, box, shadow, ICON_NOTE);
}

void artwork_draw_or(artwork_state *artwork, ui_rect box, int shadow,
                     int icon)
{
	artwork->dirty = false;
	if (shadow > 0)
		ui_fill(ui_rect_offset(box, shadow, shadow), COL_SOMBRA);
	C2D_Image image = { &artwork->tex, &artwork->subtex };
	/* Until the cover comes, its thumbnail scaled up. */
	artwork->drawn_preview = preview(artwork, &image);
	ui_art_or(box, artwork->has_texture || artwork->drawn_preview ? &image
	                                                              : NULL,
	          icon);
}

void artwork_invalidate(artwork_state *artwork)
{
	artwork->dirty = true;
}

void artwork_close(artwork_state *artwork)
{
	if (artwork->thread) {
		threadJoin(artwork->thread, U64_MAX);
		threadFree(artwork->thread);
	}
	free(artwork->worker_texels);
	if (artwork->has_texture)
		C3D_TexDelete(&artwork->tex);
	memset(artwork, 0, sizeof(*artwork));
}
