#include "top.h"

#include "draw.h"
#include "logo.h"
#include "top_detail.h"
#include "top_logic.h"
#include "top_now.h"

static player_phase phase = PLAYER_PHASE_IDLE;
/* The detail on top of the active tab and its image, or NULL. */
static const detail_doc *detail;
static artwork_state *detail_art;
static top_mode drawn_mode;
static bool drawn_valid;
/* The spinner: whether it shows, its frame count, and what was drawn. */
static bool spinning;
static int spinner_frames;
static bool drawn_spinning;
static int drawn_step;

static int spinner_step(void)
{
	return spinner_frames / LOADER_STEP;
}

bool top_init(void)
{
	drawn_valid = false;
	if (!top_now_init() || !top_detail_init()) {
		top_exit();
		return false;
	}
	return true;
}

void top_exit(void)
{
	top_detail_exit();
	top_now_exit();
}

void top_set(const player_info *info)
{
	phase = info->phase;
	top_now_set(info);
	/* The real share, not the sampled one: it disappears with the first
	 * byte. */
	spinning = top_shows_spinner(info->phase, info->download_pct);
	spinner_frames = spinning ? spinner_frames + 1 : 0;
}

void top_set_detail(const detail_doc *doc, artwork_state *art)
{
	detail = doc && art ? doc : NULL;
	detail_art = art;
}

bool top_dirty(app_tab tab, const artwork_state *art)
{
	top_mode mode = top_mode_for(tab, phase, detail != NULL);
	if (!drawn_valid || mode != drawn_mode || spinning != drawn_spinning ||
	    (spinning && spinner_step() != drawn_step))
		return true;
	switch (mode) {
	case TOP_NOW_CARD:
		return top_now_dirty(false, art);
	case TOP_ART_BIG:
		return top_now_dirty(true, art);
	case TOP_DETAIL:
		return top_detail_dirty(detail, detail_art);
	default:
		/* The logo never changes by itself. */
		return false;
	}
}

void top_draw(app_tab tab, artwork_state *art)
{
	top_mode mode = top_mode_for(tab, phase, detail != NULL);
	switch (mode) {
	case TOP_NOW_CARD:
		top_now_draw_card(art);
		break;
	case TOP_ART_BIG:
		top_now_draw_big(art);
		break;
	case TOP_DETAIL:
		top_detail_draw(detail, detail_art);
		break;
	default:
		logo_draw_centered(SCREEN_TOP_W / 2, SCREEN_H / 2);
		break;
	}
	/* Over whatever is below, on any tab. */
	if (spinning)
		ui_spinner(SCREEN_TOP_W / 2, SCREEN_H / 2, spinner_step());
	drawn_spinning = spinning;
	drawn_step = spinner_step();
	drawn_mode = mode;
	drawn_valid = true;
}
