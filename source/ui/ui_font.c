#include "ui_font.h"

#include "text_compose.h"
#include "worker_priority.h"

#include <3ds.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* citro2d 1.7.0's glyph buffer (source/text.c), which it keeps private.
 * ui_font_start checks it before anything relies on it. */
typedef struct {
	u32 lineNo;
	C3D_Tex *sheet;
	float xPos;
	float width;
	struct {
		float left, top, right, bottom;
	} texcoord;
	u32 wordNo;
} c2d_glyph;

typedef struct {
	u32 reserved[2];
	size_t glyphCount;
	size_t glyphBufSize;
	c2d_glyph glyphs[];
} c2d_text_buf;

/* The system fonts, in citro2d's order: the standard one (Latin, Greek,
 * Cyrillic, Japanese), simplified Chinese, Korean, traditional Chinese. */
enum { FONT_STD, FONT_CHN, FONT_KOR, FONT_TWN, FONT_COUNT };
static const CFG_Region font_region[FONT_COUNT] = {
	CFG_REGION_USA, CFG_REGION_CHN, CFG_REGION_KOR, CFG_REGION_TWN
};
/* Which one goes first when several have a character: for Han
 * characters, simplified Chinese before traditional. */
static const int fallback_order[FONT_COUNT] = {
	FONT_STD, FONT_CHN, FONT_TWN, FONT_KOR
};
/* The order they load in: Korean, the smallest, first, so the Chinese ones
 * are what runs short of memory, if anything does. */
static const int load_order[FONT_COUNT] = {
	FONT_KOR, FONT_STD, FONT_CHN, FONT_TWN
};
/* The system font's own place in a run: not one of the above. */
#define FONT_SYSTEM (-1)

/* Loaded by the worker, used once ui_font_wait has joined it (`ready`).
 * NULL: not loaded, or the console's own font (the system font
 * already). */
static C2D_Font fonts[FONT_COUNT];
static Thread loader;
static bool ready;
/* Whether the glyph buffer is as expected: mixing fonts is safe. */
static bool mixing;

static int region_font(u8 region)
{
	switch (region) {
	case CFG_REGION_CHN:
		return FONT_CHN;
	case CFG_REGION_KOR:
		return FONT_KOR;
	case CFG_REGION_TWN:
		return FONT_TWN;
	default:
		return FONT_STD;
	}
}

/* Reading and unpacking a font from the console's memory touches no GPU
 * state: it can run beside the main loop. */
static void loader_main(void *arg)
{
	(void)arg;
	if (R_FAILED(cfguInit()))
		return;
	u8 region = CFG_REGION_USA;
	CFGU_SecureInfoGetRegion(&region);
	int own = region_font(region);
	/* C2D_FontLoadSystem asks for the region again, with the service
	 * still up. */
	for (int i = 0; i < FONT_COUNT; i++)
		if (load_order[i] != own)
			fonts[load_order[i]] = C2D_FontLoadSystem(
				font_region[load_order[i]]);
	cfguExit();
}

/* Parses a short string with the system font and checks what landed in
 * the buffer against the layout above. */
static bool layout_matches(void)
{
	C2D_TextBuf buf = C2D_TextBufNew(8);
	if (!buf)
		return false;
	C2D_Text text;
	C2D_TextParse(&text, buf, "Ab");
	const c2d_text_buf *view = (const c2d_text_buf *)buf;
	const c2d_glyph *g = view->glyphs;
	bool ok = view->glyphBufSize == 8 && view->glyphCount == 2 &&
	          C2D_TextBufGetNumGlyphs(buf) == 2 && text.begin == 0 &&
	          text.end == 2 && text.lines == 1 && text.words == 1 &&
	          g[0].lineNo == 0 && g[1].lineNo == 0 && g[0].wordNo == 0 &&
	          g[1].wordNo == 0 && g[0].sheet && g[1].sheet &&
	          g[0].width > 0.0f && g[1].xPos > g[0].xPos &&
	          g[0].texcoord.left >= 0.0f && g[0].texcoord.right <= 1.0f &&
	          g[0].xPos + g[0].width <= text.width + 30.0f;
	C2D_TextBufDelete(buf);
	return ok;
}

void ui_font_start(void)
{
	mixing = layout_matches();
	if (!mixing)
		return;
	loader = threadCreate(loader_main, NULL, 16 * 1024,
	                      background_priority(), -2, false);
	if (!loader)
		mixing = false;
}

void ui_font_wait(void)
{
	if (!loader)
		return;
	threadJoin(loader, U64_MAX);
	threadFree(loader);
	loader = NULL;
	ready = true;
}

void ui_font_exit(void)
{
	ui_font_wait();
	for (int i = 0; i < FONT_COUNT; i++) {
		if (fonts[i])
			C2D_FontFree(fonts[i]);
		fonts[i] = NULL;
	}
	ready = false;
}

/* --- Parsing ----------------------------------------------------------- */

static bool has_glyph(C2D_Font font, uint32_t cp)
{
	return C2D_FontGlyphIndexFromCodePoint(font, cp) !=
	       C2D_FontGetInfo(font)->alterCharIndex;
}

/* The font to draw `cp` with: the system font when it has it (or no other
 * font does), else the first loaded one that has it. */
static int font_for(uint32_t cp)
{
	if (cp < 0x80 || !ready || has_glyph(NULL, cp))
		return FONT_SYSTEM;
	for (int i = 0; i < FONT_COUNT; i++) {
		int f = fallback_order[i];
		if (fonts[f] && has_glyph(fonts[f], cp))
			return f;
	}
	return FONT_SYSTEM;
}

static C2D_Font font_ptr(int f)
{
	return f == FONT_SYSTEM ? NULL : fonts[f];
}

static float cell_height(int f)
{
	return C2D_FontGetInfo(font_ptr(f))->tglp->cellHeight;
}

static bool needs_other_fonts(const char *s)
{
	uint32_t cp;
	size_t len;
	while ((len = text_utf8_decode(s, &cp)) > 0) {
		if (font_for(cp) != FONT_SYSTEM)
			return true;
		s += len;
	}
	return false;
}

/* The end of the run of characters drawn with font `f` from `s`: spaces
 * stay with the run they are in. */
static char *run_end(char *s, int f)
{
	uint32_t cp;
	size_t len;
	while ((len = text_utf8_decode(s, &cp)) > 0 && cp != '\n') {
		if (cp != ' ' && font_for(cp) != f)
			break;
		s += len;
	}
	return s;
}

/* Several fonts in one C2D_Text: each run is parsed with its own font into
 * the same buffer, then its glyphs are moved after the runs before it and
 * scaled from its font's units into the system font's, the units the text
 * is drawn in (citro2d scales every font to a 30 px cell). */
static void parse_mixed(C2D_Text *text, C2D_TextBuf buf, char *s)
{
	c2d_text_buf *view = (c2d_text_buf *)buf;
	float system_cell = cell_height(FONT_SYSTEM);
	float system_scale = 30.0f / system_cell;
	text->buf = buf;
	text->font = NULL;
	text->begin = view->glyphCount;
	text->width = 0.0f;
	text->words = 0;
	text->lines = 0;
	for (;;) {
		u32 line = text->lines++;
		float line_width = 0.0f;   /* in pixels at scale 1 */
		u32 line_words = 0;
		while (*s && *s != '\n') {
			uint32_t cp;
			text_utf8_decode(s, &cp);
			int f = cp == ' ' ? FONT_SYSTEM : font_for(cp);
			char *end = run_end(s, f);
			char saved = *end;
			*end = '\0';
			size_t first = view->glyphCount;
			C2D_Text run;
			C2D_TextFontParseLine(&run, font_ptr(f), buf, s, line);
			*end = saved;
			float ratio = system_cell / cell_height(f);
			float offset = line_width / system_scale;
			for (size_t i = first; i < view->glyphCount; i++) {
				c2d_glyph *g = &view->glyphs[i];
				g->xPos = offset + g->xPos * ratio;
				g->width *= ratio;
				g->wordNo += line_words;
			}
			line_width += run.width;
			line_words += run.words;
			s = end;
		}
		text->words += line_words;
		if (line_width > text->width)
			text->width = line_width;
		if (*s != '\n')
			break;
		s++;
	}
	text->end = view->glyphCount;
}

void ui_parse(C2D_Text *text, C2D_TextBuf buf, const char *string)
{
	if (!string)
		string = "";
	/* Composing never needs more than 3 bytes per input byte (a stray
	 * byte becomes U+FFFD). */
	size_t size = strlen(string) * 3 + 1;
	char local[512];
	char *composed = size <= sizeof(local) ? local : malloc(size);
	if (!composed) {
		C2D_TextParse(text, buf, string);
		return;
	}
	text_compose(string, composed, size);
	bool mixed = mixing && ready && needs_other_fonts(composed);
	if (mixed)
		parse_mixed(text, buf, composed);
	else
		C2D_TextParse(text, buf, composed);
	if (composed != local)
		free(composed);
}
