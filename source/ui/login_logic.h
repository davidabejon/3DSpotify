/* The screens before the session is up: connecting, pairing and the start-up
 * error (feature_plan.md, phases 2 and 4). Layout, hit testing, the QR's URL
 * and scale, and the step counter. Pure C with no 3DS headers, tested in
 * tests/native_logic_test.c. Every measure comes from theme.h (STYLES.md 4),
 * in whole pixels; text heights are measured at draw time.
 *
 * Top screen, 400 x 240:
 *   connecting / error   logo centred on LOGIN_LOGO_CY, and below it the
 *                        spinner (or the error icon) on LOGIN_STATUS_CY
 *   pairing              heading band, then one `capa` panel holding the
 *                        code, the short URL and the instructions
 * Bottom screen, 320 x 240:
 *   connecting           the stage, centred in the content area
 *   pairing              the QR on the left, its caption on the right
 *   error                a notice, then Retry and Pair side by side
 *   all                  the button bar, 216..240 */
#ifndef LOGIN_LOGIC_H
#define LOGIN_LOGIC_H

#include "lang.h"
#include "layout.h"
#include "logo.h"
#include "spinner_logic.h"
#include "theme.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef enum {
	LOGIN_CONNECTING,
	LOGIN_PAIRING,
	LOGIN_ERROR,
	/* socInit, psInit or TLS failed: there is no network to retry with,
	 * so only START (exit) and SELECT (language) remain. */
	LOGIN_FATAL
} login_phase;

enum {
	/* A QR needs 4 modules of light margin on every side (ISO 18004). */
	LOGIN_QR_QUIET = 4,
	/* 37 modules x 4 px: a version 3 code (29 modules) and its margin. */
	LOGIN_QR_BOX = 148,
	/* Below this a phone at 20-30 cm struggles to resolve the modules. */
	LOGIN_QR_MIN_SCALE = 3,
	LOGIN_LOGO_CY = 96,
	LOGIN_STATUS_CY = 196,
	LOGIN_CONTENT_H = SCREEN_H - BAR_H,
};

/* --- The QR's URL --------------------------------------------------------- */

/* Pairing codes are short and alphanumeric; anything else is not put in a
 * URL by hand. */
static inline bool login_code_is_safe(const char *code)
{
	if (!code || !code[0])
		return false;
	for (const char *p = code; *p; p++) {
		char c = *p;
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		      (c >= '0' && c <= '9') || c == '-'))
			return false;
	}
	return true;
}

/* The URL the QR opens: Spotify's verification_uri_complete when it sent
 * one (it does: https://spotify.com/pair?code=XXXXXX), else
 * verification_uri plus ?code=<user_code>. Never the device code, which is
 * secret. False, with `out` empty, when there is nothing usable or it does
 * not fit. */
static inline bool login_qr_url(const char *uri, const char *uri_complete,
                                const char *user_code, char *out,
                                size_t size)
{
	if (!out || size == 0)
		return false;
	out[0] = '\0';
	const char *pick = NULL;
	const char *join = "";
	const char *code = "";
	if (uri_complete && uri_complete[0]) {
		pick = uri_complete;
	} else if (uri && uri[0] && login_code_is_safe(user_code)) {
		pick = uri;
		join = strchr(uri, '?') ? "&code=" : "?code=";
		code = user_code;
	} else {
		return false;
	}
	size_t need = strlen(pick) + strlen(join) + strlen(code);
	if (need >= size)
		return false;
	memcpy(out, pick, strlen(pick));
	memcpy(out + strlen(pick), join, strlen(join));
	memcpy(out + strlen(pick) + strlen(join), code, strlen(code));
	out[need] = '\0';
	return true;
}

/* The URL as people type it: no scheme, no "www.", no trailing slash
 * (https://www.spotify.com/pair/ -> spotify.com/pair). False, with `out`
 * empty, when nothing is left or it does not fit. */
static inline bool login_short_url(const char *uri, char *out, size_t size)
{
	if (!out || size == 0)
		return false;
	out[0] = '\0';
	if (!uri)
		return false;
	const char *p = uri;
	if (strncmp(p, "https://", 8) == 0)
		p += 8;
	else if (strncmp(p, "http://", 7) == 0)
		p += 7;
	if (strncmp(p, "www.", 4) == 0)
		p += 4;
	size_t len = strlen(p);
	while (len > 0 && p[len - 1] == '/')
		len--;
	if (len == 0 || len >= size)
		return false;
	memcpy(out, p, len);
	out[len] = '\0';
	return true;
}

/* --- The QR's size -------------------------------------------------------- */

/* Side of a QR of `version` (1..40), in modules. */
static inline int login_qr_modules(int version)
{
	return 17 + 4 * version;
}

/* The largest whole scale at which `modules` plus the quiet zone fit in a
 * `box` px square; 0 when not even 1 px per module does. */
static inline int login_qr_scale(int modules, int box)
{
	if (modules <= 0 || box <= 0)
		return 0;
	return box / (modules + 2 * LOGIN_QR_QUIET);
}

/* The largest version that still draws at `min_scale` in `box`, for the
 * encoder's maxVersion; 0 when none does. */
static inline int login_qr_max_version(int box, int min_scale)
{
	int best = 0;
	for (int v = 1; v <= 40; v++)
		if (login_qr_scale(login_qr_modules(v), box) >= min_scale)
			best = v;
	return best;
}

/* --- Top screen ----------------------------------------------------------- */

/* The logo's largest possible box, centred on LOGIN_LOGO_CY. */
static inline ui_rect login_logo_rect(void)
{
	ui_rect r = { SCREEN_TOP_W / 2 - LOGO_MAX_W / 2,
	              LOGIN_LOGO_CY - LOGO_MAX_H / 2, LOGO_MAX_W, LOGO_MAX_H };
	return r;
}

/* The spinner, or the error icon in its place. */
static inline ui_rect login_status_rect(void)
{
	ui_rect r = { SCREEN_TOP_W / 2 - SPINNER_SIZE / 2,
	              LOGIN_STATUS_CY - SPINNER_SIZE / 2, SPINNER_SIZE,
	              SPINNER_SIZE };
	return r;
}

/* Pairing: the heading, one line centred in this band. */
static inline ui_rect login_pair_heading_rect(void)
{
	ui_rect r = { SP_3, SP_3, SCREEN_TOP_W - 2 * SP_3, BAR_H };
	return r;
}

/* Pairing: the `capa` panel below the heading, to the bottom margin. A
 * panel that is not a control: no relief, no shadow (5.1). */
static inline ui_rect login_pair_panel_rect(void)
{
	ui_rect heading = login_pair_heading_rect();
	int y = heading.y + heading.h + SP_2;
	ui_rect r = { SP_3, y, SCREEN_TOP_W - 2 * SP_3, SCREEN_H - SP_3 - y };
	return r;
}

/* --- Bottom screen -------------------------------------------------------- */

static inline ui_rect login_hints_rect(void)
{
	ui_rect r = { 0, SCREEN_H - BAR_H, SCREEN_BOTTOM_W, BAR_H };
	return r;
}

/* Connecting: the stage and the step, centred in here. */
static inline ui_rect login_content_rect(void)
{
	ui_rect r = { SP_2, SP_4, SCREEN_BOTTOM_W - 2 * SP_2,
	              LOGIN_CONTENT_H - 2 * SP_4 };
	return r;
}

/* Pairing: the QR's white square (quiet zone included), centred in the
 * content height, on the left margin. */
static inline ui_rect login_qr_rect(void)
{
	ui_rect r = { SP_2, (LOGIN_CONTENT_H - LOGIN_QR_BOX) / 2, LOGIN_QR_BOX,
	              LOGIN_QR_BOX };
	return r;
}

/* Where a `modules` wide code starts inside login_qr_rect(), centred, past
 * its quiet zone; the scale comes from login_qr_scale(). */
static inline int login_qr_origin(int box_start, int modules, int scale)
{
	return box_start + (LOGIN_QR_BOX - modules * scale) / 2;
}

/* Pairing: the caption to the right of the QR, as tall as it. */
static inline ui_rect login_pair_side_rect(void)
{
	ui_rect qr = login_qr_rect();
	int x = qr.x + qr.w + SP_3;
	ui_rect r = { x, qr.y, SCREEN_BOTTOM_W - SP_2 - x, qr.h };
	return r;
}

typedef enum {
	LOGIN_BUTTON_NONE = -1,
	LOGIN_BUTTON_RETRY,   /* primary: A, retry with the stored credential */
	LOGIN_BUTTON_PAIR,    /* secondary: Y, pair again */
	LOGIN_BUTTON_COUNT
} login_button;

/* Error: Retry and Pair side by side, SP_4 above the button bar. */
static inline ui_rect login_button_rect(int button)
{
	int w = (SCREEN_BOTTOM_W - 2 * SP_2 - SP_4) / 2;
	ui_rect r = { SP_2 + button * (w + SP_4),
	              LOGIN_CONTENT_H - SP_4 - TOUCH_MIN, w, TOUCH_MIN };
	return r;
}

/* Error: the coral notice's largest box. Above the buttons, or down to the
 * button bar on a fatal error, which has none. The notice shrinks to its
 * text; this is how far it may grow. */
static inline ui_rect login_notice_rect(bool with_buttons)
{
	int bottom = with_buttons ? login_button_rect(0).y - SP_4
	                          : LOGIN_CONTENT_H - SP_4;
	ui_rect r = { SP_2, SP_2, SCREEN_BOTTOM_W - 2 * SP_2,
	              bottom - SHADOW_OFF - SP_2 };
	return r;
}

/* The button under (x, y), or LOGIN_BUTTON_NONE. */
static inline login_button login_hit_test(int x, int y)
{
	for (int i = 0; i < LOGIN_BUTTON_COUNT; i++)
		if (ui_rect_contains(login_button_rect(i), x, y))
			return (login_button)i;
	return LOGIN_BUTTON_NONE;
}

/* D-pad focus: `delta` -1 is left, +1 right, stopping at the ends.
 * Anything that is not a button falls back to Retry. */
static inline login_button login_focus_move(login_button focus, int delta)
{
	int next = (int)focus;
	if (next < 0 || next >= LOGIN_BUTTON_COUNT)
		next = 0;
	next += delta;
	if (next < 0)
		next = 0;
	if (next >= LOGIN_BUTTON_COUNT)
		next = LOGIN_BUTTON_COUNT - 1;
	return (login_button)next;
}

/* How tall the notice gets for a body `body_w` px wide at one line, wrapped
 * at `wrap_w` and `line_h` px per line, capped at `max_h`. One line more
 * than the division gives, because words wrap early. As ui_notice() lays
 * it out: SP_3, the title row (ICON_S), SP_2, the body, SP_3, plus the
 * bevel's bands. */
static inline int login_notice_height(int body_w, int wrap_w, int line_h,
                                      int max_h)
{
	int lines = 1;
	if (wrap_w > 0 && body_w > 0)
		lines = (body_w + wrap_w - 1) / wrap_w + 1;
	int h = OUTLINE_W + SP_3 + ICON_S + SP_2 + lines * line_h + SP_3 +
	        OUTLINE_W;
	return h < max_h ? h : max_h;
}

/* The button bar (5.6) on a narrow screen: drops labels from the last
 * action backwards until the bar fits `width` px, leaving their glyphs.
 * Each action takes glyph + SP_1 + label + SP_3, or glyph + SP_3 alone,
 * after SP_2 of margin; the last gap does not count. False when not even
 * the glyphs fit. */
static inline bool login_hints_fit(const int *glyph_w, const int *label_w,
                                   int count, int width, bool *show_label)
{
	for (int i = 0; i < count; i++)
		show_label[i] = true;
	for (int hidden = 0; hidden <= count; hidden++) {
		if (hidden > 0)
			show_label[count - hidden] = false;
		int x = SP_2;
		for (int i = 0; i < count; i++) {
			x += glyph_w[i] + SP_3;
			if (show_label[i])
				x += SP_1 + label_w[i];
		}
		if (count == 0 || x - SP_3 <= width - SP_2)
			return true;
	}
	return false;
}

/* --- Step n of N ---------------------------------------------------------- */

/* How many steps the start-up has: getting a credential (renewing it, or
 * asking for a pairing code and then the user linking the account), then
 * the access point (finding one, logging in), the library and the server. */
static inline int login_step_total(bool paired)
{
	return paired ? 6 : 5;
}

/* Which step `stage` is, 1-based; 0 for a stage that is not a step (the
 * initial "connecting"), which shows no counter. */
static inline int login_step(str_id stage, bool paired)
{
	int extra = paired ? 1 : 0;
	switch (stage) {
	case STR_CREDENTIAL_RENEWING:
	case STR_PAIR_REQUESTING:
		return 1;
	case STR_AP_SEARCHING:
		return 2 + extra;
	case STR_AP_AUTHENTICATING:
		return 3 + extra;
	case STR_LIBRARY_OPENING:
		return 4 + extra;
	case STR_SERVER_RESOLVING:
		return 5 + extra;
	default:
		return 0;
	}
}

#endif
