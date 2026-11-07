/* The app logo, read from romfs:/gfx/logo.png and drawn 1:1.
 *
 * The PNG must be at most LOGO_MAX_W x LOGO_MAX_H: the code never rescales
 * it, so it stays sharp (STYLES.md 4.3), and a larger file is rejected.
 * Without the texture (no romfs, a missing or invalid file, or no linear
 * memory), the app name is drawn as text instead.
 *
 * Main thread only: logo_load after ui_init and romfsInit, logo_free before
 * ui_exit. */
#ifndef LOGO_H
#define LOGO_H

#include <stdbool.h>

#define LOGO_PATH "romfs:/gfx/logo.png"
#define LOGO_MAX_W 256
#define LOGO_MAX_H 128

/* True when the texture is ready; false keeps the text fallback. */
bool logo_load(void);
void logo_free(void);

/* Centred on (cx, cy) at scale 1 and on whole pixels. */
void logo_draw_centered(int cx, int cy);

#endif
