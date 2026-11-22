#include "top.h"

#include "draw.h"
#include "logo.h"
#include "now_layout.h"
#include "top_about.h"
#include "top_detail.h"
#include "top_logic.h"
#include "top_mini.h"
#include "top_now.h"
#include "top_strip.h"
#include "view_downloads.h"

static player_phase phase = PLAYER_PHASE_IDLE;
/* The detail on top of the active tab and its image, or NULL. */
static const detail_doc *detail;
static artwork_state *detail_art;
/* The account signed in and its picture, for Settings. */
static const about_account *account;
static artwork_state *account_art;
static bool manager;
static results_strip strip;
static bool strip_on, drawn_strip_on;
/* The mini player (top_mini.h): whether it was drawn last time. */
static bool drawn_mini;
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
	if (!top_now_init() || !top_detail_init() || !top_about_init() ||
	    !top_strip_init() || !top_mini_init()) {
		top_exit();
		return false;
	}
	return true;
}

void top_exit(void)
{
	top_mini_exit();
	top_strip_exit();
	top_about_exit();
	top_detail_exit();
	top_now_exit();
}

void top_set(const player_info *info)
{
	phase = info->phase;
	top_now_set(info);
	top_mini_set(info);
	/* The real share, not the sampled one: it disappears with the first
	 * byte. */
	spinning = top_shows_spinner(info->phase, info->download_pct);
	spinner_frames = spinning ? spinner_frames + 1 : 0;
}

void top_set_detail(const detail_doc *doc, artwork_state *art)
{
	detail = doc && art ? doc : NULL;
	detail_art = art;
	/* top_set already put this frame's phase. Whether the card has it:
	 * the whole biography never does. */
	top_detail_set_mini(top_shows_mini(TOP_DETAIL, phase, false));
}

void top_set_account(const about_account *acc, artwork_state *art)
{
	account = acc;
	account_art = art;
}

void top_set_manager(bool on)
{
	manager = on;
}

void top_set_results(const results_strip *s)
{
	strip_on = s != NULL;
	if (s)
		strip = *s;
}

/* Whether the mini player shows over `mode` (5.18). */
static bool shows_mini(top_mode mode)
{
	return top_shows_mini(mode, phase, top_detail_showing_page(detail));
}

bool top_dirty(app_tab tab, const artwork_state *art)
{
	top_mode mode = top_mode_with_manager(
		top_mode_for(tab, phase, detail != NULL), manager);
	bool mini = shows_mini(mode);
	if (!drawn_valid || mode != drawn_mode || spinning != drawn_spinning ||
	    (spinning && spinner_step() != drawn_step) ||
	    strip_on != drawn_strip_on || (strip_on && top_strip_dirty(&strip)) ||
	    mini != drawn_mini || (mini && top_mini_dirty(art)))
		return true;
	switch (mode) {
	case TOP_NOW_CARD:
		return top_now_dirty(false, art);
	case TOP_ART_BIG:
		return top_now_dirty(true, art);
	case TOP_DETAIL:
		return top_detail_dirty(detail, detail_art);
	case TOP_ABOUT:
		return account && top_about_dirty(account, account_art);
	case TOP_DOWNLOADS:
		return view_downloads_top_dirty();
	default:
		/* The logo never changes by itself. */
		return false;
	}
}

/* Where it goes: its corner on what it sits by -- the card's cover, lower
 * under the strip; the manager's panel. */
static ui_rect mini_rect(top_mode mode)
{
	if (mode == TOP_DOWNLOADS)
		return now_mini_panel_rect();
	return now_mini_card_rect(strip_on ? RESULTS_CARD_Y
	                                   : now_detail_art_rect().y);
}

void top_draw(app_tab tab, artwork_state *art)
{
	top_mode mode = top_mode_with_manager(
		top_mode_for(tab, phase, detail != NULL), manager);
	bool mini = shows_mini(mode);
	switch (mode) {
	case TOP_NOW_CARD:
		top_now_draw_card(art);
		break;
	case TOP_ART_BIG:
		top_now_draw_big(art);
		break;
	case TOP_DETAIL:
		/* Under the strip, the two centred together (5.17). */
		top_detail_draw(detail, detail_art, strip_on ? RESULTS_CARD_Y : -1);
		break;
	case TOP_ABOUT:
		if (account) {
			top_about_draw(account, account_art);
			break;
		}
		logo_draw_centered(SCREEN_TOP_W / 2, SCREEN_H / 2);
		break;
	case TOP_DOWNLOADS:
		view_downloads_draw_top(mini);
		break;
	default:
		logo_draw_centered(SCREEN_TOP_W / 2, SCREEN_H / 2);
		break;
	}
	/* Above the card, which starts lower down. */
	if (strip_on)
		top_strip_draw(&strip);
	drawn_strip_on = strip_on;
	/* In its corner, clear of the rest. While the track's download is at
	 * 0%, the spinner turns in it, in place of its icon. */
	if (mini)
		top_mini_draw(art, mini_rect(mode), spinning ? spinner_step() : -1);
	drawn_mini = mini;
	/* Otherwise over whatever is below, on any tab. */
	if (spinning && !mini)
		ui_spinner(SCREEN_TOP_W / 2, SCREEN_H / 2, spinner_step());
	drawn_spinning = spinning;
	drawn_step = spinner_step();
	drawn_mode = mode;
	drawn_valid = true;
}
