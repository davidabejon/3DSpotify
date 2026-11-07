/* The renderer: citro3d/citro2d own both screens.
 *
 * Nothing else may touch the framebuffers (no consoleInit, no
 * gfxGetFramebuffer). Every call here, and every C3D/C2D call anywhere, has
 * to happen on the main thread. */
#ifndef UI_H
#define UI_H

#include <3ds.h>
#include <citro2d.h>

#include <stdbool.h>

/* Colours, sizes and timings: theme.h (docs/STYLES.md). */
#include "theme.h"

/* After gfxInitDefault. Returns false when there is not enough linear
 * memory for the command buffer and render targets. */
bool ui_init(void);
void ui_exit(void);

/* One GPU frame: ui_begin, then ui_scene for each screen to draw (it fills
 * the screen with the common background, the vertical gradient
 * `fondo_sup` -> `fondo_inf` of STYLES.md 7.1), then ui_end, which transfers
 * and swaps. ui_begin waits for the previous frame, so it also paces the
 * loop. */
void ui_begin(void);
void ui_scene(gfxScreen_t screen);
void ui_end(void);

/* True once after the app regains the screens (HOME menu, lid, applets) and
 * on the first call: everything has to be redrawn then. */
bool ui_take_restored(void);

#endif
