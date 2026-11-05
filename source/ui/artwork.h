#ifndef ARTWORK_H
#define ARTWORK_H

#include <3ds.h>

#include <stdbool.h>

#include <3ds/thread.h>

typedef struct {
	Thread thread;
	volatile bool done;
	char url[128];
	u8 *worker_pixels;
	int worker_width;
	int worker_height;
	u8 *pixels;
	int width;
	int height;
	/* Frames painted since the content last changed. The top screen is
	 * double-buffered, so each image has to be drawn into both buffers and
	 * then never again: redrawing every frame costs the audio decoder, which
	 * runs on the same thread, enough time to make playback crackle. */
	int frames_drawn;
} artwork_state;

void artwork_request(artwork_state *artwork, const char *url);
bool artwork_pending(const artwork_state *artwork);
void artwork_poll(artwork_state *artwork);
void artwork_render(artwork_state *artwork);
/* Forces a repaint, for when something else (an applet such as the software
 * keyboard) may have drawn over the top screen. */
void artwork_invalidate(artwork_state *artwork);
void artwork_close(artwork_state *artwork);
void drawImageToScreen(u8 *pixels, int width, int height);
void drawBackgroundToScreen(void);

#endif
