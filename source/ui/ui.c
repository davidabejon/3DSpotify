#include "ui.h"

#include <citro3d.h>

static C3D_RenderTarget *top_target;
static C3D_RenderTarget *bottom_target;
static bool c3d_ready;
static bool c2d_ready;
static aptHookCookie apt_cookie;
static bool apt_hooked;
static volatile bool restored = true;

/* Back from the HOME menu, a sleeping lid or an applet, the screens may show
 * something else; with on-demand rendering nothing would repaint them. */
static void on_apt_event(APT_HookType hook, void *param)
{
	(void)param;
	if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP)
		restored = true;
}

bool ui_init(void)
{
	c3d_ready = C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	if (!c3d_ready)
		return false;
	c2d_ready = C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	if (!c2d_ready) {
		ui_exit();
		return false;
	}
	C2D_Prepare();
	top_target = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	bottom_target = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	if (!top_target || !bottom_target) {
		ui_exit();
		return false;
	}
	aptHook(&apt_cookie, on_apt_event, NULL);
	apt_hooked = true;
	return true;
}

void ui_exit(void)
{
	if (apt_hooked)
		aptUnhook(&apt_cookie);
	apt_hooked = false;
	if (top_target)
		C3D_RenderTargetDelete(top_target);
	if (bottom_target)
		C3D_RenderTargetDelete(bottom_target);
	top_target = bottom_target = NULL;
	if (c2d_ready)
		C2D_Fini();
	if (c3d_ready)
		C3D_Fini();
	c2d_ready = c3d_ready = false;
}

void ui_begin(void)
{
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
}

void ui_scene(gfxScreen_t screen)
{
	C3D_RenderTarget *target = screen == GFX_TOP ? top_target : bottom_target;
	float width = screen == GFX_TOP ? SCREEN_TOP_W : SCREEN_BOTTOM_W;
	C2D_TargetClear(target, COL_FONDO_INF);
	C2D_SceneBegin(target);
	/* One rectangle: free for the CPU. */
	C2D_DrawRectangle(0, 0, 0, width, SCREEN_H, COL_FONDO_SUP, COL_FONDO_SUP,
	                  COL_FONDO_INF, COL_FONDO_INF);
}

void ui_end(void)
{
	C3D_FrameEnd(0);
}

bool ui_take_restored(void)
{
	bool was = restored;
	restored = false;
	return was;
}
