#include "top.h"

#include "logo.h"
#include "top_logic.h"
#include "top_now.h"

static player_phase phase = PLAYER_PHASE_IDLE;
static top_mode drawn_mode;
static bool drawn_valid;

bool top_init(void)
{
	drawn_valid = false;
	return top_now_init();
}

void top_exit(void)
{
	top_now_exit();
}

void top_set(const player_info *info)
{
	phase = info->phase;
	top_now_set(info);
}

bool top_dirty(app_tab tab, const artwork_state *art)
{
	top_mode mode = top_mode_for(tab, phase);
	if (!drawn_valid || mode != drawn_mode)
		return true;
	switch (mode) {
	case TOP_NOW_CARD:
		return top_now_dirty(false, art);
	case TOP_ART_BIG:
		return top_now_dirty(true, art);
	default:
		/* The logo never changes by itself. */
		return false;
	}
}

void top_draw(app_tab tab, artwork_state *art)
{
	top_mode mode = top_mode_for(tab, phase);
	switch (mode) {
	case TOP_NOW_CARD:
		top_now_draw_card(art);
		break;
	case TOP_ART_BIG:
		top_now_draw_big(art);
		break;
	default:
		logo_draw_centered(SCREEN_TOP_W / 2, SCREEN_H / 2);
		break;
	}
	drawn_mode = mode;
	drawn_valid = true;
}
