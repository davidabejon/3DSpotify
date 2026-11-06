/* Drawing components of the Bisagra system (docs/STYLES.md 5 and 6).
 *
 * Flat and square: opaque colours from theme.h, whole-pixel rectangles drawn
 * with C2D_DrawRectSolid, no gradients (only the screen background has one,
 * in ui_scene), no transparency, no rounded corners. State is a plain flag:
 * every change shows in a single frame. Main thread only. */
#ifndef DRAW_H
#define DRAW_H

#include "layout.h"
#include "theme.h"

#include <citro2d.h>

#include <stdbool.h>

void ui_fill(ui_rect r, u32 color);

/* The raised surface (5.1): hard shadow, 1 px `tinta` outline, flat fill,
 * light band on top and dark band at the bottom. Pressed (6.3), it moves
 * onto its shadow, which is not drawn, and the bands swap. Returns where
 * the surface ended up, for placing its content. */
ui_rect ui_bevel_rect(ui_rect r, u32 fill, u32 light, u32 dark,
                      bool pressed);

/* A non-interactive panel (5.1): `capa` with a 1 px `tinta` outline. */
void ui_panel(ui_rect r);

/* The focus border (6.2): 2 px of `lima`, 1 px outside the outline. */
void ui_focus_ring(ui_rect r);

typedef enum {
	UI_BUTTON_PRIMARY,
	UI_BUTTON_SECONDARY,
	UI_BUTTON_DANGER
} ui_button_kind;

typedef struct {
	bool pressed;
	bool focused;
	bool disabled;
} ui_control_look;

/* A button (5.2) in any of the states of 6.1. The icon (may be NULL or have
 * no texture) and the label are centred as a group, label at TXT_CUERPO. */
void ui_button(ui_rect r, ui_button_kind kind, ui_control_look look,
               const C2D_Text *label, const C2D_Image *icon);
/* Width of a button's content: icon, gap and label. */
float ui_button_content_width(const C2D_Text *label, bool icon);

/* A button glyph of the button bar (5.6), its left edge at `x` and centred
 * on `cy`. `inverted` while the button is held (6.5). Returns its width. */
int ui_glyph(int x, int cy, const C2D_Text *letter, u32 color,
             bool inverted);

/* A raised `capa_alta` bubble with an icon and a message (5.9). */
void ui_bubble(ui_rect r, const C2D_Image *icon, const C2D_Text *text);
float ui_bubble_content_width(const C2D_Text *text);

/* Text whose line is vertically centred on `cy`, at whole pixels. */
void ui_text(const C2D_Text *text, int x, int cy, float scale, u32 color);
/* The same, centred on `cx`. Only for single-message screens (3.3). */
void ui_text_centered(const C2D_Text *text, int cx, int cy, float scale,
                      u32 color);
float ui_text_width(const C2D_Text *text, float scale);

/* A mask icon in `color`, top-left at (x, y), at its own size. */
void ui_icon(const C2D_Image *icon, int x, int y, u32 color);

#endif
