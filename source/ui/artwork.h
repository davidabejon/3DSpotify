/* Album artwork on the top screen, as a GPU texture.
 *
 * A worker thread downloads, decodes and box-filters the image into the
 * texture's tiled layout; artwork_poll, on the main thread, uploads it. All
 * citro3d calls and linear allocations stay on the main thread.
 *
 * Drawing is cheap for the CPU (a gradient rectangle and one textured quad),
 * so there is no "draw it twice and stop" rule any more: the top screen is
 * redrawn in the same GPU frame as the bottom one whenever either changes,
 * and artwork_dirty says when the artwork itself changed. */
#ifndef ARTWORK_H
#define ARTWORK_H

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

#include <stdbool.h>

#include <3ds/thread.h>

/* 256 is enough: the cover is shown at about 212 px. */
#define ARTWORK_TEX_SIZE 256

typedef struct {
	Thread thread;
	volatile bool done;
	char url[128];
	/* Worker output: ARTWORK_TEX_SIZE^2 GPU_RGBA8 pixels, the image in the
	 * top-left worker_width x worker_height corner. */
	u8 *worker_texels;
	int worker_width;
	int worker_height;
	/* Main thread only. */
	C3D_Tex tex;
	Tex3DS_SubTexture subtex;
	bool has_texture;
	bool dirty;
} artwork_state;

/* Drops the current image and starts loading `url` (i.scdn.co only). */
void artwork_request(artwork_state *artwork, const char *url);
bool artwork_pending(const artwork_state *artwork);
/* Uploads a finished download. Call once per frame on the main thread. */
void artwork_poll(artwork_state *artwork);
bool artwork_dirty(const artwork_state *artwork);
/* Draws into the current scene (the top screen): the background, and the
 * cover when there is one. */
void artwork_draw(artwork_state *artwork);
/* Forces a repaint, for when something else (an applet such as the software
 * keyboard) may have drawn over the top screen. */
void artwork_invalidate(artwork_state *artwork);
/* Joins the worker and frees the texture. Call before ui_exit. */
void artwork_close(artwork_state *artwork);

#endif
