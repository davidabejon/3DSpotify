#include "top_strip.h"

#include "draw.h"
#include "icons.h"
#include "theme.h"
#include "ui_font.h"

#include <citro2d.h>

#include <stdio.h>

#define NUMBER_GLYPHS 32

static const icon_id KIND_ICON[RESULT_KINDS] = {
	ICON_NOTE, ICON_USER, ICON_ALBUM, ICON_PLAYLIST,
};

static C2D_TextBuf buf;
static C2D_Text numbers[RESULT_KINDS];
static int parsed_count[RESULT_KINDS];
static bool parsed;
static results_strip drawn;
static bool drawn_valid;

bool top_strip_init(void)
{
	buf = C2D_TextBufNew(NUMBER_GLYPHS);
	if (!buf)
		return false;
	parsed = drawn_valid = false;
	return true;
}

void top_strip_exit(void)
{
	if (buf)
		C2D_TextBufDelete(buf);
	buf = NULL;
	parsed = drawn_valid = false;
}

void top_strip_invalidate(void)
{
	drawn_valid = false;
}

bool top_strip_dirty(const results_strip *s)
{
	return !drawn_valid || !results_strip_same(s, &drawn);
}

/* The counts as text, when one changed. */
static void ensure_parsed(const results_strip *s)
{
	bool same = parsed;
	for (int k = 0; same && k < RESULT_KINDS; k++)
		same = parsed_count[k] == s->count[k];
	if (same)
		return;
	C2D_TextBufClear(buf);
	for (int k = 0; k < RESULT_KINDS; k++) {
		char text[12];
		snprintf(text, sizeof(text), "%d", s->count[k]);
		ui_parse(&numbers[k], buf, text);
		C2D_TextOptimize(&numbers[k]);
		parsed_count[k] = s->count[k];
	}
	parsed = true;
}

/* One segment: at rest `capa` inside a `tinta` outline, flat (5.1); the
 * active one `clorofila` with a 2 px `musgo` band at its foot; one with
 * nothing found `capa` with its content in `texto_off` (2.6). The icon
 * and the count centred together. */
static void draw_segment(ui_rect r, result_kind k, bool active, int count)
{
	ui_fill(r, COL_TINTA);
	ui_rect inside = ui_rect_inset(r, OUTLINE_W);
	ui_fill(inside, active ? COL_CLOROFILA : COL_CAPA);
	if (active)
		ui_fill((ui_rect){ inside.x, inside.y + inside.h - RESULTS_BAND_H,
		                   inside.w, RESULTS_BAND_H }, COL_MUSGO);
	u32 ink = active ? COL_TINTA : count > 0 ? COL_TEXTO : COL_TEXTO_OFF;
	int text_w = ui_px_ceil(ui_text_width(&numbers[k], TXT_META));
	int content = ICON_S + SP_1 + text_w;
	int x = r.x + (r.w - content) / 2;
	int cy = r.y + r.h / 2;
	C2D_Image icon = icon_image(KIND_ICON[k]);
	ui_icon(&icon, x, cy - ICON_S / 2, ink);
	ui_text(&numbers[k], x + ICON_S + SP_1, cy, TXT_META, ink);
}

void top_strip_draw(const results_strip *s)
{
	ensure_parsed(s);
	result_kind kinds[RESULT_KINDS];
	int n = 0;
	for (int k = 0; k < RESULT_KINDS; k++)
		if (s->offered[k])
			kinds[n++] = (result_kind)k;
	int x[RESULT_KINDS], w[RESULT_KINDS];
	results_strip_layout(n, x, w);
	/* The active one last: its nudge draws over its neighbour. */
	int active_at = -1;
	for (int i = 0; i < n; i++) {
		if (kinds[i] == s->active) {
			active_at = i;
			continue;
		}
		draw_segment((ui_rect){ x[i], RESULTS_STRIP_Y, w[i], RESULTS_STRIP_H },
		             kinds[i], false, s->count[kinds[i]]);
	}
	if (active_at >= 0) {
		int shift = s->edge_frames > 0 ? s->edge_dir * EDGE_SHIFT : 0;
		draw_segment((ui_rect){ x[active_at] + shift, RESULTS_STRIP_Y,
		                        w[active_at], RESULTS_STRIP_H },
		             s->active, true, s->count[s->active]);
	}
	drawn = *s;
	drawn_valid = true;
}
