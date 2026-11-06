#include "home_logic.h"
#include "image_util.h"
#include "lang.h"
#include "layout.h"
#include "list_logic.h"
#include "player_logic.h"
#include "press_logic.h"
#include "search_path.h"
#include "text_fit.h"
#include "theme.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static void test_search_path_encoding(void)
{
	char path[448];
	assert(sp_search_path("Beyonce Halo", path, sizeof(path)));
	assert(strcmp(path,
	              "/context-resolve/v1/spotify:search:Beyonce+Halo") == 0);
	assert(sp_search_path("rock + roll/100%", path, sizeof(path)));
	assert(strcmp(path,
	              "/context-resolve/v1/spotify:search:rock+%2B+roll%2F100%25")
	       == 0);
	assert(sp_search_path("canci\xc3\xb3n", path, sizeof(path)));
	assert(strcmp(path,
	              "/context-resolve/v1/spotify:search:canci%C3%B3n") == 0);
	assert(!sp_search_path("", path, sizeof(path)));
	assert(!sp_search_path("test", path, 4));
}

static void test_queue_boundaries(void)
{
	assert(playback_queue_step(0, 3, -1) == -1);
	assert(playback_queue_step(0, 3, 1) == 1);
	assert(playback_queue_step(1, 3, -1) == 0);
	assert(playback_queue_step(1, 3, 1) == 2);
	assert(playback_queue_step(2, 3, 1) == -1);
	assert(playback_queue_step(0, 0, 1) == -1);
	assert(playback_queue_step(3, 3, -1) == -1);
	assert(playback_queue_step(1, 3, 0) == -1);
}

static void test_playback_cancel_transitions(void)
{
	assert(playback_cancel_result(false, false) == PLAYER_RETURN_RESULTS);
	assert(playback_cancel_result(false, true) == PLAYER_TRACK_CHANGED);
	assert(playback_cancel_result(true, false) == PLAYER_EXIT_APP);
	assert(playback_cancel_result(true, true) == PLAYER_EXIT_APP);
}

/* The conversion specifiers of a printf format, flags and widths included,
 * so "%02d" in one language and "%2d" in another count as different. */
static void format_signature(const char *text, char *out, size_t outlen)
{
	size_t n = 0;
	for (const char *p = text; *p; p++) {
		if (*p != '%')
			continue;
		p++;
		if (*p == '%')
			continue;
		while (*p && !strchr("diouxXeEfgGcsp", *p)) {
			assert(n + 1 < outlen);
			out[n++] = *p++;
		}
		assert(*p && n + 2 < outlen);
		out[n++] = *p;
		out[n++] = ';';
	}
	out[n] = '\0';
}

static void test_language_tables(void)
{
	for (int lang = 0; lang < LANG_COUNT; lang++) {
		for (int id = 0; id < STR_COUNT; id++) {
			const char *text = lang_text((lang_id)lang, (str_id)id);
			/* Every entry is filled in, with no fallback to English. */
			assert(text[0]);
			assert(lang == LANG_EN ||
			       text != lang_text(LANG_EN, (str_id)id) ||
			       strcmp(text, "Error") == 0 ||
			       strcmp(text, "Album") == 0 ||
			       strcmp(text, "3DSpotify") == 0);
			/* The console font is ASCII only. */
			for (const char *p = text; *p; p++)
				assert((unsigned char)*p >= 0x20 &&
				       (unsigned char)*p < 0x7f);
			char expected[64], actual[64];
			format_signature(lang_text(LANG_EN, (str_id)id), expected,
			                 sizeof(expected));
			format_signature(text, actual, sizeof(actual));
			assert(strcmp(expected, actual) == 0);
		}
		/* Button hints fit one 40-column console line without wrapping. */
		for (int id = STR_KEYS_LANGUAGE; id <= STR_KEYS_STARTUP_RETRY; id++)
			assert(strlen(lang_text((lang_id)lang, (str_id)id)) < 40);
	}
}

static void test_language_switching(void)
{
	lang_id lang;
	assert(lang_get() == LANG_EN);
	assert(lang_next(LANG_EN) == LANG_ES);
	assert(lang_next(LANG_ES) == LANG_EN);
	assert(lang_from_code("es", &lang) && lang == LANG_ES);
	assert(lang_from_code(lang_code(LANG_EN), &lang) && lang == LANG_EN);
	assert(!lang_from_code("fr", &lang));
	assert(!lang_from_code(NULL, &lang));
	lang_set(LANG_ES);
	assert(strcmp(tr(STR_PLAYER_TITLE), "Reproduccion local") == 0);
	lang_set(lang_next(lang_get()));
	assert(strcmp(tr(STR_PLAYER_TITLE), "Local playback") == 0);
	lang_set((lang_id)LANG_COUNT);
	assert(lang_get() == LANG_EN);
	assert(strcmp(lang_text(LANG_ES, (str_id)STR_COUNT), "") == 0);
}

static void test_image_fit(void)
{
	int w = 0, h = 0;
	image_fit(640, 640, 256, &w, &h);
	assert(w == 256 && h == 256);
	image_fit(640, 320, 256, &w, &h);
	assert(w == 256 && h == 128);
	image_fit(300, 600, 256, &w, &h);
	assert(w == 128 && h == 256);
	image_fit(1000, 1, 256, &w, &h);
	assert(w == 256 && h == 1);
	image_fit(0, 10, 256, &w, &h);
	assert(w == 1 && h == 1);
}

static void test_image_morton_offset(void)
{
	/* Inside the first 8x8 tile: Z-order, x in the low bit. */
	assert(image_morton_offset(0, 0, 256) == 0);
	assert(image_morton_offset(1, 0, 256) == 1);
	assert(image_morton_offset(0, 1, 256) == 2);
	assert(image_morton_offset(1, 1, 256) == 3);
	assert(image_morton_offset(2, 0, 256) == 4);
	assert(image_morton_offset(0, 2, 256) == 8);
	assert(image_morton_offset(4, 0, 256) == 16);
	assert(image_morton_offset(0, 4, 256) == 32);
	assert(image_morton_offset(7, 7, 256) == 63);
	/* Tiles go left to right, then down. */
	assert(image_morton_offset(8, 0, 256) == 64);
	assert(image_morton_offset(0, 8, 256) == 32 * 64);
	assert(image_morton_offset(255, 255, 256) == 256 * 256 - 1);
	assert(image_morton_offset(9, 8, 16) == 3 * 64 + 1);
}

static void test_image_to_texture(void)
{
	/* 2x2 RGBA box-filtered to one texel, written as A, B, G, R. */
	const uint8_t src[16] = {
		10, 20, 30, 255,   30, 40, 50, 255,
		50, 60, 70, 255,   70, 80, 90, 255,
	};
	uint8_t tex[8 * 8 * 4];
	memset(tex, 0xEE, sizeof(tex));
	image_to_texture(src, 2, 2, tex, 8, 1, 1);
	assert(tex[0] == 255 && tex[1] == 60 && tex[2] == 50 && tex[3] == 40);
	/* Only the dw x dh corner is written. */
	assert(tex[4] == 0xEE && tex[sizeof(tex) - 1] == 0xEE);

	/* Same size: every pixel lands at its Morton offset. */
	uint8_t big[8 * 8 * 4];
	for (int i = 0; i < 64; i++) {
		big[i * 4] = (uint8_t)i;      /* R carries the source index */
		big[i * 4 + 1] = 0;
		big[i * 4 + 2] = 0;
		big[i * 4 + 3] = 255;
	}
	image_to_texture(big, 8, 8, tex, 8, 8, 8);
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++) {
			uint32_t at = image_morton_offset((uint32_t)x, (uint32_t)y, 8);
			assert(tex[at * 4 + 3] == y * 8 + x);
			assert(tex[at * 4] == 255);
		}

	/* Upscaling repeats source pixels instead of reading past them. */
	const uint8_t one[4] = {1, 2, 3, 4};
	image_to_texture(one, 1, 1, tex, 8, 8, 8);
	for (int i = 0; i < 64; i++)
		assert(tex[i * 4] == 4 && tex[i * 4 + 3] == 1);

	/* Bad arguments leave the texture alone. */
	memset(tex, 0x11, sizeof(tex));
	image_to_texture(src, 2, 2, tex, 8, 16, 16);
	image_to_texture(NULL, 2, 2, tex, 8, 1, 1);
	assert(tex[0] == 0x11);
}

/* Every byte is 6 px wide, so a multibyte character is wider than it looks:
 * the point is that a cut never lands inside one. */
static float fake_measure(const char *text, void *ctx)
{
	int *calls = ctx;
	if (calls)
		(*calls)++;
	return 6.0f * (float)strlen(text);
}

static bool valid_utf8(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		int len = *p < 0x80 ? 1 : *p >= 0xF0 ? 4 : *p >= 0xE0 ? 3
		          : *p >= 0xC2 ? 2 : 0;
		if (len == 0)
			return false;
		for (int i = 1; i < len; i++)
			if ((p[i] & 0xC0) != 0x80)
				return false;
		p += len;
	}
	return true;
}

static bool ends_with_ellipsis(const char *s)
{
	size_t n = strlen(s), e = strlen(TEXT_FIT_ELLIPSIS);
	return n >= e && strcmp(s + n - e, TEXT_FIT_ELLIPSIS) == 0;
}

static void test_text_fit(void)
{
	char out[64];
	/* Fits: copied as is. */
	assert(!text_fit("daft punk", 54.0f, fake_measure, NULL, out,
	                 sizeof(out)));
	assert(strcmp(out, "daft punk") == 0);
	/* Too wide: prefix + ellipsis (3 bytes = 18 px). 60 px = 7 bytes. */
	assert(text_fit("daft punk", 53.0f, fake_measure, NULL, out,
	                sizeof(out)));
	assert(strcmp(out, "daft" TEXT_FIT_ELLIPSIS) == 0);
	/* Trailing spaces before the ellipsis are dropped. */
	assert(text_fit("ab   cdef", 48.0f, fake_measure, NULL, out,
	                sizeof(out)));
	assert(strcmp(out, "ab" TEXT_FIT_ELLIPSIS) == 0);
	/* Not even the ellipsis fits: empty. */
	assert(text_fit("abc", 10.0f, fake_measure, NULL, out, sizeof(out)));
	assert(out[0] == '\0');
	assert(!text_fit(NULL, 100.0f, fake_measure, NULL, out, sizeof(out)));
	assert(out[0] == '\0');

	/* Never cuts inside a multibyte character, at any width. */
	const char *mixed = "canci\xc3\xb3n \xe6\x97\xa5\xe6\x9c\xac "
	                    "\xf0\x9f\x8e\xb5 m\xc3\xbasica";
	assert(valid_utf8(mixed));
	for (int px = 0; px <= 6 * 40; px++) {
		bool cut = text_fit(mixed, (float)px, fake_measure, NULL, out,
		                    sizeof(out));
		assert(valid_utf8(out));
		assert(fake_measure(out, NULL) <= (float)px);
		if (cut && out[0])
			assert(ends_with_ellipsis(out));
		if (!cut)
			assert(strcmp(out, mixed) == 0);
	}
	/* The longest prefix that fits is chosen: one more character would
	 * not fit. "canci" + "o-acute" is 7 bytes, + ellipsis = 60 px. */
	assert(text_fit(mixed, 60.0f, fake_measure, NULL, out, sizeof(out)));
	assert(strcmp(out, "canci\xc3\xb3" TEXT_FIT_ELLIPSIS) == 0);
	assert(text_fit(mixed, 59.0f, fake_measure, NULL, out, sizeof(out)));
	assert(strcmp(out, "canci" TEXT_FIT_ELLIPSIS) == 0);

	/* A small output buffer truncates even when the width would allow
	 * more, and stays terminated. */
	char tiny[8];
	assert(text_fit("abcdefghij", 1000.0f, fake_measure, NULL, tiny,
	                sizeof(tiny)));
	assert(strcmp(tiny, "abcd" TEXT_FIT_ELLIPSIS) == 0);

	/* Binary search: a long string costs a handful of measurements. */
	char long_text[96];
	memset(long_text, 'x', sizeof(long_text) - 1);
	long_text[sizeof(long_text) - 1] = '\0';
	int calls = 0;
	text_fit(long_text, 120.0f, fake_measure, &calls, out, sizeof(out));
	assert(calls <= 10);
	assert(fake_measure(out, NULL) <= 120.0f);
}

static void test_home_layout(void)
{
	const float content = 119.5f;
	ui_rect search = home_search_rect(content);
	ui_rect label = home_label_rect();
	ui_rect repeat = home_repeat_rect(50);
	ui_rect header = home_header_rect();
	ui_rect hints = home_hints_rect();

	/* Bars of BAR_H at both ends; 8 px side margins (STYLES.md 4.2). */
	assert(header.y == 0 && header.h == BAR_H);
	assert(hints.h == BAR_H && hints.y + hints.h == SCREEN_H);
	assert(search.x == SP_2 && repeat.x == SP_2);
	/* Blocks SP_4 apart, starting SP_4 under the header. */
	assert(search.y == header.h + SP_4);
	assert(label.y == search.y + search.h + SP_4);
	assert(repeat.y >= label.y + label.h);

	/* Buttons: 32 px high, hugging content plus SP_3 each side (5.2),
	 * rounded up to whole pixels (4.3). */
	assert(search.h == TOUCH_MIN && repeat.h == TOUCH_MIN);
	assert(search.w == 120 + 2 * SP_3);
	assert(home_search_rect(0).w == TOUCH_MIN);
	assert(home_repeat_rect(1000).w == SCREEN_BOTTOM_W - 2 * SP_2);
	assert(home_repeat_max_text_width() + 2 * SP_3 ==
	       home_repeat_rect(1000).w);

	/* Focus borders and hard shadows stay clear of each other and of the
	 * button bar: 4 px between targets at least (4.4). */
	int ring = FOCUS_GAP + FOCUS_W;
	int below = ring > SHADOW_OFF ? ring : SHADOW_OFF;
	assert((repeat.y - ring) - (search.y + search.h + below) >= SP_1);
	assert(repeat.y + repeat.h + below + PRESS_SHIFT <= hints.y);

	/* Hit testing matches what is drawn. */
	assert(home_hit_test(search.x, search.y, content, false, 0) ==
	       HOME_ITEM_SEARCH);
	assert(home_hit_test(search.x + search.w - 1, search.y + search.h - 1,
	                     content, true, 50) == HOME_ITEM_SEARCH);
	assert(home_hit_test(search.x + search.w, search.y, content, false, 0)
	       == HOME_ITEM_NONE);
	assert(home_hit_test(repeat.x + 1, repeat.y + 1, content, true, 50) ==
	       HOME_ITEM_REPEAT);
	assert(home_hit_test(repeat.x + 1, repeat.y + 1, content, false, 50) ==
	       HOME_ITEM_NONE);
	assert(home_hit_test(repeat.x + repeat.w + 1, repeat.y + 1, content,
	                     true, 50) == HOME_ITEM_NONE);
	/* Bars and empty space do nothing (6.4). */
	assert(home_hit_test(10, 10, content, true, 50) == HOME_ITEM_NONE);
	assert(home_hit_test(10, 230, content, true, 50) == HOME_ITEM_NONE);

	/* The bubble: centred, SP_2 above the button bar, clear of the
	 * repeat button and its shadow. */
	ui_rect bubble = home_bubble_rect(100);
	assert(bubble.w == 100 + 2 * SP_3);
	assert(bubble.x * 2 + bubble.w == SCREEN_BOTTOM_W);
	assert(bubble.y + bubble.h + SP_2 == hints.y);
	assert(bubble.y > repeat.y + repeat.h + SHADOW_OFF);
	assert(home_bubble_rect(1000).w == SCREEN_BOTTOM_W - 2 * SP_2);
}

static void test_home_focus(void)
{
	assert(home_focus_move(HOME_ITEM_SEARCH, 1, true) == HOME_ITEM_REPEAT);
	assert(home_focus_move(HOME_ITEM_REPEAT, 1, true) == HOME_ITEM_REPEAT);
	assert(home_focus_move(HOME_ITEM_REPEAT, -1, true) == HOME_ITEM_SEARCH);
	assert(home_focus_move(HOME_ITEM_SEARCH, -1, true) == HOME_ITEM_SEARCH);
	/* Without a last search there is only the search button. */
	assert(home_focus_move(HOME_ITEM_SEARCH, 1, false) == HOME_ITEM_SEARCH);
	/* A focus that went away falls back. */
	assert(home_focus_move(HOME_ITEM_REPEAT, 0, false) == HOME_ITEM_SEARCH);
	assert(home_focus_move(HOME_ITEM_NONE, 1, true) == HOME_ITEM_REPEAT);
}

static void test_layout_rects(void)
{
	ui_rect r = { 10, 20, 100, 40 };
	ui_rect g = ui_rect_inset(r, -3);
	assert(g.x == 7 && g.y == 17 && g.w == 106 && g.h == 46);
	ui_rect o = ui_rect_offset(r, 2, 2);
	assert(o.x == 12 && o.y == 22 && o.w == r.w && o.h == r.h);
	assert(ui_rect_contains(r, 10, 20) && !ui_rect_contains(r, 110, 20));
	assert(ui_px_ceil(12.0f) == 12 && ui_px_ceil(12.01f) == 13);
	assert(ui_px_ceil(0.0f) == 0);
}

/* Ticks `n` frames. */
static void ticks(press_state *p, int n)
{
	while (n-- > 0)
		press_tick(p);
}

static void test_press_timing(void)
{
	press_state p;
	press_reset(&p);
	assert(!press_down(&p) && press_shown(&p));

	/* A quick tap still shows the pressed look ANIM_PRESS_MIN frames,
	 * then returns to rest in a single frame (6.3). */
	press_begin(&p);
	press_end(&p);
	int pressed_frames = 0;
	while (press_down(&p)) {
		assert(!press_shown(&p));
		pressed_frames++;
		press_tick(&p);
	}
	assert(pressed_frames == ANIM_PRESS_MIN);
	assert(press_shown(&p));

	/* Held: pressed for as long as the button is down. */
	press_begin(&p);
	ticks(&p, ANIM_PRESS_MIN * 10);
	assert(press_down(&p) && press_shown(&p));
	press_end(&p);
	press_tick(&p);
	assert(!press_down(&p));

	/* Cancelled (stylus slid out): back to rest at once. */
	press_begin(&p);
	press_tick(&p);
	press_cancel(&p);
	assert(!press_down(&p));
}

static void test_touch_tracking(void)
{
	touch_track t = { -1 };
	int control;
	/* Touching outside every control: nothing, ever. */
	assert(touch_track_update(&t, true, true, false, -1, &control) ==
	       TOUCH_NONE);
	assert(touch_track_update(&t, false, false, true, -1, &control) ==
	       TOUCH_NONE);

	/* Press inside, lift inside: activate on release. */
	assert(touch_track_update(&t, true, true, false, 1, &control) ==
	       TOUCH_PRESS && control == 1);
	assert(touch_track_update(&t, false, true, false, 1, &control) ==
	       TOUCH_NONE);
	assert(touch_track_update(&t, false, false, true, -1, &control) ==
	       TOUCH_ACTIVATE && control == 1);
	assert(t.target == -1);

	/* Sliding out cancels for good, even when coming back. */
	assert(touch_track_update(&t, true, true, false, 0, &control) ==
	       TOUCH_PRESS);
	assert(touch_track_update(&t, false, true, false, -1, &control) ==
	       TOUCH_CANCEL && control == 0);
	assert(touch_track_update(&t, false, true, false, 0, &control) ==
	       TOUCH_NONE);
	assert(touch_track_update(&t, false, false, true, -1, &control) ==
	       TOUCH_NONE);

	/* Sliding onto another control cancels too. */
	assert(touch_track_update(&t, true, true, false, 0, &control) ==
	       TOUCH_PRESS);
	assert(touch_track_update(&t, false, true, false, 1, &control) ==
	       TOUCH_CANCEL && control == 0);
}

/* WCAG relative luminance and contrast ratio. */
static double channel_luminance(uint32_t c, int shift)
{
	double v = (double)((c >> shift) & 0xFF) / 255.0;
	return v <= 0.03928 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double luminance(uint32_t c)
{
	return 0.2126 * channel_luminance(c, 0) +
	       0.7152 * channel_luminance(c, 8) +
	       0.0722 * channel_luminance(c, 16);
}

static double contrast(uint32_t a, uint32_t b)
{
	double la = luminance(a), lb = luminance(b);
	double hi = la > lb ? la : lb, lo = la > lb ? lb : la;
	return (hi + 0.05) / (lo + 0.05);
}

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static void test_theme_colors(void)
{
	/* Every colour is opaque (2, 2.7). */
	const uint32_t all[] = {
		COL_FONDO_SUP, COL_FONDO_INF, COL_CAPA, COL_CAPA_ALTA, COL_CAPA_LUZ,
		COL_SOMBRA, COL_CLOROFILA, COL_MENTA, COL_MUSGO, COL_LIMA,
		COL_TINTA, COL_TEXTO, COL_TEXTO_2, COL_TEXTO_OFF, COL_CORAL,
		COL_CORAL_LUZ, COL_CORAL_OSC, COL_AMBAR, COL_AMBAR_LUZ,
		COL_AMBAR_OSC, COL_BTN_A, COL_BTN_B, COL_BTN_X, COL_BTN_Y,
	};
	for (size_t i = 0; i < COUNT(all); i++)
		assert(all[i] >> 24 == 0xFF);
	/* Packing matches C2D_Color32: R in the low byte. */
	assert(COL_BTN_X == (0x3Fu | 0xA2u << 8 | 0xFFu << 16 | 0xFFu << 24));
	/* Never Spotify's greens (2.7). */
	for (size_t i = 0; i < COUNT(all); i++) {
		assert(all[i] != THEME_RGB(0x1D, 0xB9, 0x54));
		assert(all[i] != THEME_RGB(0x1E, 0xD7, 0x60));
	}
}

static void test_theme_contrast(void)
{
	/* The text combinations of STYLES.md 2.6, >= 4.5:1 (10.2). */
	const uint32_t on_background[] = { COL_TEXTO, COL_TEXTO_2 };
	const uint32_t backgrounds[] = { COL_FONDO_SUP, COL_FONDO_INF };
	for (size_t i = 0; i < COUNT(on_background); i++)
		for (size_t j = 0; j < COUNT(backgrounds); j++)
			assert(contrast(on_background[i], backgrounds[j]) >= 4.5);

	/* `capa` also holds disabled text and a pressed secondary button's
	 * label (6.1, 6.3). */
	const uint32_t on_capa[] = {
		COL_TEXTO, COL_TEXTO_2, COL_TEXTO_OFF, COL_CLOROFILA, COL_LIMA
	};
	for (size_t i = 0; i < COUNT(on_capa); i++)
		assert(contrast(on_capa[i], COL_CAPA) >= 4.5);

	assert(contrast(COL_TEXTO, COL_CAPA_ALTA) >= 4.5);
	assert(contrast(COL_TEXTO_2, COL_CAPA_ALTA) >= 4.5);

	/* Includes the pressed fills `musgo` and `coral_osc` (6.3). */
	const uint32_t under_tinta[] = {
		COL_CLOROFILA, COL_MUSGO, COL_LIMA, COL_CORAL, COL_CORAL_OSC,
		COL_AMBAR, COL_BTN_A, COL_BTN_B, COL_BTN_X, COL_BTN_Y,
	};
	for (size_t i = 0; i < COUNT(under_tinta); i++)
		assert(contrast(COL_TINTA, under_tinta[i]) >= 4.5);

	/* Disabled text stays dimmer than secondary text. */
	assert(luminance(COL_TEXTO_OFF) < luminance(COL_TEXTO_2));
	/* Pressed fills stay darker than their resting colour. */
	assert(luminance(COL_CORAL_OSC) < luminance(COL_CORAL));
	assert(luminance(COL_MUSGO) < luminance(COL_CLOROFILA));
}

static void test_list_logic(void)
{
	bool bumped;
	/* Clamping. */
	assert(list_clamp(5, 3) == 2 && list_clamp(-1, 3) == 0);
	assert(list_clamp(4, 0) == 0);

	/* Moving stops at the ends and reports the bump (6.6). */
	assert(list_move(0, 1, 24, &bumped) == 1 && !bumped);
	assert(list_move(0, -1, 24, &bumped) == 0 && bumped);
	assert(list_move(23, 1, 24, &bumped) == 23 && bumped);
	assert(list_move(2, -RESULTS_PER_PAGE, 24, &bumped) == 0 && !bumped);
	assert(list_move(21, RESULTS_PER_PAGE, 24, &bumped) == 23 && !bumped);
	assert(list_move(0, 1, 0, &bumped) == 0 && !bumped);

	/* Scrolling moves just enough to keep the selection visible. */
	assert(list_scroll(0, 3, 24, 4) == 0);
	assert(list_scroll(0, 4, 24, 4) == 1);
	assert(list_scroll(5, 4, 24, 4) == 4);
	assert(list_scroll(5, 6, 24, 4) == 5);
	assert(list_scroll(0, 23, 24, 4) == 20);
	assert(list_scroll(30, 23, 24, 4) == 20);
	assert(list_scroll(2, 1, 3, 4) == 0);
	assert(list_scroll(0, 0, 0, 4) == 0);

	/* "from-to of count". */
	int from, to;
	list_range(0, 24, 4, &from, &to);
	assert(from == 1 && to == 4);
	list_range(20, 24, 4, &from, &to);
	assert(from == 21 && to == 24);
	list_range(0, 3, 4, &from, &to);
	assert(from == 1 && to == 3);
	list_range(0, 0, 4, &from, &to);
	assert(from == 0 && to == 0);

	/* 7.4: four rows of ROW_H, SP_1 apart, fill the 192 px between the
	 * bars exactly. */
	ui_rect header = list_header_rect();
	ui_rect hints = list_hints_rect();
	assert(header.h == BAR_H && hints.h == BAR_H);
	assert(list_row_rect(0).y == header.y + header.h + SP_1);
	for (int slot = 1; slot < RESULTS_PER_PAGE; slot++)
		assert(list_row_rect(slot).y ==
		       list_row_rect(slot - 1).y + ROW_H + SP_1);
	ui_rect last = list_row_rect(RESULTS_PER_PAGE - 1);
	assert(last.y + last.h == hints.y);
	assert(list_row_rect(0).x == SP_2 &&
	       list_row_rect(0).w == SCREEN_BOTTOM_W - 2 * SP_2);
	/* Pressed rows move PRESS_SHIFT right and stay on screen. */
	assert(last.x + last.w + PRESS_SHIFT <= SCREEN_BOTTOM_W);

	/* 5.3: a 36 px thumbnail SP_1 from the row's edges, text SP_2 after. */
	ui_rect row = list_row_rect(1);
	ui_rect thumb = list_thumb_rect(row);
	assert(thumb.w == THUMB_SIZE && thumb.h == THUMB_SIZE);
	assert(thumb.x == row.x + SP_1 && thumb.y == row.y + SP_1);
	assert(thumb.y + thumb.h + SP_1 == row.y + row.h);
	assert(list_text_x(row) == thumb.x + thumb.w + SP_2);

	/* Hit testing: rows only, offset by the scroll, not past the end. */
	ui_rect r0 = list_row_rect(0);
	assert(list_hit_row(r0.x + 1, r0.y + 1, 0, 24) == 0);
	assert(list_hit_row(r0.x + 1, r0.y + 1, 7, 24) == 7);
	assert(list_hit_row(r0.x + 1, r0.y + r0.h, 0, 24) == -1);  /* gap */
	assert(list_hit_row(r0.x + 1, last.y + 1, 0, 24) == 3);
	assert(list_hit_row(r0.x + 1, last.y + 1, 0, 3) == -1);
	assert(list_hit_row(r0.x + 1, 5, 0, 24) == -1);            /* header */
	assert(list_hit_row(r0.x + 1, 230, 0, 24) == -1);          /* bar */
	assert(list_hit_row(2, r0.y + 1, 0, 24) == -1);            /* margin */

	/* Empty and error states: notice, then a 32 px button SP_4 below,
	 * clear of the bar with its shadow. */
	ui_rect notice = list_notice_rect();
	ui_rect button = list_action_rect(100, false);
	ui_rect centred = list_action_rect(100, true);
	assert(notice.y == BAR_H + SP_4);
	assert(button.y == notice.y + notice.h + SP_4);
	assert(button.h == TOUCH_MIN && button.w == 100 + 2 * SP_3);
	assert(button.x == SP_2);
	assert(centred.x * 2 + centred.w == SCREEN_BOTTOM_W);
	assert(button.y + button.h + SHADOW_OFF + SP_1 <= hints.y);
	assert(list_action_rect(1000, false).w == notice.w);

	/* Durations: never cut, never negative. */
	char text[16];
	list_format_duration(320000, text, sizeof(text));
	assert(strcmp(text, "5:20") == 0);
	list_format_duration(9000, text, sizeof(text));
	assert(strcmp(text, "0:09") == 0);
	list_format_duration(-5, text, sizeof(text));
	assert(strcmp(text, "0:00") == 0);
	list_format_duration(3725000, text, sizeof(text));
	assert(strcmp(text, "62:05") == 0);
}

int main(void)
{
	test_list_logic();
	test_layout_rects();
	test_press_timing();
	test_touch_tracking();
	test_theme_colors();
	test_theme_contrast();
	test_text_fit();
	test_home_layout();
	test_home_focus();
	test_image_fit();
	test_image_morton_offset();
	test_image_to_texture();
	test_search_path_encoding();
	test_queue_boundaries();
	test_playback_cancel_transitions();
	test_language_tables();
	test_language_switching();
	return 0;
}
