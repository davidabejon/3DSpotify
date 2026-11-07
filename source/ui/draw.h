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
/* A square button with only an icon, centred: the player's controls. */
void ui_icon_button(ui_rect r, ui_button_kind kind, ui_control_look look,
                    const C2D_Image *icon);
/* Width of a button's content: icon, gap and label. */
float ui_button_content_width(const C2D_Text *label, bool icon);

/* A button glyph of the button bar (5.6), its left edge at `x` and centred
 * on `cy`. `inverted` while the button is held (6.5). Returns its width. */
int ui_glyph(int x, int cy, const C2D_Text *letter, u32 color,
             bool inverted);
/* The width ui_glyph() will take for `letter`. */
int ui_glyph_width(const C2D_Text *letter);

/* One action of the button bar: the button's glyph and what it does. */
typedef struct {
	const C2D_Text *letter;
	const C2D_Text *label;
	u32 color;      /* the button's colour (2.5) */
	bool inverted;  /* the button is held (6.5) */
} ui_hint;

/* The button bar (5.6): a `capa` panel with up to four actions, left to
 * right from the margin. An action whose label is NULL shows its glyph
 * alone, for a bar that would not fit otherwise. */
void ui_button_bar(ui_rect bar, const ui_hint *hints, int count);

/* A progress (5.4) or volume (5.5) bar in `r`: 1 px `tinta` outline,
 * `capa_alta` track, the download in `musgo` ahead of the progress in
 * `clorofila`, both in thousandths (download < 0: none). With `handle`, a
 * HANDLE_W x HANDLE_H `texto` handle outlined in `tinta`, centred on the
 * end of the progress. */
void ui_progress(ui_rect r, int download, int progress, bool handle);

/* The loading indicator (5.7), centred on (cx, cy): UI_LOADER_BARS bars,
 * UI_LOADER_W px wide in all. `step` advances every LOADER_STEP frames. */
#define UI_LOADER_BARS 3
#define UI_LOADER_W (UI_LOADER_BARS * LOADER_BAR_W + (UI_LOADER_BARS - 1) * SP_1)
void ui_loader(int cx, int cy, int step);

/* The download spinner (spinner_logic.h), centred on (cx, cy): a ring of
 * square blocks whose lit block moves clockwise, on a translucent grey
 * square; `step` advances every LOADER_STEP frames. */
void ui_spinner(int cx, int cy, int step);
/* The ring alone, for a flat background with nothing to veil: the login
 * screens (feature_plan.md, decision 1). */
void ui_spinner_ring(int cx, int cy, int step);

/* A notice (5.8): raised `coral` (error) or `ambar` (warning) surface with
 * an icon, a title and an explanation that wraps inside it, all `tinta`. */
void ui_notice(ui_rect r, bool error, const C2D_Image *icon,
               const C2D_Text *title, const C2D_Text *body);

/* A cover in `box` (5.3, 7.5): 1 px `tinta` outline, the image fitted
 * inside and centred; without `image` (NULL or no texture), a `capa_alta`
 * square with the note icon centred in `texto_2`, so the layout does not
 * move when the image arrives. */
void ui_art(ui_rect box, const C2D_Image *image);

/* A raised `capa_alta` bubble with an icon and a message (5.9). */
void ui_bubble(ui_rect r, const C2D_Image *icon, const C2D_Text *text);
float ui_bubble_content_width(const C2D_Text *text);
/* The same bubble with the loading indicator (5.7) where the icon goes:
 * the floating "Searching..." over the top screen. */
void ui_bubble_loading(ui_rect r, int step, const C2D_Text *text);
float ui_bubble_loading_content_width(const C2D_Text *text);

/* Text whose line is vertically centred on `cy`, at whole pixels. */
void ui_text(const C2D_Text *text, int x, int cy, float scale, u32 color);
/* The same, centred on `cx`. Only for single-message screens (3.3). */
void ui_text_centered(const C2D_Text *text, int cx, int cy, float scale,
                      u32 color);
float ui_text_width(const C2D_Text *text, float scale);
/* Text wrapped at word boundaries to `width`, its first line's top at
 * `y`. */
void ui_text_wrapped(const C2D_Text *text, int x, int y, float scale,
                     u32 color, int width);
/* The same, every line centred on `cx`. Single-message screens (3.3). */
void ui_text_wrapped_centered(const C2D_Text *text, int cx, int y,
                              float scale, u32 color, int width);
/* Height of one line of the system font at `scale`, in whole pixels. */
int ui_line_height(float scale);

/* Width of `string` at `scale`, parsed into `scratch` (which it clears). */
float ui_measure(C2D_TextBuf scratch, const char *string, float scale);

/* The longest a fitted string gets: the source plus "...". */
#define UI_FIELD_SIZE 136

/* Parses `string` into `buf`, cut with "..." to `width` px at `scale`
 * (3.3); `scratch` (UI_FIELD_SIZE glyphs) is used for measuring. */
void ui_parse_fitted(C2D_Text *text, C2D_TextBuf buf, C2D_TextBuf scratch,
                     const char *string, float scale, int width);

/* A mask icon in `color`, top-left at (x, y), at its own size. */
void ui_icon(const C2D_Image *icon, int x, int y, u32 color);

#endif
