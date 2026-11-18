/* Layout of the Settings top screen (top_about.h, docs/STYLES.md 7.8).
 * Pure C with no 3DS headers, tested in tests/native_logic_test.c. Whole
 * pixels throughout.
 *
 * Top screen, 400 x 240, over the logo darkened by the veil:
 *   card      16..80   the account: a `capa` panel with the picture
 *                      (48 px) and up to three lines beside it
 *   credits   96..176  four rows, a label column and a value column:
 *                      developer, based on, QR codes, licence
 *   rule      180      1 px across, between the credits and the notice
 *   notice    188..    the trademark notice, wrapped, at most three lines
 */
#ifndef ABOUT_LAYOUT_H
#define ABOUT_LAYOUT_H

#include "layout.h"
#include "theme.h"

#define ABOUT_MARGIN SP_4
#define ABOUT_AVATAR 48
#define ABOUT_ROWS 4
#define ABOUT_ROW_PITCH 20
/* The label column, wide enough for "Developed by" at t_meta. */
#define ABOUT_LABEL_W 88

static inline ui_rect about_card_rect(void)
{
	ui_rect r = { ABOUT_MARGIN, ABOUT_MARGIN,
	              SCREEN_TOP_W - 2 * ABOUT_MARGIN, ABOUT_AVATAR + 2 * SP_2 };
	return r;
}

/* The account's picture, SP_2 inside the card's top left corner. */
static inline ui_rect about_avatar_rect(void)
{
	ui_rect card = about_card_rect();
	ui_rect r = { card.x + SP_2, card.y + SP_2, ABOUT_AVATAR, ABOUT_AVATAR };
	return r;
}

/* Where the card's lines start, and how wide they may be. */
static inline int about_card_text_x(void)
{
	ui_rect avatar = about_avatar_rect();
	return avatar.x + avatar.w + SP_3;
}

static inline int about_card_text_w(void)
{
	ui_rect card = about_card_rect();
	return card.x + card.w - SP_3 - about_card_text_x();
}

/* The vertical centre of credit row `i` (0..ABOUT_ROWS-1). */
static inline int about_row_cy(int i)
{
	ui_rect card = about_card_rect();
	return card.y + card.h + SP_4 + i * ABOUT_ROW_PITCH +
	       ABOUT_ROW_PITCH / 2 - SP_1 / 2;
}

static inline int about_label_x(void)
{
	return ABOUT_MARGIN + SP_1;
}

static inline int about_value_x(void)
{
	return about_label_x() + ABOUT_LABEL_W;
}

static inline int about_value_w(void)
{
	return SCREEN_TOP_W - ABOUT_MARGIN - about_value_x();
}

/* The rule under the credits, 1 px high. */
static inline ui_rect about_rule_rect(void)
{
	ui_rect r = { ABOUT_MARGIN, about_row_cy(ABOUT_ROWS - 1) +
	              ABOUT_ROW_PITCH / 2 + SP_1 / 2 + SP_1,
	              SCREEN_TOP_W - 2 * ABOUT_MARGIN, 1 };
	return r;
}

/* The notice under the rule: its first line's top, and its width. */
static inline int about_notice_y(void)
{
	ui_rect rule = about_rule_rect();
	return rule.y + rule.h + SP_2 - 1;
}

static inline int about_notice_w(void)
{
	return SCREEN_TOP_W - 2 * ABOUT_MARGIN;
}

#endif
