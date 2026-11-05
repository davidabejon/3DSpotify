#include "artwork.h"

#include "http3ds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARTWORK_MAX_BYTES (1024 * 1024)
#define ARTWORK_MAX_DIMENSION 1024
#define STBI_MAX_DIMENSIONS ARTWORK_MAX_DIMENSION
#define STB_IMAGE_IMPLEMENTATION
#include "../../include/stb_image.h"

static bool artwork_url_allowed(const char *url)
{
	static const char prefix[] = "https://i.scdn.co/";
	const size_t prefix_len = sizeof(prefix) - 1;
	if (!url || strncmp(url, prefix, prefix_len) != 0 ||
	    url[prefix_len] == '\0')
		return false;

	for (const unsigned char *p = (const unsigned char *)url + prefix_len;
	     *p; p++)
		if (*p <= 0x20 || *p > 0x7e || *p == '\\' || *p == '#')
			return false;
	return true;
}

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
		    height <= ARTWORK_MAX_DIMENSION &&
		    (size_t)width * (size_t)height <=
			    (size_t)ARTWORK_MAX_DIMENSION * ARTWORK_MAX_DIMENSION) {
			artwork->worker_pixels = pixels;
			artwork->worker_width = width;
			artwork->worker_height = height;
		} else if (pixels) {
			stbi_image_free(pixels);
		}
	}
	http_reply_free(&reply);
	__sync_synchronize();
	artwork->done = true;
}

void artwork_request(artwork_state *artwork, const char *url)
{
	artwork_close(artwork);
	if (!artwork_url_allowed(url) || strlen(url) >= sizeof(artwork->url))
		return;

	snprintf(artwork->url, sizeof(artwork->url), "%s", url);
	artwork->thread = threadCreate(artwork_worker, artwork, 32 * 1024,
	                               0x18, -2, false);
	if (!artwork->thread)
		artwork->url[0] = '\0';
}

bool artwork_pending(const artwork_state *artwork)
{
	return artwork->thread && !artwork->done;
}

void artwork_poll(artwork_state *artwork)
{
	if (!artwork->thread || !artwork->done)
		return;

	__sync_synchronize();
	threadJoin(artwork->thread, U64_MAX);
	threadFree(artwork->thread);
	artwork->thread = NULL;
	free(artwork->pixels);
	artwork->pixels = artwork->worker_pixels;
	artwork->width = artwork->worker_width;
	artwork->height = artwork->worker_height;
	artwork->worker_pixels = NULL;
	artwork->worker_width = 0;
	artwork->worker_height = 0;
	artwork->url[0] = '\0';
	artwork->done = false;
}

static void put_pixel(u8 *fb, u16 fb_width, u16 fb_height,
                      int x, int y, u8 r, u8 g, u8 b)
{
	int fb_x = 239 - y;
	int fb_y = x;
	if (fb_x < 0 || fb_x >= fb_width || fb_y < 0 || fb_y >= fb_height)
		return;
	size_t offset = ((size_t)fb_x + (size_t)fb_y * fb_width) * 3;
	fb[offset] = b;
	fb[offset + 1] = g;
	fb[offset + 2] = r;
}

void drawBackgroundToScreen(void)
{
	u16 fb_width = 0;
	u16 fb_height = 0;
	u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fb_width, &fb_height);
	if (!fb)
		return;

	for (int y = 0; y < 240; y++) {
		float t = (float)y / 239.0f;
		u8 b = (u8)(30.0f + t * 40.0f);
		u8 g = (u8)(80.0f + t * 120.0f);
		u8 r = (u8)(40.0f + t * 15.0f);
		for (int x = 0; x < 400; x++)
			put_pixel(fb, fb_width, fb_height, x, y, r, g, b);
	}
}

void drawImageToScreen(u8 *pixels, int width, int height)
{
	if (!pixels || width <= 0 || height <= 0)
		return;

	drawBackgroundToScreen();
	u16 fb_width = 0;
	u16 fb_height = 0;
	u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fb_width, &fb_height);
	if (!fb)
		return;

	const int max_width = 372;
	const int max_height = 212;
	float scale_x = (float)max_width / width;
	float scale_y = (float)max_height / height;
	float scale = scale_x < scale_y ? scale_x : scale_y;
	int scaled_width = (int)(width * scale);
	int scaled_height = (int)(height * scale);
	int start_x = (400 - scaled_width) / 2;
	int start_y = (240 - scaled_height) / 2;

	for (int y = start_y - 4; y < start_y + scaled_height + 4; y++)
		for (int x = start_x - 4; x < start_x + scaled_width + 4; x++)
			if (x >= 0 && x < 400 && y >= 0 && y < 240)
				put_pixel(fb, fb_width, fb_height, x, y, 255, 255, 255);

	for (int y = 0; y < scaled_height; y++) {
		int source_y = (int)(y / scale);
		if (source_y >= height)
			source_y = height - 1;
		for (int x = 0; x < scaled_width; x++) {
			int source_x = (int)(x / scale);
			if (source_x >= width)
				source_x = width - 1;
			const u8 *source =
				pixels + ((size_t)source_y * width + source_x) * 4;
			put_pixel(fb, fb_width, fb_height, start_x + x, start_y + y,
			          source[0], source[1], source[2]);
		}
	}
}

void artwork_render(const artwork_state *artwork)
{
	if (artwork->pixels)
		drawImageToScreen(artwork->pixels, artwork->width, artwork->height);
	else
		drawBackgroundToScreen();
}

void artwork_close(artwork_state *artwork)
{
	if (artwork->thread) {
		threadJoin(artwork->thread, U64_MAX);
		threadFree(artwork->thread);
	}
	free(artwork->worker_pixels);
	free(artwork->pixels);
	memset(artwork, 0, sizeof(*artwork));
}
