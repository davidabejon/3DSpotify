#include "textscreen.h"

#include "ui.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Every cell can hold a glyph; a little slack for the parser. */
#define TEXT_GLYPHS     (TEXTSCREEN_ROWS * TEXTSCREEN_COLS + 256)
#define MAX_LINE_HEIGHT 16.0f
#define MARGIN_X        4.0f
#define MARGIN_Y        2.0f
#define USABLE_WIDTH    (320.0f - 2 * MARGIN_X)
#define USABLE_HEIGHT   (240.0f - 2 * MARGIN_Y)

static uint32_t cells[TEXTSCREEN_ROWS][TEXTSCREEN_COLS];
static int cursor_row;
static int cursor_col;
/* `dirty`: a GPU frame is needed. `stale`: the parsed C2D_Text no longer
 * matches the cells. Parsing is the expensive part, so it only happens in
 * textscreen_draw and only after the content really changed. */
static bool dirty = true;
static bool stale = true;

static C2D_TextBuf text_buf;
static C2D_Text row_text[TEXTSCREEN_ROWS];
static float row_width[TEXTSCREEN_ROWS]; /* at scale 1; 0 = empty row */
static int rows_used;

bool textscreen_init(void)
{
	text_buf = C2D_TextBufNew(TEXT_GLYPHS);
	textscreen_clear();
	return text_buf != NULL;
}

void textscreen_exit(void)
{
	if (text_buf)
		C2D_TextBufDelete(text_buf);
	text_buf = NULL;
}

static void changed(void)
{
	dirty = true;
	stale = true;
}

void textscreen_clear(void)
{
	static const uint32_t empty[TEXTSCREEN_ROWS][TEXTSCREEN_COLS];
	if (memcmp(cells, empty, sizeof(cells)) != 0) {
		memset(cells, 0, sizeof(cells));
		changed();
	}
	cursor_row = cursor_col = 0;
}

static void new_line(void)
{
	cursor_col = 0;
	if (++cursor_row < TEXTSCREEN_ROWS)
		return;
	memmove(cells[0], cells[1], sizeof(cells[0]) * (TEXTSCREEN_ROWS - 1));
	memset(cells[TEXTSCREEN_ROWS - 1], 0, sizeof(cells[0]));
	cursor_row = TEXTSCREEN_ROWS - 1;
	changed();
}

static void put(uint32_t cp)
{
	/* Like the console: a full row wraps before the next character, so a
	 * 40-column line followed by '\n' still takes one row. */
	if (cursor_col >= TEXTSCREEN_COLS)
		new_line();
	if (cells[cursor_row][cursor_col] != cp) {
		cells[cursor_row][cursor_col] = cp;
		changed();
	}
	cursor_col++;
}

/* `p` points just past ESC. Returns where the sequence ends. */
static const char *escape(const char *p)
{
	if (*p != '[')
		return p;
	p++;
	int args[2] = {0, 0};
	int n = 0;
	for (; *p; p++) {
		if (*p >= '0' && *p <= '9') {
			if (n < 2 && args[n] < 1000)
				args[n] = args[n] * 10 + (*p - '0');
		} else if (*p == ';') {
			n++;
		} else {
			char command = *p++;
			if (command == 'H' || command == 'f') {
				int row = args[0] > 0 ? args[0] - 1 : 0;
				int col = args[1] > 0 ? args[1] - 1 : 0;
				cursor_row = row < TEXTSCREEN_ROWS ? row : TEXTSCREEN_ROWS - 1;
				cursor_col = col < TEXTSCREEN_COLS ? col : TEXTSCREEN_COLS - 1;
			} else if (command == 'J' && args[0] == 2) {
				textscreen_clear();
			}
			return p;
		}
	}
	return p;
}

/* Decodes one UTF-8 sequence; malformed input becomes '?' one byte at a
 * time, so it can never swallow the terminator. */
static const char *decode(const char *s, uint32_t *cp)
{
	const unsigned char *p = (const unsigned char *)s;
	if (p[0] < 0x80) {
		*cp = p[0];
		return s + 1;
	}
	int len = p[0] >= 0xF0 ? 4 : p[0] >= 0xE0 ? 3 : p[0] >= 0xC2 ? 2 : 0;
	if (len == 0 || p[0] > 0xF4) {
		*cp = '?';
		return s + 1;
	}
	uint32_t value = p[0] & (0x7F >> len);
	for (int i = 1; i < len; i++) {
		if ((p[i] & 0xC0) != 0x80) {
			*cp = '?';
			return s + 1;
		}
		value = (value << 6) | (p[i] & 0x3F);
	}
	*cp = value;
	return s + len;
}

static void write_text(const char *s)
{
	while (*s) {
		char c = *s;
		if (c == '\x1b') {
			s = escape(s + 1);
		} else if (c == '\n') {
			new_line();
			s++;
		} else if (c == '\r') {
			cursor_col = 0;
			s++;
		} else if ((unsigned char)c < 0x20) {
			if (c == '\t')
				put(' ');
			s++;
		} else {
			uint32_t cp;
			s = decode(s, &cp);
			put(cp);
		}
	}
}

void textscreen_printf(const char *fmt, ...)
{
	char text[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	write_text(text);
}

bool textscreen_dirty(void)
{
	return dirty;
}

void textscreen_invalidate(void)
{
	dirty = true;
}

static size_t encode(uint32_t cp, char *out)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

static void reparse(void)
{
	C2D_TextBufClear(text_buf);
	rows_used = 0;
	for (int row = 0; row < TEXTSCREEN_ROWS; row++) {
		char line[TEXTSCREEN_COLS * 4 + 1];
		size_t len = 0;
		size_t visible = 0; /* length without trailing blanks */
		for (int col = 0; col < TEXTSCREEN_COLS; col++) {
			uint32_t cp = cells[row][col];
			len += encode(cp ? cp : ' ', line + len);
			if (cp && cp != ' ')
				visible = len;
		}
		line[visible] = '\0';
		row_width[row] = 0.0f;
		if (visible == 0)
			continue;
		C2D_TextParse(&row_text[row], text_buf, line);
		C2D_TextOptimize(&row_text[row]);
		C2D_TextGetDimensions(&row_text[row], 1.0f, 1.0f,
		                      &row_width[row], NULL);
		if (row_width[row] <= 0.0f)
			row_width[row] = 1.0f;
		rows_used = row + 1;
	}
	stale = false;
}

void textscreen_draw(void)
{
	if (!text_buf)
		return;
	if (stale)
		reparse();
	dirty = false;
	if (rows_used == 0)
		return;

	float line_height = USABLE_HEIGHT / rows_used;
	if (line_height > MAX_LINE_HEIGHT)
		line_height = MAX_LINE_HEIGHT;
	const FINF_s *info = C2D_FontGetInfo(NULL);
	float line_feed = info && info->lineFeed ? info->lineFeed : 30.0f;
	float scale = line_height / line_feed;

	for (int row = 0; row < rows_used; row++) {
		if (row_width[row] <= 0.0f)
			continue;
		/* Proportional glyphs rarely need it, but a row of wide ones is
		 * squeezed horizontally rather than run off the screen. */
		float scale_x = scale;
		if (row_width[row] * scale_x > USABLE_WIDTH)
			scale_x = USABLE_WIDTH / row_width[row];
		C2D_DrawText(&row_text[row], C2D_WithColor, MARGIN_X,
		             MARGIN_Y + row * line_height, 0.0f, scale_x, scale,
		             row == 0 ? UI_COLOR_ACCENT : UI_COLOR_TEXT);
	}
}
