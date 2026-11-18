#include "top_about.h"

#include "about_layout.h"
#include "draw.h"
#include "icons.h"
#include "lang.h"
#include "logo.h"
#include "theme.h"
#include "ui_font.h"

#include <3ds.h>
#include <citro2d.h>

#include <stdio.h>
#include <string.h>

/* The card's three lines and the credits' values fitted to their columns,
 * the labels and the notice, which wraps. */
#define TEXT_GLYPHS (8 * UI_FIELD_SIZE + 256)

/* Names, not words: they read the same in every language, so they stay out
 * of the language tables (lang.h). The order is the rows'. */
static const char *const credit_values[ABOUT_ROWS] = {
	"David Abej\xc3\xb3n",
	"SpotiBacan (ohquebacan)",
	"QR Code generator, Project Nayuki (MIT)",
	"Apache License 2.0",
};
static const str_id credit_labels[ABOUT_ROWS] = {
	STR_ABOUT_DEVELOPER, STR_ABOUT_BASED_ON, STR_ABOUT_QR, STR_ABOUT_LICENSE,
};

/* The card's lines: the name, then the plan and the country, then the
 * username when the name is another. Empty ones take no place. */
enum { CARD_NAME, CARD_PLAN, CARD_USER, CARD_LINES };
static const float card_scale[CARD_LINES] = {
	TXT_CABEZA, TXT_CUERPO, TXT_META
};
static const u32 card_ink[CARD_LINES] = {
	COL_TEXTO, COL_TEXTO_2, COL_TEXTO_2
};

static C2D_TextBuf text_buf, scratch_buf;
static C2D_Text card_text[CARD_LINES];
static bool card_shown[CARD_LINES];
static C2D_Text label_text[ABOUT_ROWS];
static C2D_Text value_text[ABOUT_ROWS];
static C2D_Text notice_text;

/* What was parsed, and whether it was drawn since. */
static unsigned parsed_serial;
static lang_id parsed_lang = LANG_COUNT;
static bool parsed_valid;
static bool drawn_valid;

bool top_about_init(void)
{
	text_buf = C2D_TextBufNew(TEXT_GLYPHS);
	scratch_buf = C2D_TextBufNew(UI_FIELD_SIZE);
	if (!text_buf || !scratch_buf) {
		top_about_exit();
		return false;
	}
	return true;
}

void top_about_exit(void)
{
	C2D_TextBuf *bufs[] = { &text_buf, &scratch_buf };
	for (size_t i = 0; i < sizeof(bufs) / sizeof(bufs[0]); i++) {
		if (*bufs[i])
			C2D_TextBufDelete(*bufs[i]);
		*bufs[i] = NULL;
	}
	parsed_valid = false;
	drawn_valid = false;
}

void top_about_invalidate(void)
{
	drawn_valid = false;
}

/* --- Text -------------------------------------------------------------- */

static const char *plan_name(const char *product)
{
	if (strcmp(product, "premium") == 0)
		return tr(STR_ABOUT_PLAN_PREMIUM);
	if (strcmp(product, "free") == 0 || strcmp(product, "open") == 0)
		return tr(STR_ABOUT_PLAN_FREE);
	return "";
}

static void parse_card_line(int line, const char *string)
{
	card_shown[line] = string[0] != '\0';
	if (card_shown[line])
		ui_parse_fitted(&card_text[line], text_buf, scratch_buf, string,
		                card_scale[line], about_card_text_w());
}

static void parse(const about_account *account)
{
	C2D_TextBufClear(text_buf);

	/* The name the user chose, else the username. */
	bool named = account->display_name[0] != '\0';
	parse_card_line(CARD_NAME, named ? account->display_name
	                                 : account->username);
	/* "Premium - ES", either half alone when the other is unknown. */
	char plan[64];
	const char *product = plan_name(account->product);
	snprintf(plan, sizeof(plan), "%s%s%s", product,
	         product[0] && account->country[0] ? " - " : "",
	         account->country);
	parse_card_line(CARD_PLAN, plan);
	char user[160] = "";
	if (named && account->username[0] &&
	    strcmp(account->display_name, account->username) != 0)
		snprintf(user, sizeof(user), tr(STR_ABOUT_USER_FMT),
		         account->username);
	parse_card_line(CARD_USER, user);

	for (int i = 0; i < ABOUT_ROWS; i++) {
		ui_parse_fitted(&label_text[i], text_buf, scratch_buf,
		                tr(credit_labels[i]), TXT_META, ABOUT_LABEL_W - SP_2);
		ui_parse_fitted(&value_text[i], text_buf, scratch_buf,
		                credit_values[i], TXT_CUERPO, about_value_w());
	}
	ui_parse(&notice_text, text_buf, tr(STR_ABOUT_DISCLAIMER));
	C2D_TextOptimize(&notice_text);
}

static bool stale(const about_account *account)
{
	return !parsed_valid || account->serial != parsed_serial ||
	       parsed_lang != lang_get();
}

/* --- State ------------------------------------------------------------- */

bool top_about_dirty(const about_account *account, const artwork_state *art)
{
	return !drawn_valid || stale(account) || artwork_dirty(art);
}

/* --- Drawing ----------------------------------------------------------- */

/* The card's lines, centred as a block on the picture's height. */
static void draw_card(artwork_state *art)
{
	ui_rect card = about_card_rect();
	ui_panel(card);
	ui_rect avatar = about_avatar_rect();
	artwork_draw_or(art, avatar, 0, ICON_USER);

	int heights[CARD_LINES];
	int block = 0, shown = 0;
	for (int i = 0; i < CARD_LINES; i++) {
		heights[i] = ui_line_height(card_scale[i]);
		if (card_shown[i]) {
			block += heights[i];
			shown++;
		}
	}
	if (shown == 0)
		return;
	block += (shown - 1) * SP_1 / 2;
	int y = avatar.y + (avatar.h - block) / 2;
	int x = about_card_text_x();
	for (int i = 0; i < CARD_LINES; i++) {
		if (!card_shown[i])
			continue;
		ui_text(&card_text[i], x, y + heights[i] / 2, card_scale[i],
		        card_ink[i]);
		y += heights[i] + SP_1 / 2;
	}
}

void top_about_draw(const about_account *account, artwork_state *art)
{
	if (stale(account)) {
		parse(account);
		parsed_serial = account->serial;
		parsed_lang = lang_get();
		parsed_valid = true;
	}

	/* The logo where it always is, then the veil over it and the
	 * background: the screen reads as the logo's, dimmed. */
	logo_draw_centered(SCREEN_TOP_W / 2, SCREEN_H / 2);
	ui_fill((ui_rect){ 0, 0, SCREEN_TOP_W, SCREEN_H }, ABOUT_VEIL_COLOR);

	draw_card(art);

	/* On the veil, darker than `capa`: its text colours apply (2.6). */
	for (int i = 0; i < ABOUT_ROWS; i++) {
		int cy = about_row_cy(i);
		ui_text(&label_text[i], about_label_x(), cy, TXT_META,
		        COL_CLOROFILA);
		ui_text(&value_text[i], about_value_x(), cy, TXT_CUERPO,
		        COL_TEXTO);
	}
	ui_fill(about_rule_rect(), COL_CAPA_LUZ);
	ui_text_wrapped(&notice_text, ABOUT_MARGIN, about_notice_y(), TXT_META,
	                COL_TEXTO_2, about_notice_w());

	drawn_valid = true;
}
