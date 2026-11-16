/* Text in any script the console has a font for.
 *
 * Everything is drawn with the system font, which on a European, American
 * or Japanese console covers Latin, Greek, Cyrillic and Japanese but not
 * Korean or most of Chinese. The console also carries the Korean, Chinese
 * and Taiwanese system fonts, the same design: ui_font_start loads the ones
 * it is not using in the background, and ui_parse draws each character
 * with the first font that has it, all in one C2D_Text, so measuring,
 * fitting with "...", wrapping and rolling work on it as on any other.
 * Before that the text goes through text_compose.h (accents and Korean
 * written apart from their letter).
 * An emulator without the console's system archives has none of those
 * fonts: there the other scripts still show as boxes.
 *
 * Mixing fonts in one C2D_Text means writing into citro2d's glyph buffer,
 * whose layout is private: ui_font_start checks it against a parse of its
 * own and, if it does not match (another citro2d), every string is parsed
 * with the system font alone, as before. Main thread only, except the
 * loader. */
#ifndef UI_FONT_H
#define UI_FONT_H

#include <citro2d.h>

/* After ui_init: checks the glyph buffer and starts loading the fonts. */
void ui_font_start(void);
/* Waits for the loader. Before any text with content from the service is
 * parsed (the tabs); the start-up screens only show the app's own text. */
void ui_font_wait(void);
/* Frees the fonts. After the last frame, before ui_exit. */
void ui_font_exit(void);

/* C2D_TextParse with the fonts above and text_compose. */
void ui_parse(C2D_Text *text, C2D_TextBuf buf, const char *string);

#endif
