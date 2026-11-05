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
} artwork_state;

void artwork_request(artwork_state *artwork, const char *url);
bool artwork_pending(const artwork_state *artwork);
void artwork_poll(artwork_state *artwork);
void artwork_render(const artwork_state *artwork);
void artwork_close(artwork_state *artwork);
void drawImageToScreen(u8 *pixels, int width, int height);
void drawBackgroundToScreen(void);

#endif
