#include "artwork.h"

#include "art_url.h"
#include "draw.h"
#include "http3ds.h"
#include "image_util.h"
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

#define TOP_WIDTH 400.0f
#define TOP_HEIGHT 240.0f
/* The cover's box on the top screen, and the white frame around it. */
#define ART_MAX_WIDTH 372.0f
#define ART_MAX_HEIGHT 212.0f
#define ART_BORDER 4.0f

#define TEXEL_BYTES ((size_t)ARTWORK_TEX_SIZE * ARTWORK_TEX_SIZE * 4)

/* Pure CPU work only: no citro3d and no linearAlloc off the main thread. */
static void artwork_worker(void *arg)
{
	artwork_state *artwork = arg;
	http_reply reply = {0};
	int rc = http_get_url(artwork->url, ARTWORK_MAX_BYTES, &reply);
	if (rc == 0 && reply.status == 200 && reply.body &&
	    reply.len > 0 && reply.len <= ARTWORK_MAX_BYTES) {
		int width = 0;
		int height = 0;
		int channels = 0;
		stbi_uc *pixels = stbi_load_from_memory(
			(const stbi_uc *)reply.body, (int)reply.len,
			&width, &height, &channels, STBI_rgb_alpha);
		if (pixels && width > 0 && height > 0 &&
		    width <= ARTWORK_MAX_DIMENSION &&
		    height <= ARTWORK_MAX_DIMENSION) {
			u8 *texels = calloc(1, TEXEL_BYTES);
			if (texels) {
				int fit_width = 0;
				int fit_height = 0;
				image_fit(width, height, ARTWORK_TEX_SIZE,
				          &fit_width, &fit_height);
				image_to_texture(pixels, width, height, texels,
				                 ARTWORK_TEX_SIZE, fit_width, fit_height);
				artwork->worker_texels = texels;
				artwork->worker_width = fit_width;
				artwork->worker_height = fit_height;
			}
		}
		if (pixels)
			stbi_image_free(pixels);
	}
	http_reply_free(&reply);
	__sync_synchronize();
	artwork->done = true;
}

void artwork_request(artwork_state *artwork, const char *url)
{
	artwork_close(artwork);
	artwork->dirty = true;
	if (!art_url_allowed(url) || strlen(url) >= sizeof(artwork->url))
		return;

	snprintf(artwork->url, sizeof(artwork->url), "%s", url);
	artwork->thread = threadCreate(artwork_worker, artwork, 32 * 1024,
	                               background_priority(), -2, false);
	if (!artwork->thread)
		artwork->url[0] = '\0';
}

bool artwork_pending(const artwork_state *artwork)
{
	return artwork->thread && !artwork->done;
}

static void upload(artwork_state *artwork)
{
	if (!artwork->worker_texels)
		return;
	if (!C3D_TexInit(&artwork->tex, ARTWORK_TEX_SIZE, ARTWORK_TEX_SIZE,
	                 GPU_RGBA8))
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

	__sync_synchronize();
	threadJoin(artwork->thread, U64_MAX);
	threadFree(artwork->thread);
	artwork->thread = NULL;
	if (artwork->has_texture) {
		C3D_TexDelete(&artwork->tex);
		artwork->has_texture = false;
	}
	upload(artwork);
	free(artwork->worker_texels);
	artwork->worker_texels = NULL;
	artwork->worker_width = 0;
	artwork->worker_height = 0;
	artwork->url[0] = '\0';
	artwork->done = false;
	artwork->dirty = true;
}

bool artwork_dirty(const artwork_state *artwork)
{
	return artwork->dirty;
}

void artwork_draw(artwork_state *artwork)
{
	artwork->dirty = false;
	if (!artwork->has_texture)
		return;

	float width = artwork->subtex.width;
	float height = artwork->subtex.height;
	float scale_x = ART_MAX_WIDTH / width;
	float scale_y = ART_MAX_HEIGHT / height;
	float scale = scale_x < scale_y ? scale_x : scale_y;
	float shown_width = width * scale;
	float shown_height = height * scale;
	float x = (TOP_WIDTH - shown_width) / 2.0f;
	float y = (TOP_HEIGHT - shown_height) / 2.0f;
	C2D_DrawRectSolid(x - ART_BORDER, y - ART_BORDER, 0,
	                  shown_width + 2 * ART_BORDER,
	                  shown_height + 2 * ART_BORDER,
	                  COL_TEXTO);
	C2D_Image image = { &artwork->tex, &artwork->subtex };
	C2D_DrawImageAt(image, x, y, 0, NULL, scale, scale);
}

void artwork_draw_in(artwork_state *artwork, ui_rect box, int shadow)
{
	artwork->dirty = false;
	if (shadow > 0)
		ui_fill(ui_rect_offset(box, shadow, shadow), COL_SOMBRA);
	C2D_Image image = { &artwork->tex, &artwork->subtex };
	ui_art(box, artwork->has_texture ? &image : NULL);
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
