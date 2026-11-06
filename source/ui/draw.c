#include "draw.h"

#include "icons.h"
#include "player_logic.h"
#include "text_fit.h"

#include <math.h>
#include <stddef.h>

void ui_fill(ui_rect r, u32 color)
{
	if (r.w > 0 && r.h > 0)
		C2D_DrawRectSolid(r.x, r.y, 0.0f, r.w, r.h, color);
}

/* A 1 px `tinta` outline filled with `fill`. Returns the inside. */
static ui_rect outlined(ui_rect r, u32 fill)
{
	ui_fill(r, COL_TINTA);
	ui_rect inside = ui_rect_inset(r, OUTLINE_W);
	ui_fill(inside, fill);
	return inside;
}

ui_rect ui_bevel_rect(ui_rect r, u32 fill, u32 light, u32 dark,
                      bool pressed)
{
	if (pressed)
		r = ui_rect_offset(r, PRESS_SHIFT, PRESS_SHIFT);
	else
		ui_fill(ui_rect_offset(r, SHADOW_OFF, SHADOW_OFF), COL_SOMBRA);
	ui_rect inside = outlined(r, fill);
	ui_rect top = { inside.x, inside.y, inside.w, BEVEL_W };
	ui_rect bottom = { inside.x, inside.y + inside.h - BEVEL_W, inside.w,
	                   BEVEL_W };
	ui_fill(top, pressed ? dark : light);
	ui_fill(bottom, pressed ? light : dark);
	return r;
}

void ui_panel(ui_rect r)
{
	outlined(r, COL_CAPA);
}

void ui_focus_ring(ui_rect r)
{
	ui_rect o = ui_rect_inset(r, -(FOCUS_GAP + FOCUS_W));
	ui_rect top = { o.x, o.y, o.w, FOCUS_W };
	ui_rect bottom = { o.x, o.y + o.h - FOCUS_W, o.w, FOCUS_W };
	ui_rect left = { o.x, o.y + FOCUS_W, FOCUS_W, o.h - 2 * FOCUS_W };
	ui_rect right = { o.x + o.w - FOCUS_W, o.y + FOCUS_W, FOCUS_W,
	                  o.h - 2 * FOCUS_W };
	ui_fill(top, COL_LIMA);
	ui_fill(bottom, COL_LIMA);
	ui_fill(left, COL_LIMA);
	ui_fill(right, COL_LIMA);
}

int ui_line_height(float scale)
{
	const FINF_s *info = C2D_FontGetInfo(NULL);
	float feed = info && info->lineFeed ? info->lineFeed : 30.0f;
	return (int)lroundf(feed * scale);
}

void ui_text_wrapped(const C2D_Text *text, int x, int y, float scale,
                     u32 color, int width)
{
	C2D_DrawText(text, C2D_WithColor | C2D_WordWrap, x, y, 0.0f, scale,
	             scale, color, (float)width);
}

float ui_text_width(const C2D_Text *text, float scale)
{
	float width = 0;
	C2D_TextGetDimensions(text, scale, scale, &width, NULL);
	return width;
}

void ui_text(const C2D_Text *text, int x, int cy, float scale, u32 color)
{
	int y = cy - ui_line_height(scale) / 2;
	C2D_DrawText(text, C2D_WithColor, x, y, 0.0f, scale, scale, color);
}

void ui_text_centered(const C2D_Text *text, int cx, int cy, float scale,
                      u32 color)
{
	int x = cx - (int)lroundf(ui_text_width(text, scale) / 2);
	ui_text(text, x, cy, scale, color);
}

void ui_icon(const C2D_Image *icon, int x, int y, u32 color)
{
	if (!icon || !icon->tex)
		return;
	C2D_ImageTint tint;
	C2D_PlainImageTint(&tint, color, 1.0f);
	C2D_DrawImageAt(*icon, x, y, 0.0f, &tint, 1.0f, 1.0f);
}

float ui_button_content_width(const C2D_Text *label, bool icon)
{
	return (icon ? ICON_S + SP_1 : 0) + ui_text_width(label, TXT_CUERPO);
}

/* The button's surface in any state of 6.1. Returns where it ended up;
 * `*ink` is the colour of its content. */
static ui_rect button_body(ui_rect r, ui_button_kind kind,
                           ui_control_look look, u32 *ink_out)
{
	ui_rect body;
	u32 ink;
	if (look.disabled) {
		/* 6.1: flat `capa`, no bevel or shadow, `texto_off`. */
		body = r;
		outlined(body, COL_CAPA);
		ink = COL_TEXTO_OFF;
	} else {
		bool pressed = look.pressed;
		switch (kind) {
		case UI_BUTTON_PRIMARY:
			body = ui_bevel_rect(r, pressed ? COL_MUSGO : COL_CLOROFILA,
			                     COL_MENTA, COL_MUSGO, pressed);
			ink = COL_TINTA;
			break;
		case UI_BUTTON_DANGER:
			body = ui_bevel_rect(r, pressed ? COL_CORAL_OSC : COL_CORAL,
			                     COL_CORAL_LUZ, COL_CORAL_OSC, pressed);
			ink = COL_TINTA;
			break;
		default:
			body = ui_bevel_rect(r, pressed ? COL_CAPA : COL_CAPA_ALTA,
			                     COL_CAPA_LUZ, COL_SOMBRA, pressed);
			ink = COL_TEXTO;
			break;
		}
		if (look.focused)
			ui_focus_ring(body);
	}
	*ink_out = ink;
	return body;
}

void ui_icon_button(ui_rect r, ui_button_kind kind, ui_control_look look,
                    const C2D_Image *icon)
{
	u32 ink;
	ui_rect body = button_body(r, kind, look, &ink);
	ui_icon(icon, body.x + (body.w - ICON_S) / 2,
	        body.y + (body.h - ICON_S) / 2, ink);
}

void ui_button(ui_rect r, ui_button_kind kind, ui_control_look look,
               const C2D_Text *label, const C2D_Image *icon)
{
	u32 ink;
	ui_rect body = button_body(r, kind, look, &ink);
	bool has_icon = icon && icon->tex;
	int width = ui_px_ceil(ui_button_content_width(label, has_icon));
	int x = body.x + (body.w - width) / 2;
	int cy = body.y + body.h / 2;
	if (has_icon) {
		ui_icon(icon, x, cy - ICON_S / 2, ink);
		x += ICON_S + SP_1;
	}
	ui_text(label, x, cy, TXT_CUERPO, ink);
}

int ui_glyph(int x, int cy, const C2D_Text *letter, u32 color, bool inverted)
{
	int w = ui_px_ceil(ui_text_width(letter, TXT_META)) + SP_2;
	if (w < GLYPH_H)
		w = GLYPH_H;
	ui_rect r = { x, cy - GLYPH_H / 2, w, GLYPH_H };
	/* 6.5: inverted, a `tinta` glyph with the letter in the button's
	 * colour, 1 px lower. */
	if (inverted)
		r = ui_rect_offset(r, 0, GLYPH_SHIFT);
	outlined(r, inverted ? COL_TINTA : color);
	ui_text_centered(letter, r.x + r.w / 2, r.y + r.h / 2, TXT_META,
	                 inverted ? color : COL_TINTA);
	return w;
}

void ui_button_bar(ui_rect bar, const ui_hint *hints, int count)
{
	ui_panel(bar);
	int cy = bar.y + bar.h / 2;
	int x = bar.x + SP_2;
	for (int i = 0; i < count; i++) {
		x += ui_glyph(x, cy, hints[i].letter, hints[i].color,
		              hints[i].inverted) + SP_1;
		ui_text(hints[i].label, x, cy, TXT_META, COL_TEXTO_2);
		x += ui_px_ceil(ui_text_width(hints[i].label, TXT_META)) + SP_3;
	}
}

void ui_progress(ui_rect r, int download, int progress, bool handle)
{
	ui_rect inside = outlined(r, COL_CAPA_ALTA);
	if (download >= 0) {
		ui_rect loaded = inside;
		loaded.w = player_fill_px(inside.w, download);
		ui_fill(loaded, COL_MUSGO);
	}
	ui_rect played = inside;
	played.w = player_fill_px(inside.w, progress);
	ui_fill(played, COL_CLOROFILA);
	if (handle) {
		ui_rect knob = { played.x + played.w - HANDLE_W / 2,
		                 r.y + r.h / 2 - HANDLE_H / 2, HANDLE_W, HANDLE_H };
		outlined(knob, COL_TEXTO);
	}
}

void ui_loader(int cx, int cy, int step)
{
	/* Each bar a step behind the previous one: 0, -2, -4, -2 px. */
	static const int lift[] = { 0, 1, 2, 1 };
	int x = cx - UI_LOADER_W / 2;
	for (int i = 0; i < UI_LOADER_BARS; i++) {
		int up = lift[(step + i) % 4] * LOADER_STEP_PX;
		ui_rect bar = { x + i * (LOADER_BAR_W + SP_1),
		                cy - LOADER_BAR_H / 2 - up, LOADER_BAR_W,
		                LOADER_BAR_H };
		outlined(bar, COL_CLOROFILA);
	}
}

void ui_notice(ui_rect r, bool error, const C2D_Image *icon,
               const C2D_Text *title, const C2D_Text *body)
{
	ui_rect body_rect = error
		? ui_bevel_rect(r, COL_CORAL, COL_CORAL_LUZ, COL_CORAL_OSC, false)
		: ui_bevel_rect(r, COL_AMBAR, COL_AMBAR_LUZ, COL_AMBAR_OSC, false);
	int x = body_rect.x + SP_3;
	int title_cy = body_rect.y + SP_3 + ICON_S / 2;
	ui_icon(icon, x, body_rect.y + SP_3, COL_TINTA);
	ui_text(title, x + ICON_S + SP_1, title_cy, TXT_CABEZA, COL_TINTA);
	int body_y = body_rect.y + SP_3 + ICON_S + SP_2;
	ui_text_wrapped(body, x, body_y, TXT_CUERPO, COL_TINTA,
	                body_rect.w - 2 * SP_3);
}

void ui_art(ui_rect box, const C2D_Image *image)
{
	ui_rect inside = outlined(box, COL_CAPA_ALTA);
	if (image && image->tex && image->subtex->width &&
	    image->subtex->height) {
		/* Fitted into the box and centred, on whole pixels; the
		 * `capa_alta` fill shows only beside a non-square image. */
		int w = image->subtex->width, h = image->subtex->height;
		float sx = (float)inside.w / (float)w;
		float sy = (float)inside.h / (float)h;
		float scale = sx < sy ? sx : sy;
		int shown_w = ui_px_ceil(w * scale), shown_h = ui_px_ceil(h * scale);
		C2D_DrawImageAt(*image, inside.x + (inside.w - shown_w) / 2,
		                inside.y + (inside.h - shown_h) / 2, 0.0f, NULL,
		                scale, scale);
		return;
	}
	C2D_Image note = icon_image(ICON_NOTE);
	ui_icon(&note, box.x + (box.w - ICON_S) / 2,
	        box.y + (box.h - ICON_S) / 2, COL_TEXTO_2);
}

float ui_bubble_content_width(const C2D_Text *text)
{
	return ICON_S + SP_1 + ui_text_width(text, TXT_CUERPO);
}

/* The raised `capa_alta` surface of a bubble (5.9), shared by both kinds;
 * returns where it landed. */
static ui_rect bubble_body(ui_rect r)
{
	return ui_bevel_rect(r, COL_CAPA_ALTA, COL_CAPA_LUZ, COL_SOMBRA, false);
}

void ui_bubble(ui_rect r, const C2D_Image *icon, const C2D_Text *text)
{
	ui_rect body = bubble_body(r);
	int cy = body.y + body.h / 2;
	int x = body.x + SP_3;
	ui_icon(icon, x, cy - ICON_S / 2, COL_TEXTO);
	ui_text(text, x + ICON_S + SP_1, cy, TXT_CUERPO, COL_TEXTO);
}

float ui_bubble_loading_content_width(const C2D_Text *text)
{
	return UI_LOADER_W + SP_1 + ui_text_width(text, TXT_CUERPO);
}

void ui_bubble_loading(ui_rect r, int step, const C2D_Text *text)
{
	ui_rect body = bubble_body(r);
	int cy = body.y + body.h / 2;
	int x = body.x + SP_3;
	ui_loader(x + UI_LOADER_W / 2, cy, step);
	ui_text(text, x + UI_LOADER_W + SP_1, cy, TXT_CUERPO, COL_TEXTO);
}

/* --- Text fitting ------------------------------------------------------ */

typedef struct {
	C2D_TextBuf scratch;
	float scale;
} fit_ctx;

static float measure_fit(const char *string, void *ctx)
{
	const fit_ctx *fit = ctx;
	return ui_measure(fit->scratch, string, fit->scale);
}

float ui_measure(C2D_TextBuf scratch, const char *string, float scale)
{
	C2D_Text text;
	C2D_TextBufClear(scratch);
	C2D_TextParse(&text, scratch, string);
	return ui_text_width(&text, scale);
}

void ui_parse_fitted(C2D_Text *text, C2D_TextBuf buf, C2D_TextBuf scratch,
                     const char *string, float scale, int width)
{
	char fitted[UI_FIELD_SIZE];
	fit_ctx fit = { scratch, scale };
	text_fit(string, (float)width, measure_fit, &fit, fitted,
	         sizeof(fitted));
	C2D_TextParse(text, buf, fitted);
	C2D_TextOptimize(text);
}
