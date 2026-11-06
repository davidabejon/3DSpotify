#include "tabbar.h"

#include "draw.h"
#include "icons.h"
#include "press_logic.h"
#include "theme.h"

static const icon_id tab_icons[TAB_COUNT] = {
	ICON_HOME, ICON_SEARCH, ICON_PLAYING, ICON_SETTINGS
};

static app_tab active = TAB_HOME;
static press_state press[TAB_COUNT];
static touch_track touch = { -1 };
/* The button that pressed a tab, released with it (6.5). */
static u32 press_keys;

typedef struct {
	app_tab active;
	bool pressed[TAB_COUNT];
} tabbar_look;
static tabbar_look drawn;
static bool drawn_valid;

void tabbar_reset(app_tab tab)
{
	active = tab;
	for (int i = 0; i < TAB_COUNT; i++)
		press_reset(&press[i]);
	touch.target = -1;
	press_keys = 0;
	drawn_valid = false;
}

void tabbar_set_active(app_tab tab)
{
	active = tab;
}

app_tab tabbar_active(void)
{
	return active;
}

bool tabbar_touching(void)
{
	return touch.target >= 0;
}

int tabbar_update(u32 down, u32 held, u32 up)
{
	for (int i = 0; i < TAB_COUNT; i++)
		press_tick(&press[i]);
	if (up & press_keys) {
		for (int i = 0; i < TAB_COUNT; i++)
			press_end(&press[i]);
		press_keys = 0;
	}

	/* Touch: only a touch that starts on the bar belongs to it. */
	int hit = -1;
	if (held & KEY_TOUCH) {
		touchPosition pos;
		hidTouchRead(&pos);
		hit = tabs_hit(pos.px, pos.py);
	}
	int control;
	touch_event event = touch_track_update(&touch, (down & KEY_TOUCH) != 0,
	                                       (held & KEY_TOUCH) != 0,
	                                       (up & KEY_TOUCH) != 0, hit,
	                                       &control);
	switch (event) {
	case TOUCH_PRESS:
		press_begin(&press[control]);
		break;
	case TOUCH_CANCEL:
		press_cancel(&press[control]);
		break;
	case TOUCH_ACTIVATE:
		press_end(&press[control]);
		return control;
	case TOUCH_NONE:
		break;
	}
	if (touch.target >= 0)
		return -1;

	/* Buttons: the switch happens on pressing (6.5). */
	int delta = tabs_key_delta((down & KEY_L) != 0, (down & KEY_R) != 0,
	                           (down & KEY_ZL) != 0, (down & KEY_ZR) != 0);
	int next = tabs_step(active, delta);
	if (next < 0)
		return -1;
	press_begin(&press[next]);
	press_keys = down & (KEY_L | KEY_R | KEY_ZL | KEY_ZR);
	return next;
}

static tabbar_look current_look(void)
{
	tabbar_look look = { .active = active };
	for (int i = 0; i < TAB_COUNT; i++)
		look.pressed[i] = press_down(&press[i]);
	return look;
}

bool tabbar_dirty(void)
{
	if (!drawn_valid)
		return true;
	tabbar_look look = current_look();
	if (look.active != drawn.active)
		return true;
	for (int i = 0; i < TAB_COUNT; i++)
		if (look.pressed[i] != drawn.pressed[i])
			return true;
	return false;
}

/* 5.12: at rest a `capa` panel with the icon in `texto`; active, flat
 * `clorofila` with a `musgo` bottom band and the icon in `tinta`; pressed,
 * `musgo` with the icon in `tinta`. Pressed and active tabs do not move
 * like other controls (6.3): their bottom edge rises (tabs_tab_face), more
 * while pressed, and the strip left below is filled with the background
 * so rows scrolling under the bar stay hidden. */
static void draw_tab(int tab, bool pressed)
{
	tab_face face = pressed ? TAB_FACE_PRESSED
	              : tab == (int)active ? TAB_FACE_ACTIVE : TAB_FACE_REST;
	ui_rect r = tabs_tab_face(tab, face);
	if (face != TAB_FACE_REST) {
		ui_rect full = tabs_tab_rect(tab);
		ui_rect strip = { full.x, r.y + r.h, full.w, full.h - r.h };
		ui_fill(strip, COL_FONDO_SUP);
	}
	u32 ink;
	if (pressed) {
		ui_fill(r, COL_TINTA);
		ui_fill(ui_rect_inset(r, OUTLINE_W), COL_MUSGO);
		ink = COL_TINTA;
	} else if (tab == (int)active) {
		ui_fill(r, COL_TINTA);
		ui_rect inside = ui_rect_inset(r, OUTLINE_W);
		ui_fill(inside, COL_CLOROFILA);
		ui_rect band = { inside.x, inside.y + inside.h - BEVEL_W, inside.w,
		                 BEVEL_W };
		ui_fill(band, COL_MUSGO);
		ink = COL_TINTA;
	} else {
		ui_panel(r);
		ink = COL_TEXTO;
	}
	C2D_Image icon = icon_image(tab_icons[tab]);
	ui_icon(&icon, r.x + (r.w - ICON_S) / 2, r.y + (r.h - ICON_S) / 2, ink);
}

void tabbar_draw(void)
{
	tabbar_look look = current_look();
	/* Tabs only shrink inside their own space: no order needed. */
	for (int i = 0; i < TAB_COUNT; i++)
		draw_tab(i, look.pressed[i]);
	drawn = look;
	drawn_valid = true;
}
