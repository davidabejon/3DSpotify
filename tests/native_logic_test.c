#include "art_url.h"
#include "home_logic.h"
#include "image_util.h"
#include "lang.h"
#include "layout.h"
#include "list_logic.h"
#include "now_layout.h"
#include "player_logic.h"
#include "press_logic.h"
#include "search_path.h"
#include "spinner_logic.h"
#include "settings_logic.h"
#include "tabs_logic.h"
#include "text_fit.h"
#include "thumb_queue.h"
#include "theme.h"
#include "top_logic.h"

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
	assert(strcmp(tr(STR_PLAYER_FINISHED_TITLE), "Pista terminada") == 0);
	lang_set(lang_next(lang_get()));
	assert(strcmp(tr(STR_PLAYER_FINISHED_TITLE), "Track finished") == 0);
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

static void test_image_pow2(void)
{
	assert(image_pow2(0) == 8);
	assert(image_pow2(1) == 8);
	assert(image_pow2(8) == 8);
	assert(image_pow2(9) == 16);
	assert(image_pow2(128) == 128);
	assert(image_pow2(129) == 256);
	assert(image_pow2(1024) == 1024);
	assert(image_pow2(1025) == 0);
}

static void test_image_copy_to_texture(void)
{
	/* A 10x3 image in a 16x8 texture: every pixel lands unfiltered at its
	 * Morton offset, as A, B, G, R. */
	enum { W = 10, H = 3, TW = 16, TH = 8 };
	uint8_t src[W * H * 4];
	for (int i = 0; i < W * H; i++) {
		src[i * 4] = (uint8_t)i;           /* R carries the index */
		src[i * 4 + 1] = (uint8_t)(i + 1); /* G */
		src[i * 4 + 2] = (uint8_t)(i + 2); /* B */
		src[i * 4 + 3] = (i & 1) ? 255 : 0;
	}
	uint8_t tex[TW * TH * 4];
	memset(tex, 0xEE, sizeof(tex));
	image_copy_to_texture(src, W, H, tex, TW, TH);
	for (int y = 0; y < TH; y++)
		for (int x = 0; x < TW; x++) {
			uint32_t at = image_morton_offset((uint32_t)x, (uint32_t)y,
			                                  TW) * 4;
			if (x < W && y < H) {
				int i = y * W + x;
				assert(tex[at] == ((i & 1) ? 255 : 0));
				assert(tex[at + 1] == (uint8_t)(i + 2));
				assert(tex[at + 2] == (uint8_t)(i + 1));
				assert(tex[at + 3] == (uint8_t)i);
			} else {
				/* Nothing outside w x h is written. */
				assert(tex[at] == 0xEE && tex[at + 1] == 0xEE &&
				       tex[at + 2] == 0xEE && tex[at + 3] == 0xEE);
			}
		}

	/* Images that do not fit, bad widths and NULLs leave it alone. */
	memset(tex, 0x11, sizeof(tex));
	image_copy_to_texture(src, TW + 1, 1, tex, TW, TH);
	image_copy_to_texture(src, 1, TH + 1, tex, TW, TH);
	image_copy_to_texture(src, 1, 1, tex, 12, TH);
	image_copy_to_texture(NULL, 1, 1, tex, TW, TH);
	image_copy_to_texture(src, 0, 1, tex, TW, TH);
	for (size_t i = 0; i < sizeof(tex); i++)
		assert(tex[i] == 0x11);
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
	ui_rect tabs = tabs_bar_rect();
	ui_rect hints = home_hints_rect();

	/* The tab bar on top, the button bar at the bottom; 8 px side margins
	 * (STYLES.md 4.2). */
	assert(tabs.y == 0 && tabs.h == TAB_H);
	assert(hints.h == BAR_H && hints.y + hints.h == SCREEN_H);
	assert(search.x == SP_2 && repeat.x == SP_2);
	/* Blocks SP_4 apart, starting SP_4 under the tab bar. */
	assert(search.y == tabs.h + SP_4);
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

	/* 7.4: four rows of ROW_H, SP_1 apart and SP_1 from both bars, fill
	 * the 184 px between the tab bar and the button bar exactly. */
	ui_rect hints = list_hints_rect();
	assert(list_top() == TAB_H && hints.h == BAR_H);
	assert(4 * ROW_H + 5 * SP_1 == SCREEN_H - TAB_H - BAR_H);
	assert(list_row_rect(0).y == list_top() + SP_1);
	for (int slot = 1; slot < RESULTS_PER_PAGE; slot++)
		assert(list_row_rect(slot).y ==
		       list_row_rect(slot - 1).y + ROW_H + SP_1);
	ui_rect last = list_row_rect(RESULTS_PER_PAGE - 1);
	/* The same gap over the button bar as under the tab bar. */
	assert(last.y + last.h + SP_1 == hints.y);
	assert(list_row_rect(0).y - list_top() == hints.y - (last.y + last.h));
	assert(list_row_rect(0).x == SP_2 &&
	       list_row_rect(0).w == SCREEN_BOTTOM_W - 2 * SP_2);
	/* Pressed rows move PRESS_SHIFT right and stay on screen. */
	assert(last.x + last.w + PRESS_SHIFT <= SCREEN_BOTTOM_W);

	/* 5.3: a thumbnail SP_1 from the row's edges, text SP_2 after. */
	ui_rect row = list_row_rect(1);
	ui_rect thumb = list_thumb_rect(row);
	assert(thumb.w == THUMB_SIZE && thumb.h == THUMB_SIZE);
	assert(thumb.x == row.x + SP_1 && thumb.y == row.y + SP_1);
	assert(thumb.y + thumb.h + SP_1 == row.y + row.h);
	assert(list_text_x(row) == thumb.x + thumb.w + SP_2);

	/* Hit testing: rows only, offset by the scroll, not past the end. */
	ui_rect r0 = list_row_rect(0);
	assert(list_hit_row(r0.x + 1, r0.y + 1, 0, 24) == 0);
	assert(list_hit_row(r0.x + 1, r0.y + 1, 7 * LIST_PITCH, 24) == 7);
	assert(list_hit_row(r0.x + 1, r0.y + r0.h, 0, 24) == -1);  /* gap */
	assert(list_hit_row(r0.x + 1, last.y + 1, 0, 24) == 3);
	assert(list_hit_row(r0.x + 1, last.y + 1, 0, 3) == -1);
	assert(list_hit_row(r0.x + 1, 5, 0, 24) == -1);            /* tabs */
	assert(list_hit_row(r0.x + 1, 230, 0, 24) == -1);          /* bar */
	assert(list_hit_row(2, r0.y + 1, 0, 24) == -1);            /* margin */

	/* Empty and error states: notice, then a 32 px button SP_4 below,
	 * clear of the bar with its shadow. */
	ui_rect notice = list_notice_rect();
	ui_rect button = list_action_rect(100, false);
	ui_rect centred = list_action_rect(100, true);
	assert(notice.y == list_top() + SP_4);
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

static void test_art_url(void)
{
	assert(art_url_allowed("https://i.scdn.co/image/ab67616d00004851"));
	assert(strcmp(art_url_path("https://i.scdn.co/image/abc"),
	              "/image/abc") == 0);
	/* Other hosts, schemes, and anything that could escape the path. */
	assert(!art_url_allowed("http://i.scdn.co/image/abc"));
	assert(!art_url_allowed("https://i.scdn.co.evil.com/image/abc"));
	assert(!art_url_allowed("https://evil.com/i.scdn.co/abc"));
	assert(!art_url_allowed("https://i.scdn.co/"));
	assert(!art_url_allowed("https://i.scdn.co/a b"));
	assert(!art_url_allowed("https://i.scdn.co/a\r\nHost: x"));
	assert(!art_url_allowed("https://i.scdn.co/a\\b"));
	assert(!art_url_allowed("https://i.scdn.co/a#b"));
	assert(!art_url_allowed("https://i.scdn.co/\xc3\xa1"));
	assert(!art_url_allowed(""));
	assert(!art_url_allowed(NULL));
	assert(art_url_path("https://evil.com/x") == NULL);
}

static void test_thumb_queue(void)
{
	thumb_state s[24];
	for (int i = 0; i < 24; i++)
		s[i] = THUMB_WAITING;
	/* Visible rows first, in order. */
	assert(thumb_pick(s, 24, 0, 4) == 0);
	s[0] = s[1] = THUMB_READY;
	s[2] = THUMB_LOADING;
	s[3] = THUMB_NONE;
	/* Then the next page. */
	assert(thumb_pick(s, 24, 0, 4) == 4);
	for (int i = 4; i < 8; i++)
		s[i] = THUMB_FAILED;
	/* Nothing beyond the next page until the list scrolls. */
	assert(thumb_pick(s, 24, 0, 4) == -1);
	assert(thumb_pick(s, 24, 5, 4) == 8);
	/* Scrolling back up picks what was skipped there first. */
	s[0] = THUMB_WAITING;
	assert(thumb_pick(s, 24, 0, 4) == 0);
	/* The window is cut at the end of the list. */
	assert(thumb_pick(s, 10, 8, 4) == 8);
	assert(thumb_pick(s, 0, 0, 4) == -1);
	assert(thumb_pick(s, 24, -3, 4) == 0);

	/* What survives a new list at the same position. */
	assert(thumb_keep(THUMB_READY, true));
	assert(thumb_keep(THUMB_NONE, true));
	assert(thumb_keep(THUMB_DECODED, true));
	assert(!thumb_keep(THUMB_READY, false));
	assert(!thumb_keep(THUMB_FAILED, true));   /* try again */
	assert(!thumb_keep(THUMB_WAITING, true));
	assert(!thumb_keep(THUMB_LOADING, true));
}

static void test_list_drag(void)
{
	/* Pixel geometry: one row per LIST_PITCH. */
	assert(LIST_PITCH == ROW_H + SP_1);
	assert(list_max_scroll(24) == 20 * LIST_PITCH);
	assert(list_max_scroll(4) == 0 && list_max_scroll(0) == 0);
	assert(list_row_at(0, 0).y == list_row_rect(0).y);
	assert(list_row_at(3, 0).y == list_row_rect(3).y);
	assert(list_row_at(5, 2 * LIST_PITCH).y == list_row_rect(3).y);
	assert(list_row_at(1, 10).y == list_row_rect(1).y - 10);

	/* Hit testing follows the scroll, rows only, inside the list area. */
	ui_rect r0 = list_row_rect(0);
	assert(list_hit_row(r0.x + 1, r0.y + 1, 7 * LIST_PITCH, 24) == 7);
	assert(list_hit_row(r0.x + 1, r0.y + 1, 10, 24) == 0);
	/* Scrolled 20 px: row 0's top part is under the tab bar. */
	assert(list_hit_row(r0.x + 1, list_top() - 1, 20, 24) == -1);
	assert(list_hit_row(r0.x + 1, list_top(), 20, 24) == 0);
	/* A fifth row peeking in at the bottom is hit; the bar is not. */
	assert(list_hit_row(r0.x + 1, SCREEN_H - BAR_H - 1, 20, 24) == 4);
	assert(list_hit_row(r0.x + 1, SCREEN_H - BAR_H, 20, 24) == -1);

	/* Snapping to the nearest whole row, within the ends. */
	assert(list_snap(0, 24) == 0);
	/* Halfway or more rounds to the next row, whatever the pitch's
	 * parity. */
	assert(list_snap((LIST_PITCH - 1) / 2, 24) == 0);
	assert(list_snap((LIST_PITCH + 1) / 2, 24) == 1);
	assert(list_snap(3 * LIST_PITCH + 30, 24) == 4);
	assert(list_snap(1000000, 24) == 20);
	assert(list_snap(-30, 24) == 0);
	assert(list_snap(100, 3) == 0);

	/* Whole rows on screen: 4 when resting on a row, 3 when cut at both
	 * ends. */
	int from, to;
	list_full_rows(0, 24, &from, &to);
	assert(from == 0 && to == 3);
	list_full_rows(5 * LIST_PITCH, 24, &from, &to);
	assert(from == 5 && to == 8);
	list_full_rows(20, 24, &from, &to);
	assert(from == 1 && to == 3);
	list_full_rows(SP_1, 24, &from, &to);       /* row 0 touches the tabs */
	assert(from == 0 && to == 3);
	list_full_rows(SP_1 + 1, 24, &from, &to);   /* now it is cut */
	assert(from == 1 && to == 3);
	list_full_rows(0, 2, &from, &to);
	assert(from == 0 && to == 1);
	/* Every whole row really is whole, and the next ones are not. */
	for (int s = 0; s <= list_max_scroll(24); s++) {
		list_full_rows(s, 24, &from, &to);
		assert(to - from + 1 >= RESULTS_PER_PAGE - 1);
		for (int i = from; i <= to; i++) {
			ui_rect r = list_row_at(i, s);
			assert(r.y >= list_top() && r.y + r.h <= SCREEN_H - BAR_H);
		}
		ui_rect before = list_row_at(from - 1, s);
		ui_rect after = list_row_at(to + 1, s);
		assert(from == 0 || before.y < list_top());
		assert(after.y + after.h > SCREEN_H - BAR_H);
	}

	/* Whether the focused row is whole: when it is not (cut or dragged
	 * out of view), the first button press only brings it back. */
	assert(list_row_whole(2, 20, 24));
	assert(!list_row_whole(0, 20, 24));          /* cut by the tabs */
	assert(!list_row_whole(4, 20, 24));          /* peeking in */
	assert(!list_row_whole(12, 5 * LIST_PITCH, 24));
	assert(list_row_whole(8, 5 * LIST_PITCH, 24));
	assert(list_row_whole(2, 0, 3));
	assert(!list_row_whole(0, 0, 0));
	/* Aligning on a hidden focus shows it whole. */
	for (int s = 0; s <= list_max_scroll(24); s += 7)
		for (int sel = 0; sel < 24; sel++)
			assert(list_row_whole(sel, list_align(s, sel, 24) * LIST_PITCH,
			                      24));

	/* Buttons bring a cut list back to whole rows, moving as little as
	 * possible while keeping the focus in view (6.6). */
	assert(list_align(20, 2, 24) == 0);
	assert(list_align(30, 2, 24) == 1);
	assert(list_align(30, 5, 24) == 2);
	assert(list_align(30, 0, 24) == 0);
	assert(list_align(5 * LIST_PITCH, 6, 24) == 5);
	assert(list_align(list_max_scroll(24) - 10, 23, 24) == 20);

	/* A touch becomes a drag after DRAG_START px, either way. */
	list_drag d;
	list_drag_begin(&d, 100, 0);
	assert(!list_drag_update(&d, 100 + DRAG_START - 1));
	assert(!list_drag_update(&d, 100 - DRAG_START + 1));
	assert(list_drag_update(&d, 100 - DRAG_START));
	/* Once dragging it stays a drag, even back at the start. */
	assert(list_drag_update(&d, 100));
	/* The list follows the stylus 1:1: moving up scrolls down. */
	list_drag_begin(&d, 150, 2 * LIST_PITCH);
	assert(list_drag_scroll(&d, 140, 24) == 2 * LIST_PITCH + 10);
	assert(list_drag_scroll(&d, 170, 24) == 2 * LIST_PITCH - 20);
	/* ... and stops at both ends. */
	assert(list_drag_scroll(&d, 150 + 1000, 24) == 0);
	assert(list_drag_scroll(&d, 150 - 5000, 24) == list_max_scroll(24));
	assert(list_drag_scroll(&d, 100, 3) == 0);
	/* Not touching: no drag. */
	d.down = false;
	d.dragging = false;
	assert(!list_drag_update(&d, 0));

	/* 5.11: the indicator against the right edge, between the bars. */
	ui_rect track = list_scroll_track();
	assert(track.x + track.w == SCREEN_BOTTOM_W && track.w == SCROLL_W);
	assert(track.y == list_top() && track.y + track.h == SCREEN_H - BAR_H);
	/* Drags only start inside the list area, never on the tab bar. */
	assert(!list_area_contains(10, TAB_H - 1));
	assert(list_area_contains(10, TAB_H));
	assert(!list_area_contains(10, SCREEN_H - BAR_H));
	/* Clear of the rows, pressed ones included. */
	assert(r0.x + r0.w + PRESS_SHIFT < track.x);
	/* Thumb: visible share of the list, top and bottom at the ends. */
	ui_rect top = list_scroll_thumb(0, 24);
	ui_rect bottom = list_scroll_thumb(list_max_scroll(24), 24);
	assert(top.y == track.y && top.h == track.h * 4 / 24);
	assert(bottom.y + bottom.h == track.y + track.h);
	ui_rect middle = list_scroll_thumb(list_max_scroll(24) / 2, 24);
	assert(middle.y > top.y && middle.y < bottom.y);
	/* Never shorter than SCROLL_MIN_H, never outside the track. */
	ui_rect tiny = list_scroll_thumb(list_max_scroll(1000), 1000);
	assert(tiny.h == SCROLL_MIN_H);
	assert(tiny.y + tiny.h == track.y + track.h);
	assert(list_scroll_thumb(-50, 24).y == track.y);
	assert(list_scroll_thumb(1000000, 24).y == bottom.y);
}

static void test_tabs(void)
{
	/* 5.12: four tabs across the top, at least the minimum touch target
	 * high, together exactly as wide as the screen. */
	ui_rect bar = tabs_bar_rect();
	assert(bar.x == 0 && bar.y == 0 && bar.w == SCREEN_BOTTOM_W);
	assert(bar.h == TAB_H && TAB_H >= TOUCH_MIN);
	int x = 0;
	for (int t = 0; t < TAB_COUNT; t++) {
		ui_rect r = tabs_tab_rect(t);
		assert(r.x == x && r.y == 0 && r.h == TAB_H);
		assert(r.w >= TOUCH_MIN && r.w >= ICON_S + 2 * SP_3);
		x += r.w;
		/* Hit testing matches what is drawn, edges included. */
		assert(tabs_hit(r.x, r.y) == t);
		assert(tabs_hit(r.x + r.w - 1, r.y + r.h - 1) == t);
	}
	assert(x == SCREEN_BOTTOM_W);
	/* Outside the bar: nothing. */
	assert(tabs_hit(10, TAB_H) == -1 && tabs_hit(10, 230) == -1);
	assert(tabs_hit(-1, 5) == -1 && tabs_hit(SCREEN_BOTTOM_W, 5) == -1);
	/* Active and pressed: only the bottom edge rises, 2 and 4 px. */
	assert(TAB_LIFT_PRESSED > TAB_LIFT_ACTIVE && TAB_LIFT_ACTIVE > 0);
	for (int t = 0; t < TAB_COUNT; t++) {
		ui_rect full = tabs_tab_rect(t);
		ui_rect rest = tabs_tab_face(t, TAB_FACE_REST);
		ui_rect on = tabs_tab_face(t, TAB_FACE_ACTIVE);
		ui_rect down = tabs_tab_face(t, TAB_FACE_PRESSED);
		assert(rest.x == full.x && rest.y == full.y && rest.w == full.w &&
		       rest.h == full.h);
		assert(on.x == full.x && on.y == full.y && on.w == full.w);
		assert(down.x == full.x && down.y == full.y && down.w == full.w);
		assert(on.h == full.h - TAB_LIFT_ACTIVE);
		assert(down.h == full.h - TAB_LIFT_PRESSED);
		assert(down.h >= ICON_S + 2 * SP_1);
	}
	/* The last tab ends at the screen's edge. */
	assert(tabs_tab_rect(TAB_SETTINGS).x + tabs_tab_rect(TAB_SETTINGS).w ==
	       SCREEN_BOTTOM_W);

	/* Stepping stops at both ends (no wrap, like the lists). */
	assert(tabs_step(TAB_HOME, 1) == TAB_SEARCH);
	assert(tabs_step(TAB_SEARCH, -1) == TAB_HOME);
	assert(tabs_step(TAB_HOME, -1) == -1);
	assert(tabs_step(TAB_SETTINGS, 1) == -1);
	assert(tabs_step(TAB_PLAYER, 0) == -1);
	assert(tabs_step(-1, 1) == -1 && tabs_step(TAB_COUNT, -1) == -1);

	/* L/R and ZL/ZR always switch tabs; both ways at once cancel out. */
	assert(tabs_key_delta(false, true, false, false) == 1);
	assert(tabs_key_delta(true, false, false, false) == -1);
	assert(tabs_key_delta(false, false, false, true) == 1);
	assert(tabs_key_delta(false, false, true, false) == -1);
	assert(tabs_key_delta(true, true, false, false) == 0);
	assert(tabs_key_delta(true, false, false, true) == 0);
	assert(tabs_key_delta(false, false, false, false) == 0);
}

static void test_settings_layout(void)
{
	ui_rect hints = settings_hints_rect();
	assert(hints.y + hints.h == SCREEN_H && hints.h == BAR_H);
	int ring = FOCUS_GAP + FOCUS_W;
	int below = ring > SHADOW_OFF ? ring : SHADOW_OFF;
	for (int i = 0; i < SETTINGS_ITEM_COUNT; i++) {
		ui_rect r = settings_item_rect(i);
		/* Full content width, 32 px high (4.4, 5.2). */
		assert(r.x == SP_2 && r.w == SCREEN_BOTTOM_W - 2 * SP_2);
		assert(r.h == TOUCH_MIN);
		assert(r.x + r.w + PRESS_SHIFT <= SCREEN_BOTTOM_W);
		if (i == 0)
			assert(r.y == TAB_H + SP_4);
		else {
			ui_rect prev = settings_item_rect(i - 1);
			/* Focus borders and shadows stay SP_1 apart at least. */
			assert((r.y - ring) - (prev.y + prev.h + below) >= SP_1);
		}
		assert(settings_hit_test(r.x, r.y) == (settings_item)i);
		assert(settings_hit_test(r.x + r.w - 1, r.y + r.h - 1) ==
		       (settings_item)i);
	}
	ui_rect last = settings_item_rect(SETTINGS_ITEM_COUNT - 1);
	assert(last.y + last.h + below + PRESS_SHIFT <= hints.y);
	/* The tab bar, the gaps and the bars are not buttons. */
	assert(settings_hit_test(10, 10) == SETTINGS_ITEM_NONE);
	assert(settings_hit_test(10, TAB_H + SP_4 - 1) == SETTINGS_ITEM_NONE);
	assert(settings_hit_test(10, 230) == SETTINGS_ITEM_NONE);

	/* Focus stops at the ends; anything else falls back to the first. */
	assert(settings_focus_move(SETTINGS_ITEM_LANGUAGE, 1) ==
	       SETTINGS_ITEM_RECONNECT);
	assert(settings_focus_move(SETTINGS_ITEM_PAIR, 1) == SETTINGS_ITEM_PAIR);
	assert(settings_focus_move(SETTINGS_ITEM_LANGUAGE, -1) ==
	       SETTINGS_ITEM_LANGUAGE);
	assert(settings_focus_move(SETTINGS_ITEM_NONE, 1) ==
	       SETTINGS_ITEM_RECONNECT);
}

static void test_player_layout(void)
{
	/* 5.2: a 48 px primary between two 40 px secondaries, centred,
	 * SP_4 apart, under the tab bar and over the bars. */
	ui_rect prev = player_control_rect(PLAYER_CONTROL_PREV);
	ui_rect play = player_control_rect(PLAYER_CONTROL_PLAY);
	ui_rect next = player_control_rect(PLAYER_CONTROL_NEXT);
	assert(play.w == BTN_PLAY && play.h == BTN_PLAY);
	assert(prev.w == BTN_SKIP && next.h == BTN_SKIP);
	assert(play.x * 2 + play.w == SCREEN_BOTTOM_W);
	assert(play.x - (prev.x + prev.w) == SP_4);
	assert(next.x - (play.x + play.w) == SP_4);
	assert(prev.y + prev.h / 2 == play.y + play.h / 2);
	assert(play.y > TAB_H + SP_4);
	assert(player_hit_test(play.x, play.y) == PLAYER_CONTROL_PLAY);
	assert(player_hit_test(prev.x + 1, prev.y + 1) == PLAYER_CONTROL_PREV);
	assert(player_hit_test(next.x + next.w - 1, next.y + 1) ==
	       PLAYER_CONTROL_NEXT);
	assert(player_hit_test(prev.x + prev.w + 1, prev.y + 1) ==
	       PLAYER_CONTROL_NONE);
	assert(player_hit_test(10, 10) == PLAYER_CONTROL_NONE);

	/* 5.4, 5.5: the bars, below the controls and their shadow, clear of
	 * each other and of the button bar, the handle included. */
	ui_rect progress = player_progress_rect();
	ui_rect volume = player_volume_rect(40);
	assert(progress.h == PROGRESS_H && volume.h == VOLUME_H);
	assert(progress.x == SP_2 && progress.w == SCREEN_BOTTOM_W - 2 * SP_2);
	assert(play.y + play.h + SHADOW_OFF < player_times_cy() - SP_2);
	int handle_top = progress.y + progress.h / 2 - HANDLE_H / 2;
	assert(handle_top > player_times_cy());
	assert(volume.y > handle_top + HANDLE_H);
	assert(volume.x == SP_2 + 40 + SP_2);
	assert(volume.x + volume.w == SCREEN_BOTTOM_W - SP_2);
	assert(volume.y + volume.h + SP_2 <= player_hints_rect().y);
	ui_rect notice = player_notice_rect();
	assert(notice.y == TAB_H + SP_4);
	assert(notice.y + notice.h + SHADOW_OFF < player_hints_rect().y);

	/* Fractions. */
	assert(player_permille(0, 0) == 0);
	assert(player_permille(30, 60) == 500);
	assert(player_permille(90, 60) == 1000);
	assert(player_permille(4000000000u, 4000000000u) == 1000);
	assert(player_permille(1, 3) == 333);
	assert(player_fill_px(300, 500) == 150);
	assert(player_fill_px(300, 0) == 0 && player_fill_px(300, -5) == 0);
	assert(player_fill_px(300, 1000) == 300 && player_fill_px(300, 2000) == 300);
	assert(player_fill_px(0, 500) == 0);
}

static void test_player_shared_helpers(void)
{
	char text[16];
	player_format_time(0, text, sizeof(text));
	assert(strcmp(text, "0:00") == 0);
	player_format_time(84, text, sizeof(text));
	assert(strcmp(text, "1:24") == 0);
	player_format_time(3601, text, sizeof(text));
	assert(strcmp(text, "60:01") == 0);

	assert(player_progress_permille(true, 0, 0) == 1000);
	assert(player_progress_permille(false, 30, 60) == 500);
	assert(player_progress_permille(false, 5, 0) == 0);

	/* The download follows every tick, else once a second. */
	player_download_sample sample = { 0, 0 };
	player_sample_download(&sample, true, 10);
	assert(sample.shown == 10);
	for (int i = 1; i < PLAYER_DOWNLOAD_FRAMES; i++) {
		player_sample_download(&sample, false, 20 + i);
		assert(sample.shown == 10);
	}
	player_sample_download(&sample, false, 99);
	assert(sample.shown == 99);
}

static bool overlaps(ui_rect a, ui_rect b)
{
	return a.x < b.x + b.w && b.x < a.x + a.w &&
	       a.y < b.y + b.h && b.y < a.y + a.h;
}

static bool on_top_screen(ui_rect r, int margin)
{
	return r.x >= margin && r.y >= margin &&
	       r.x + r.w <= SCREEN_TOP_W - margin &&
	       r.y + r.h <= SCREEN_H - margin;
}

static void test_now_layout(void)
{
	/* The system font's line feed is 30: t_titulo 23, t_cuerpo 15,
	 * t_meta 14 (ui_line_height). Also try a taller font. */
	static const int heights[][3] = { { 23, 15, 14 }, { 27, 18, 16 } };
	for (size_t k = 0; k < sizeof(heights) / sizeof(heights[0]); k++) {
		int title_h = heights[k][0], body_h = heights[k][1];
		int meta_h = heights[k][2];

		/* 7.5: 160x160 cover, e3 from the top and left edges, its 3 px
		 * shadow included on screen. */
		ui_rect art = now_art_rect();
		assert(art.x == SP_3 && art.y == SP_3);
		assert(art.w == ART_SIZE && art.h == ART_SIZE);
		ui_rect shadow = ui_rect_offset(art, ART_SHADOW_OFF,
		                                ART_SHADOW_OFF);
		assert(on_top_screen(art, SP_3) && on_top_screen(shadow, SP_3));

		/* The text column: right of the cover and its shadow, up to
		 * e3 from the right edge. */
		ui_rect text = { now_text_x(), SP_3, now_text_w(),
		                 now_album_cy(title_h, body_h) + body_h / 2 + 1 -
		                 SP_3 };
		assert(text.x == SP_3 + ART_SIZE + SP_4);
		assert(text.x + text.w == SCREEN_TOP_W - SP_3);
		assert(!overlaps(text, shadow) && on_top_screen(text, SP_3));
		int title_bottom = now_title_cy(title_h) + (title_h + 1) / 2;
		int artist_top = now_artist_cy(title_h, body_h) - body_h / 2;
		assert(artist_top - title_bottom >= SP_1 - 1);
		assert(now_album_cy(title_h, body_h) -
		       now_artist_cy(title_h, body_h) == body_h);
		assert(text.y + text.h <= art.y + art.h);

		/* Times e4 under the cover, the bar below with its handle
		 * clear of their line, everything inside e3. */
		int times_top = now_times_cy(meta_h) - meta_h / 2;
		assert(times_top == art.y + art.h + SP_4);
		assert(times_top > shadow.y + shadow.h);
		ui_rect bar = now_progress_rect(meta_h);
		assert(bar.h == PROGRESS_H);
		assert(bar.x == SP_3 && bar.x + bar.w == SCREEN_TOP_W - SP_3);
		ui_rect handle = { bar.x, bar.y + bar.h / 2 - HANDLE_H / 2,
		                   HANDLE_W, HANDLE_H };
		assert(handle.y >= times_top + meta_h);
		assert(on_top_screen(handle, SP_3) && on_top_screen(bar, SP_3));
	}

	/* The big cover: centred, e3 above and below. */
	ui_rect big = now_big_art_rect();
	assert(big.w == ART_BIG && big.h == ART_BIG);
	assert(big.x == 92 && big.y == SP_3);
	assert(big.x * 2 + big.w == SCREEN_TOP_W);
	assert(big.y + big.h == SCREEN_H - SP_3);
	ui_rect big_shadow = ui_rect_offset(big, ART_SHADOW_OFF, ART_SHADOW_OFF);
	assert(on_top_screen(big_shadow, 0));
}

static void test_list_bubbles(void)
{
	/* 5.9: centred SP_2 above the button bar; a second bubble stacks
	 * above, clear of the first one's shadow and under the tab bar. */
	ui_rect first = list_bubble_rect(100.0f);
	assert(first.h == BUBBLE_H && first.w == 100 + 2 * SP_3);
	assert(first.y + first.h + SP_2 == SCREEN_H - BAR_H);
	assert(first.x * 2 + first.w == SCREEN_BOTTOM_W);
	ui_rect same = list_bubble_slot_rect(100.0f, 0);
	assert(same.x == first.x && same.y == first.y && same.w == first.w);
	ui_rect second = list_bubble_slot_rect(60.0f, 1);
	assert(second.y + second.h + SHADOW_OFF + SP_2 == first.y);
	assert(!overlaps(ui_rect_offset(second, SHADOW_OFF, SHADOW_OFF), first));
	assert(second.y > TAB_H);
}

static void test_spinner(void)
{
	/* A 44 px ring of 8 blocks centred on the top screen: whole pixels,
	 * inside the screen, no two blocks touching, the centre empty. */
	int cx = SCREEN_TOP_W / 2, cy = SCREEN_H / 2;
	assert(SPINNER_SIZE == 44);
	ui_rect box = { cx - SPINNER_SIZE / 2, cy - SPINNER_SIZE / 2,
	                SPINNER_SIZE, SPINNER_SIZE };
	assert(box.x * 2 + box.w == SCREEN_TOP_W);
	assert(box.y * 2 + box.h == SCREEN_H);
	ui_rect centre = { box.x + SPINNER_CELL + SPINNER_GAP,
	                   box.y + SPINNER_CELL + SPINNER_GAP, SPINNER_CELL,
	                   SPINNER_CELL };
	for (int i = 0; i < SPINNER_CELLS; i++) {
		ui_rect a = spinner_cell_rect(cx, cy, i);
		assert(a.w == SPINNER_CELL && a.h == SPINNER_CELL);
		assert(a.x >= box.x && a.y >= box.y);
		assert(a.x + a.w <= box.x + box.w && a.y + a.h <= box.y + box.h);
		assert(!overlaps(a, centre));
		for (int j = i + 1; j < SPINNER_CELLS; j++)
			assert(!overlaps(a, spinner_cell_rect(cx, cy, j)));
		/* Clockwise: each block is a grid step from the previous. */
		ui_rect b = spinner_cell_rect(cx, cy, i + 1);
		int dx = b.x - a.x, dy = b.y - a.y;
		int pitch = SPINNER_CELL + SPINNER_GAP;
		assert((dx == 0) != (dy == 0));
		assert(dx == 0 || dx == pitch || dx == -pitch);
		assert(dy == 0 || dy == pitch || dy == -pitch);
	}
	/* The backdrop: SPINNER_PAD px around the ring, centred, translucent. */
	ui_rect back = spinner_backdrop_rect(cx, cy);
	assert(back.w == SPINNER_SIZE + 2 * SPINNER_PAD && back.h == back.w);
	assert(back.x == box.x - SPINNER_PAD && back.y == box.y - SPINNER_PAD);
	assert(back.x * 2 + back.w == SCREEN_TOP_W);
	assert(back.y * 2 + back.h == SCREEN_H);
	assert((SCRIM_COLOR >> 24) == SCRIM_ALPHA);
	assert(SCRIM_ALPHA > 0 && SCRIM_ALPHA < 0xFF);
	assert((SCRIM_COLOR & 0x00FFFFFFu) == (SCRIM_RGB & 0x00FFFFFFu));
	/* The top-left block, then right along the top. */
	assert(spinner_cell_rect(cx, cy, 0).x == box.x);
	assert(spinner_cell_rect(cx, cy, 1).y == box.y);
	assert(spinner_lit(0) == 0 && spinner_lit(9) == 1 &&
	       spinner_lit(-1) == SPINNER_CELLS - 1);

	/* Only while a track loads or plays with nothing downloaded. */
	assert(top_shows_spinner(PLAYER_PHASE_LOADING, 0));
	assert(top_shows_spinner(PLAYER_PHASE_PLAYING, 0));
	assert(!top_shows_spinner(PLAYER_PHASE_LOADING, 1));
	assert(!top_shows_spinner(PLAYER_PHASE_PLAYING, 100));
	assert(!top_shows_spinner(PLAYER_PHASE_IDLE, 0));
	assert(!top_shows_spinner(PLAYER_PHASE_FINISHED, 0));
	assert(!top_shows_spinner(PLAYER_PHASE_FAILED, 0));
}

static void test_top_mode(void)
{
	static const player_phase phases[] = {
		PLAYER_PHASE_IDLE, PLAYER_PHASE_LOADING, PLAYER_PHASE_PLAYING,
		PLAYER_PHASE_FINISHED, PLAYER_PHASE_FAILED
	};
	for (int tab = 0; tab < TAB_COUNT; tab++)
		for (size_t i = 0; i < sizeof(phases) / sizeof(phases[0]); i++) {
			player_phase phase = phases[i];
			bool music = phase == PLAYER_PHASE_LOADING ||
			             phase == PLAYER_PHASE_PLAYING ||
			             phase == PLAYER_PHASE_FINISHED;
			assert(top_music_on(phase) == music);
			top_mode mode = top_mode_for((app_tab)tab, phase);
			if (!music)
				assert(mode == TOP_LOGO);
			else if (tab == TAB_PLAYER)
				assert(mode == TOP_ART_BIG);
			else
				assert(mode == TOP_NOW_CARD);
		}
}

int main(void)
{
	test_tabs();
	test_settings_layout();
	test_player_layout();
	test_player_shared_helpers();
	test_now_layout();
	test_list_bubbles();
	test_spinner();
	test_top_mode();
	test_list_drag();
	test_art_url();
	test_thumb_queue();
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
	test_image_pow2();
	test_image_copy_to_texture();
	test_search_path_encoding();
	test_queue_boundaries();
	test_language_tables();
	test_language_switching();
	return 0;
}
