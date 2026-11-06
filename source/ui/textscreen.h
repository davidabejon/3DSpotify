/* A stand-in for the libctru console on the bottom screen, drawn with
 * citro2d and the system font.
 *
 * It keeps the console's model: a 40 x 30 grid of character cells, a cursor,
 * wrapping at 40 columns and scrolling at the bottom, so the screens not yet
 * redesigned only swap consoleClear/printf for textscreen_clear/printf.
 * Unlike the console it stores Unicode code points, so UTF-8 track names
 * keep their accents and CJK characters (as far as the system font goes).
 *
 * Understood escapes: "\x1b[<row>;<col>H" (1-based cursor move) and
 * "\x1b[2J" (clear). Any other escape sequence is skipped.
 *
 * Rows are spread over the screen by how many are in use, so short screens
 * get larger text. All rows use `texto`, the colour allowed on the
 * background (docs/STYLES.md 2.6). */
#ifndef TEXTSCREEN_H
#define TEXTSCREEN_H

#include <stdbool.h>

#define TEXTSCREEN_ROWS 30
#define TEXTSCREEN_COLS 40

/* After ui_init. */
bool textscreen_init(void);
void textscreen_exit(void);

void textscreen_clear(void);
void textscreen_printf(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));

/* True when the content changed since the last textscreen_draw. Writing the
 * same characters again does not count, so status lines can be reprinted
 * every frame without forcing a GPU frame. */
bool textscreen_dirty(void);
/* Forces a redraw, for when an applet may have drawn over the screen. */
void textscreen_invalidate(void);
/* Draws into the current scene (the bottom screen). */
void textscreen_draw(void);

#endif
