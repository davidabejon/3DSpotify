/* Pressed-state timing and touch tracking (docs/STYLES.md 6.3-6.5). Pure C,
 * no 3DS headers, tested in tests/native_logic_test.c.
 *
 * A press shows the pressed look from the frame it is detected, stays while
 * the button or stylus is held and for at least ANIM_PRESS_MIN frames, then
 * returns to rest in a single frame. Call press_tick once per frame before
 * handling that frame's input. */
#ifndef PRESS_LOGIC_H
#define PRESS_LOGIC_H

#include "theme.h"

#include <stdbool.h>

typedef struct {
	bool active;   /* showing the pressed look */
	bool held;
	int frames;    /* frames shown pressed so far, minus one */
} press_state;

static inline void press_reset(press_state *p)
{
	p->active = p->held = false;
	p->frames = 0;
}

static inline void press_begin(press_state *p)
{
	p->active = p->held = true;
	p->frames = 0;
}

/* The button or stylus was released: rest once the minimum has shown. */
static inline void press_end(press_state *p)
{
	p->held = false;
}

/* The stylus left the control: back to rest now, minimum or not. */
static inline void press_cancel(press_state *p)
{
	press_reset(p);
}

static inline void press_tick(press_state *p)
{
	if (!p->active)
		return;
	p->frames++;
	if (!p->held && p->frames >= ANIM_PRESS_MIN)
		press_reset(p);
}

static inline bool press_down(const press_state *p)
{
	return p->active;
}

/* The pressed look has been on screen for the minimum: an action that
 * takes the screens over (the keyboard, a reconnect) may run now. */
static inline bool press_shown(const press_state *p)
{
	return !p->active || p->frames >= ANIM_PRESS_MIN;
}

/* Touch: pressed on touching a control, activated on lifting the stylus
 * inside it, cancelled for good if the stylus slides out first. Touching
 * outside every control does nothing. */
typedef enum {
	TOUCH_NONE,
	TOUCH_PRESS,
	TOUCH_CANCEL,
	TOUCH_ACTIVATE
} touch_event;

typedef struct {
	int target; /* the pressed control, -1 for none */
} touch_track;

/* `hit` is the control under the stylus this frame (-1 for none); it is
 * only read while touching. `*control` receives the control the event is
 * about. */
static inline touch_event touch_track_update(touch_track *t, bool down,
                                             bool held, bool up, int hit,
                                             int *control)
{
	*control = t->target;
	if (down) {
		t->target = hit;
		*control = hit;
		return hit >= 0 ? TOUCH_PRESS : TOUCH_NONE;
	}
	if (t->target < 0)
		return TOUCH_NONE;
	if (held && hit != t->target) {
		t->target = -1;
		return TOUCH_CANCEL;
	}
	if (up) {
		t->target = -1;
		return TOUCH_ACTIVATE;
	}
	return TOUCH_NONE;
}

#endif
