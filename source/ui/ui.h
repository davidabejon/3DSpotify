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

/* Theme tokens (feature_plan.md, section 2.1). */
#define UI_COLOR_BG            C2D_Color32(0x10, 0x14, 0x18, 0xFF)
#define UI_COLOR_SURFACE       C2D_Color32(0x1B, 0x21, 0x28, 0xFF)
#define UI_COLOR_SURFACE_FOCUS C2D_Color32(0x26, 0x32, 0x3C, 0xFF)
#define UI_COLOR_TEXT          C2D_Color32(0xF2, 0xF4, 0xF5, 0xFF)
#define UI_COLOR_TEXT_DIM      C2D_Color32(0x9A, 0xA5, 0xB1, 0xFF)
#define UI_COLOR_ACCENT        C2D_Color32(0x4F, 0xD1, 0xC5, 0xFF)
#define UI_COLOR_DANGER        C2D_Color32(0xF0, 0x71, 0x78, 0xFF)

/* After gfxInitDefault. Returns false when there is not enough linear
 * memory for the command buffer and render targets. */
bool ui_init(void);
void ui_exit(void);

/* One GPU frame: ui_begin, then ui_scene for each screen to draw (it clears
 * the screen to the background colour), then ui_end, which transfers and
 * swaps. ui_begin waits for the previous frame, so it also paces the loop. */
void ui_begin(void);
void ui_scene(gfxScreen_t screen);
void ui_end(void);

/* True once after the app regains the screens (HOME menu, lid, applets) and
 * on the first call: everything has to be redrawn then. */
bool ui_take_restored(void);

#endif
