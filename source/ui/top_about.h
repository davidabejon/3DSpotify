/* The top screen of Settings (docs/STYLES.md 7.8): the logo darkened by a
 * veil, and over it the account signed in, the credits (the author and
 * what NOTICE names) and the licence, laid out by about_layout.h.
 *
 * The account comes from the session (the username) and from the Web
 * API's /v1/me (name, plan, country, picture), which the caller fetches
 * and keeps; any of it may be missing. It is personal data, shown only
 * here, on the user's own screen. Main thread only. */
#ifndef TOP_ABOUT_H
#define TOP_ABOUT_H

#include "artwork.h"

#include <stdbool.h>

typedef struct {
	char username[128];
	char display_name[128];
	char product[16];     /* "premium", "free"... or "" */
	char country[8];
	/* Bumped by the caller whenever any of the above changes. */
	unsigned serial;
} about_account;

/* After ui_init; false when there is no memory for the text. */
bool top_about_init(void);
void top_about_exit(void);

/* Forces a repaint: another view drew the top screen meanwhile. */
void top_about_invalidate(void);

bool top_about_dirty(const about_account *account, const artwork_state *art);
/* `art` is the account's picture. */
void top_about_draw(const about_account *account, artwork_state *art);

#endif
