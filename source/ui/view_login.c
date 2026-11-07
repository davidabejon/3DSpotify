#include "view_login.h"

#include "draw.h"
#include "icons.h"
#include "login_logic.h"
#include "logo.h"
#include "press_logic.h"
#include "qrcodegen.h"
#include "theme.h"

#include <citro2d.h>

#include <stdio.h>
#include <string.h>

/* Every string of one screen, in both languages: the longest is the error
 * detail (startup_error, 256 bytes) plus the notice's title and the bar. */
#define TEXT_GLYPHS 1024
/* The code at t_titulo x 1.5 (7.3). */
#define TXT_CODE (TXT_TITULO * 1.5f)

/* The button bar (5.6). START and SELECT have no colour of their own in 2.5:
 * a light glyph, apart from the four face buttons. */
enum { GLYPH_A, GLYPH_B, GLYPH_Y, GLYPH_START, GLYPH_SELECT, GLYPH_COUNT };
static const char *const glyph_letters[GLYPH_COUNT] = {
	"A", "B", "Y", "START", "SELECT"
};
static const u32 glyph_colors[GLYPH_COUNT] = {
	COL_BTN_A, COL_BTN_B, COL_BTN_Y, COL_TEXTO_2, COL_TEXTO_2
};

/* --- What is on screen ---------------------------------------------------- */

static login_phase phase = LOGIN_CONNECTING;
static str_id stage = STR_LOGIN_CONNECTING;
static bool paired;
static bool reason_expired;
static bool offline;
static char user_code[16];
static char short_url[128];
static char qr_url[192];
static str_id category = STR_ERROR;
static char detail[256];
/* Moves on with every change above, so the text is parsed again. */
static unsigned content;

/* The QR, encoded once per URL. */
static uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
static uint8_t qr_temp[qrcodegen_BUFFER_LEN_MAX];
static char qr_encoded[192];
static bool qr_ok;

/* --- Text ----------------------------------------------------------------- */

static C2D_TextBuf text_buf;
static C2D_TextBuf scratch_buf;
static lang_id parsed_lang = LANG_COUNT;
static unsigned parsed_content = ~0u;

static C2D_Text letter_text[GLYPH_COUNT];
static C2D_Text hint_language, hint_cancel, hint_retry, hint_pair, hint_exit;
static C2D_Text stage_text, step_text;
static C2D_Text heading_text, your_code_text, code_text, url_text;
static C2D_Text instructions_text;
static C2D_Text qr_body_text, status_text;
static C2D_Text title_text, detail_text;
static C2D_Text retry_text, pair_text;

/* --- Input ---------------------------------------------------------------- */

static login_button focus = LOGIN_BUTTON_RETRY;
static press_state press_button[LOGIN_BUTTON_COUNT];
static press_state press_glyph[GLYPH_COUNT];
static touch_track touch = { -1 };
static login_action pending = LOGIN_ACTION_NONE;
static const press_state *pending_press;
/* Frames on this screen, for the spinner and the loading bars. */
static int anim_frames;

typedef struct {
	login_phase phase;
	unsigned content;
	lang_id lang;
	login_button focus;
	int step;
	bool button[LOGIN_BUTTON_COUNT];
	bool glyph[GLYPH_COUNT];
} login_look;
static login_look drawn;
static bool drawn_valid;

bool view_login_init(void)
{
	text_buf = C2D_TextBufNew(TEXT_GLYPHS);
	scratch_buf = C2D_TextBufNew(UI_FIELD_SIZE);
	return text_buf != NULL && scratch_buf != NULL;
}

void view_login_exit(void)
{
	if (text_buf)
		C2D_TextBufDelete(text_buf);
	if (scratch_buf)
		C2D_TextBufDelete(scratch_buf);
	text_buf = scratch_buf = NULL;
}

/* A new screen: nothing pressed, nothing pending, Retry focused. */
static void enter(login_phase next)
{
	if (next == phase)
		return;
	phase = next;
	for (int i = 0; i < LOGIN_BUTTON_COUNT; i++)
		press_reset(&press_button[i]);
	for (int i = 0; i < GLYPH_COUNT; i++)
		press_reset(&press_glyph[i]);
	touch.target = -1;
	pending = LOGIN_ACTION_NONE;
	pending_press = NULL;
	focus = LOGIN_BUTTON_RETRY;
	anim_frames = 0;
	content++;
}

void view_login_connecting(str_id next_stage, bool next_paired)
{
	enter(LOGIN_CONNECTING);
	if (stage == next_stage && paired == next_paired)
		return;
	stage = next_stage;
	paired = next_paired;
	content++;
}

static void encode_qr(void)
{
	if (strcmp(qr_encoded, qr_url) == 0)
		return;
	snprintf(qr_encoded, sizeof(qr_encoded), "%s", qr_url);
	/* Low correction to start with; the encoder raises it as far as the
	 * smallest version that holds the URL allows. */
	qr_ok = qr_url[0] &&
	        qrcodegen_encodeText(qr_url, qr_temp, qr, qrcodegen_Ecc_LOW,
	                             qrcodegen_VERSION_MIN,
	                             login_qr_max_version(LOGIN_QR_BOX,
	                                                  LOGIN_QR_MIN_SCALE),
	                             qrcodegen_Mask_AUTO, true);
}

void view_login_pairing(const login_pair_info *info)
{
	enter(LOGIN_PAIRING);
	char next_short[sizeof(short_url)];
	if (!login_short_url(info->verification_uri, next_short,
	                     sizeof(next_short)))
		snprintf(next_short, sizeof(next_short), "%s",
		         info->verification_uri ? info->verification_uri : "");
	const char *code = info->user_code ? info->user_code : "";
	const char *url = info->qr_url ? info->qr_url : "";
	if (reason_expired == info->reason_expired &&
	    offline == info->offline && strcmp(user_code, code) == 0 &&
	    strcmp(short_url, next_short) == 0 && strcmp(qr_url, url) == 0)
		return;
	reason_expired = info->reason_expired;
	offline = info->offline;
	snprintf(user_code, sizeof(user_code), "%s", code);
	snprintf(short_url, sizeof(short_url), "%s", next_short);
	snprintf(qr_url, sizeof(qr_url), "%s", url);
	encode_qr();
	content++;
}

void view_login_error(str_id next_category, const char *next_detail,
                      bool fatal)
{
	enter(fatal ? LOGIN_FATAL : LOGIN_ERROR);
	const char *text = next_detail ? next_detail : "";
	if (category == next_category && strcmp(detail, text) == 0)
		return;
	category = next_category;
	snprintf(detail, sizeof(detail), "%s", text);
	content++;
}

static void parse(C2D_Text *text, const char *string)
{
	C2D_TextParse(text, text_buf, string);
	C2D_TextOptimize(text);
}

static void ensure_parsed(void)
{
	if (parsed_lang == lang_get() && parsed_content == content)
		return;
	C2D_TextBufClear(text_buf);
	for (int i = 0; i < GLYPH_COUNT; i++)
		parse(&letter_text[i], glyph_letters[i]);
	/* SELECT names the language it switches to. */
	parse(&hint_language, lang_text(lang_next(lang_get()), STR_LANG_NAME));
	parse(&hint_cancel, tr(STR_HINT_CANCEL));
	parse(&hint_retry, tr(STR_HINT_RETRY));
	parse(&hint_pair, tr(STR_HINT_PAIR));
	parse(&hint_exit, tr(STR_HINT_EXIT));

	switch (phase) {
	case LOGIN_CONNECTING: {
		parse(&stage_text, tr(stage));
		char step[48] = "";
		int n = login_step(stage, paired);
		if (n > 0)
			snprintf(step, sizeof(step), tr(STR_LOGIN_STEP_FMT), n,
			         login_step_total(paired));
		parse(&step_text, step);
		break;
	}
	case LOGIN_PAIRING: {
		ui_rect heading = login_pair_heading_rect();
		ui_parse_fitted(&heading_text, text_buf, scratch_buf,
		                tr(reason_expired ? STR_PAIR_HEADING_EXPIRED
		                                  : STR_PAIR_HEADING),
		                TXT_CABEZA, heading.w);
		parse(&your_code_text, tr(STR_PAIR_YOUR_CODE));
		parse(&code_text, user_code);
		parse(&url_text, short_url);
		parse(&instructions_text, tr(STR_PAIR_INSTRUCTIONS));
		parse(&qr_body_text, tr(STR_PAIR_QR_BODY));
		parse(&status_text, tr(offline ? STR_PAIR_OFFLINE : STR_PAIR_WAITING));
		break;
	}
	default: {
		ui_rect notice = login_notice_rect(phase == LOGIN_ERROR);
		ui_parse_fitted(&title_text, text_buf, scratch_buf, tr(category),
		                TXT_CABEZA, notice.w - 2 * SP_3 - ICON_S - SP_1);
		parse(&detail_text, detail);
		parse(&retry_text, tr(STR_HINT_RETRY));
		parse(&pair_text, tr(STR_HINT_PAIR));
		break;
	}
	}
	parsed_lang = lang_get();
	parsed_content = content;
}

/* --- State ---------------------------------------------------------------- */

static bool animated(void)
{
	return phase == LOGIN_CONNECTING || phase == LOGIN_PAIRING;
}

static login_look current_look(void)
{
	login_look look = {
		.phase = phase,
		.content = content,
		.lang = lang_get(),
		.focus = focus,
		.step = animated() ? anim_frames / LOADER_STEP : 0,
	};
	for (int i = 0; i < LOGIN_BUTTON_COUNT; i++)
		look.button[i] = press_down(&press_button[i]);
	for (int i = 0; i < GLYPH_COUNT; i++)
		look.glyph[i] = press_down(&press_glyph[i]);
	return look;
}

static bool same_look(const login_look *a, const login_look *b)
{
	if (a->phase != b->phase || a->content != b->content ||
	    a->lang != b->lang || a->focus != b->focus || a->step != b->step)
		return false;
	for (int i = 0; i < LOGIN_BUTTON_COUNT; i++)
		if (a->button[i] != b->button[i])
			return false;
	for (int i = 0; i < GLYPH_COUNT; i++)
		if (a->glyph[i] != b->glyph[i])
			return false;
	return true;
}

static void tick(void)
{
	anim_frames++;
	for (int i = 0; i < LOGIN_BUTTON_COUNT; i++)
		press_tick(&press_button[i]);
	for (int i = 0; i < GLYPH_COUNT; i++)
		press_tick(&press_glyph[i]);
}

/* --- Input ---------------------------------------------------------------- */

static void queue(login_action action, const press_state *press)
{
	pending = action;
	pending_press = press;
}

static login_action button_action(login_button button)
{
	return button == LOGIN_BUTTON_PAIR ? LOGIN_ACTION_PAIR
	                                   : LOGIN_ACTION_RETRY;
}

static void release_keys(u32 up)
{
	if (up & KEY_A) {
		press_end(&press_glyph[GLYPH_A]);
		if (touch.target < 0)
			press_end(&press_button[focus]);
	}
	if (up & KEY_Y) {
		press_end(&press_glyph[GLYPH_Y]);
		if (touch.target < 0)
			press_end(&press_button[LOGIN_BUTTON_PAIR]);
	}
	if (up & KEY_B)
		press_end(&press_glyph[GLYPH_B]);
	if (up & KEY_START)
		press_end(&press_glyph[GLYPH_START]);
	if (up & KEY_SELECT)
		press_end(&press_glyph[GLYPH_SELECT]);
}

/* Returns true while the stylus owns the input. Only the error has
 * buttons to touch. */
static bool handle_touch(u32 down, u32 held, u32 up)
{
	if (phase != LOGIN_ERROR)
		return false;
	int hit = -1;
	if (held & KEY_TOUCH) {
		touchPosition pos;
		hidTouchRead(&pos);
		hit = login_hit_test(pos.px, pos.py);
	}
	int control;
	touch_event event = touch_track_update(&touch, (down & KEY_TOUCH) != 0,
	                                       (held & KEY_TOUCH) != 0,
	                                       (up & KEY_TOUCH) != 0, hit,
	                                       &control);
	switch (event) {
	case TOUCH_PRESS:
		focus = (login_button)control;
		press_begin(&press_button[control]);
		break;
	case TOUCH_CANCEL:
		press_cancel(&press_button[control]);
		break;
	case TOUCH_ACTIVATE:
		press_end(&press_button[control]);
		queue(button_action((login_button)control),
		      &press_button[control]);
		break;
	case TOUCH_NONE:
		break;
	}
	return touch.target >= 0;
}

login_action view_login_update(u32 down)
{
	u32 held = hidKeysHeld();
	u32 up = hidKeysUp();
	ensure_parsed();
	tick();
	release_keys(up);

	if (pending != LOGIN_ACTION_NONE) {
		if (!press_shown(pending_press))
			return LOGIN_ACTION_NONE;
		login_action action = pending;
		pending = LOGIN_ACTION_NONE;
		pending_press = NULL;
		return action;
	}

	if (handle_touch(down, held, up) || pending != LOGIN_ACTION_NONE)
		return LOGIN_ACTION_NONE;

	/* Every screen: the language switches at once (6.5: its glyph shows
	 * the press). */
	if (down & KEY_SELECT) {
		press_begin(&press_glyph[GLYPH_SELECT]);
		return LOGIN_ACTION_LANGUAGE;
	}
	switch (phase) {
	case LOGIN_PAIRING:
		if (down & KEY_B) {
			press_begin(&press_glyph[GLYPH_B]);
			queue(LOGIN_ACTION_CANCEL, &press_glyph[GLYPH_B]);
		}
		break;
	case LOGIN_ERROR:
		/* 6.2: the focus moves, and shows, in the same frame. */
		if (down & (KEY_LEFT | KEY_RIGHT))
			focus = login_focus_move(focus, (down & KEY_LEFT) ? -1 : 1);
		if (down & KEY_A) {
			press_begin(&press_button[focus]);
			press_begin(&press_glyph[GLYPH_A]);
			queue(button_action(focus), &press_button[focus]);
		} else if (down & KEY_Y) {
			focus = LOGIN_BUTTON_PAIR;
			press_begin(&press_button[LOGIN_BUTTON_PAIR]);
			press_begin(&press_glyph[GLYPH_Y]);
			queue(LOGIN_ACTION_PAIR, &press_button[LOGIN_BUTTON_PAIR]);
		} else if (down & KEY_START) {
			press_begin(&press_glyph[GLYPH_START]);
			queue(LOGIN_ACTION_EXIT, &press_glyph[GLYPH_START]);
		}
		break;
	case LOGIN_FATAL:
		if (down & KEY_START) {
			press_begin(&press_glyph[GLYPH_START]);
			queue(LOGIN_ACTION_EXIT, &press_glyph[GLYPH_START]);
		}
		break;
	default:
		break;
	}
	return LOGIN_ACTION_NONE;
}

bool view_login_dirty(void)
{
	if (parsed_lang != lang_get() || parsed_content != content ||
	    !drawn_valid)
		return true;
	login_look look = current_look();
	return !same_look(&look, &drawn);
}

/* --- Drawing: top --------------------------------------------------------- */

/* In place of the spinner when the start-up failed (7.2): a raised coral
 * square with the error icon. */
static void draw_error_badge(void)
{
	ui_rect r = login_status_rect();
	ui_bevel_rect(r, COL_CORAL, COL_CORAL_LUZ, COL_CORAL_OSC, false);
	C2D_Image icon = icon_image(ICON_ERROR);
	ui_icon(&icon, r.x + (r.w - ICON_S) / 2, r.y + (r.h - ICON_S) / 2,
	        COL_TINTA);
}

static void draw_pair_top(void)
{
	int cx = SCREEN_TOP_W / 2;
	ui_rect heading = login_pair_heading_rect();
	ui_text_centered(&heading_text, cx, heading.y + heading.h / 2,
	                 TXT_CABEZA, COL_TEXTO);

	/* The `capa` panel: the URL's `lima` is only allowed on it (2.6). */
	ui_rect panel = login_pair_panel_rect();
	ui_panel(panel);
	int meta_h = ui_line_height(TXT_META);
	int code_h = ui_line_height(TXT_CODE);
	int url_h = ui_line_height(TXT_CABEZA);
	int body_h = 3 * ui_line_height(TXT_CUERPO);
	int block = meta_h + code_h + url_h + body_h + 3 * SP_2;
	int y = panel.y + (panel.h - block) / 2;
	ui_text_centered(&your_code_text, cx, y + meta_h / 2, TXT_META,
	                 COL_TEXTO_2);
	y += meta_h + SP_2;
	ui_text_centered(&code_text, cx, y + code_h / 2, TXT_CODE, COL_TEXTO);
	y += code_h + SP_2;
	ui_text_centered(&url_text, cx, y + url_h / 2, TXT_CABEZA, COL_LIMA);
	y += url_h + SP_2;
	ui_text_wrapped_centered(&instructions_text, cx, y, TXT_CUERPO,
	                         COL_TEXTO_2, panel.w - 2 * SP_4);
}

void view_login_draw_top(void)
{
	ensure_parsed();
	if (phase == LOGIN_PAIRING) {
		draw_pair_top();
		return;
	}
	logo_draw_centered(SCREEN_TOP_W / 2, LOGIN_LOGO_CY);
	if (phase == LOGIN_CONNECTING)
		ui_spinner_ring(SCREEN_TOP_W / 2, LOGIN_STATUS_CY,
		                anim_frames / LOADER_STEP);
	else
		draw_error_badge();
}

/* --- Drawing: bottom ------------------------------------------------------ */

/* How many lines `text` wraps to in `width`, roughly: one more than the
 * division, since words wrap early. */
static int wrapped_lines(const C2D_Text *text, float scale, int width)
{
	int w = ui_px_ceil(ui_text_width(text, scale));
	return w <= width ? 1 : w / width + 2;
}

static void draw_connecting(void)
{
	ui_rect area = login_content_rect();
	int cx = area.x + area.w / 2;
	int body_h = ui_line_height(TXT_CUERPO);
	int meta_h = ui_line_height(TXT_META);
	int lines = wrapped_lines(&stage_text, TXT_CUERPO, area.w);
	bool has_step = login_step(stage, paired) > 0;
	int block = lines * body_h + (has_step ? SP_2 + meta_h : 0);
	int y = area.y + (area.h - block) / 2;
	ui_text_wrapped_centered(&stage_text, cx, y, TXT_CUERPO, COL_TEXTO,
	                         area.w);
	if (has_step)
		ui_text_centered(&step_text, cx, y + lines * body_h + SP_2 +
		                 meta_h / 2, TXT_META, COL_TEXTO_2);
}

/* The QR (feature_plan.md, phase 4): `tinta` modules on a `texto` square,
 * with a 1 px `tinta` outline and the hard shadow, drawn a run of dark
 * modules at a time. */
static void draw_qr(void)
{
	ui_rect box = login_qr_rect();
	ui_fill(ui_rect_offset(ui_rect_inset(box, -OUTLINE_W), SHADOW_OFF,
	                       SHADOW_OFF), COL_SOMBRA);
	ui_fill(ui_rect_inset(box, -OUTLINE_W), COL_TINTA);
	ui_fill(box, COL_TEXTO);
	int modules = qrcodegen_getSize(qr);
	int scale = login_qr_scale(modules, LOGIN_QR_BOX);
	if (scale <= 0)
		return;
	int ox = login_qr_origin(box.x, modules, scale);
	int oy = login_qr_origin(box.y, modules, scale);
	for (int row = 0; row < modules; row++) {
		int col = 0;
		while (col < modules) {
			if (!qrcodegen_getModule(qr, col, row)) {
				col++;
				continue;
			}
			int start = col;
			while (col < modules && qrcodegen_getModule(qr, col, row))
				col++;
			ui_rect run = { ox + start * scale, oy + row * scale,
			                (col - start) * scale, scale };
			ui_fill(run, COL_TINTA);
		}
	}
}

static void draw_pairing(void)
{
	ui_rect side = login_pair_side_rect();
	if (qr_ok) {
		draw_qr();
	} else {
		/* No QR: the caption takes the content width. */
		side.x = SP_2;
		side.w = SCREEN_BOTTOM_W - 2 * SP_2;
	}
	int meta_h = ui_line_height(TXT_META);
	/* One sentence beside the QR: the code on the top screen is the main
	 * way, scanning the alternative. */
	if (qr_ok)
		ui_text_wrapped(&qr_body_text, side.x, side.y, TXT_CUERPO,
		                COL_TEXTO, side.w);
	/* The polling status at the foot of the caption: the loading
	 * indicator (5.7) and its text beside it, two lines at most. */
	int text_x = side.x + UI_LOADER_W + SP_2;
	int status_y = side.y + side.h - 2 * meta_h;
	ui_loader(side.x + UI_LOADER_W / 2, status_y + meta_h / 2,
	          anim_frames / LOADER_STEP);
	ui_text_wrapped(&status_text, text_x, status_y, TXT_META, COL_TEXTO_2,
	                side.x + side.w - text_x);
}

static void draw_error(void)
{
	bool buttons = phase == LOGIN_ERROR;
	ui_rect notice = login_notice_rect(buttons);
	int wrap = notice.w - 2 * SP_3;
	notice.h = login_notice_height(
		ui_px_ceil(ui_text_width(&detail_text, TXT_CUERPO)), wrap,
		ui_line_height(TXT_CUERPO), notice.h);
	C2D_Image icon = icon_image(ICON_ERROR);
	ui_notice(notice, true, &icon, &title_text, &detail_text);
	if (!buttons)
		return;
	for (int i = 0; i < LOGIN_BUTTON_COUNT; i++) {
		ui_control_look look = {
			.pressed = press_down(&press_button[i]),
			.focused = focus == (login_button)i,
		};
		/* 5.2: one primary per screen, Retry. */
		ui_button(login_button_rect(i),
		          i == LOGIN_BUTTON_RETRY ? UI_BUTTON_PRIMARY
		                                  : UI_BUTTON_SECONDARY,
		          look, i == LOGIN_BUTTON_RETRY ? &retry_text : &pair_text,
		          NULL);
	}
}

/* The bar of each screen; labels drop from the end if it would not fit. */
static void draw_hints(void)
{
	int glyphs[4];
	const C2D_Text *labels[4];
	int count = 0;
	switch (phase) {
	case LOGIN_PAIRING:
		glyphs[count] = GLYPH_B;
		labels[count++] = &hint_cancel;
		break;
	case LOGIN_ERROR:
		/* A presses the focused button, so it says which. */
		glyphs[count] = GLYPH_A;
		labels[count++] = focus == LOGIN_BUTTON_PAIR ? &hint_pair
		                                             : &hint_retry;
		glyphs[count] = GLYPH_Y;
		labels[count++] = &hint_pair;
		glyphs[count] = GLYPH_START;
		labels[count++] = &hint_exit;
		break;
	case LOGIN_FATAL:
		glyphs[count] = GLYPH_START;
		labels[count++] = &hint_exit;
		break;
	default:
		break;
	}
	glyphs[count] = GLYPH_SELECT;
	labels[count++] = &hint_language;

	int glyph_w[4], label_w[4];
	bool show[4];
	for (int i = 0; i < count; i++) {
		glyph_w[i] = ui_glyph_width(&letter_text[glyphs[i]]);
		label_w[i] = ui_px_ceil(ui_text_width(labels[i], TXT_META));
	}
	ui_rect bar = login_hints_rect();
	login_hints_fit(glyph_w, label_w, count, bar.w, show);
	ui_hint hints[4];
	for (int i = 0; i < count; i++)
		hints[i] = (ui_hint){ &letter_text[glyphs[i]],
		                      show[i] ? labels[i] : NULL,
		                      glyph_colors[glyphs[i]],
		                      press_down(&press_glyph[glyphs[i]]) };
	ui_button_bar(bar, hints, count);
}

void view_login_draw_bottom(void)
{
	ensure_parsed();
	switch (phase) {
	case LOGIN_CONNECTING:
		draw_connecting();
		break;
	case LOGIN_PAIRING:
		draw_pairing();
		break;
	default:
		draw_error();
		break;
	}
	draw_hints();
	drawn = current_look();
	drawn_valid = true;
}
