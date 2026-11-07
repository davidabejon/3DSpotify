/* The screens before the session is up (feature_plan.md, phase 4; STYLES.md
 * 7.2 and 7.3): connecting, pairing with a QR code, and the start-up error.
 * Both screens are this view's while it is active.
 *
 * The caller says which screen to show and with what; the view draws it and
 * turns input into actions, following STYLES.md 6 like the other views:
 * pressed states for at least ANIM_PRESS_MIN frames, touch acts on lifting
 * the stylus, and actions that change the screen are returned once their
 * press has shown. SELECT switches the language on every screen. Main
 * thread only. */
#ifndef VIEW_LOGIN_H
#define VIEW_LOGIN_H

#include <3ds.h>

#include "lang.h"

#include <stdbool.h>

typedef enum {
	LOGIN_ACTION_NONE,
	LOGIN_ACTION_LANGUAGE,  /* SELECT: the caller switches the language */
	LOGIN_ACTION_CANCEL,    /* B while pairing */
	LOGIN_ACTION_RETRY,     /* the error's Retry: A, or the button */
	LOGIN_ACTION_PAIR,      /* the error's Pair: Y, or the button */
	LOGIN_ACTION_EXIT       /* START on an error */
} login_action;

/* What the pairing screen shows. Never the device code: it is secret. */
typedef struct {
	bool reason_expired;    /* the stored credential was refused */
	bool offline;           /* the last poll could not reach Spotify */
	const char *user_code;
	const char *verification_uri;
	const char *qr_url;     /* empty: no QR, only the code */
} login_pair_info;

/* After ui_init and icons_init. */
bool view_login_init(void);
void view_login_exit(void);

/* Which screen to show. Cheap to call every frame: the same content
 * changes nothing. */
void view_login_connecting(str_id stage, bool paired);
void view_login_pairing(const login_pair_info *info);
/* `detail` is already text; `fatal` leaves only START and SELECT. */
void view_login_error(str_id category, const char *detail, bool fatal);

/* Once per frame, after hidScanInput. */
login_action view_login_update(u32 down);

bool view_login_dirty(void);
void view_login_draw_top(void);
void view_login_draw_bottom(void);

#endif
