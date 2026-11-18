/* Word wrap for the few texts that are read as a paragraph rather than as
 * one line per fact: an artist's biography on its card (STYLES.md 7.12).
 * Pure C, measured through the same callback as text_fit.h, so the host
 * tests can measure with a fake font. Run when the text is parsed, never
 * per frame (STYLES.md 9). */
#ifndef TEXT_WRAP_H
#define TEXT_WRAP_H

#include "text_fit.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* The longest line kept, in bytes: far more than 200 px of t_meta. */
#define TEXT_WRAP_LINE_MAX 160

/* Whether text[0..len) fits `max_width`, measured in `scratch`. */
static inline bool text_wrap_fits(const char *text, size_t len,
                                  float max_width, text_measure_fn measure,
                                  void *ctx, char *scratch)
{
	if (len >= TEXT_WRAP_LINE_MAX)
		return false;
	memcpy(scratch, text, len);
	scratch[len] = '\0';
	return measure(scratch, ctx) <= max_width;
}

/* Greedy word wrap of UTF-8 `text` into at most `max_lines` lines no wider
 * than `max_width`. A word wider than a line is cut between characters.
 * When text is left over, the last line ends in "..." (text_fit). Spaces
 * where a line breaks are dropped. Returns the lines written, each
 * NUL-terminated in lines[i]. */
static inline int text_wrap(const char *text, float max_width, int max_lines,
                            text_measure_fn measure, void *ctx,
                            char (*lines)[TEXT_WRAP_LINE_MAX])
{
	int n = 0;
	if (!text || max_lines <= 0)
		return 0;
	const char *p = text;
	while (*p == ' ')
		p++;
	while (*p && n < max_lines) {
		char *out = lines[n];
		size_t rest = strlen(p);
		if (text_wrap_fits(p, rest, max_width, measure, ctx, out)) {
			n++;
			break;
		}
		if (n == max_lines - 1) {
			/* The last line: whole words and "...", or, when not even one
			 * word fits with it, as many characters (text_fit). */
			size_t words = 0;
			for (size_t i = 1; i < rest && i + 4 < TEXT_WRAP_LINE_MAX;
			     i++) {
				if (p[i] != ' ' || p[i - 1] == ' ')
					continue;
				memcpy(out, p, i);
				memcpy(out + i, TEXT_FIT_ELLIPSIS,
				       sizeof(TEXT_FIT_ELLIPSIS));
				if (measure(out, ctx) > max_width)
					break;
				words = i;
			}
			if (words) {
				memcpy(out, p, words);
				memcpy(out + words, TEXT_FIT_ELLIPSIS,
				       sizeof(TEXT_FIT_ELLIPSIS));
			} else {
				text_fit(p, max_width, measure, ctx, out,
				         TEXT_WRAP_LINE_MAX);
			}
			n++;
			break;
		}
		/* The longest run of whole words that fits: widths grow with
		 * length, so the first break that does not fit ends the search. */
		size_t best = 0;
		for (size_t i = 1; i < rest && i < TEXT_WRAP_LINE_MAX; i++) {
			if (p[i] != ' ' || p[i - 1] == ' ')
				continue;
			if (!text_wrap_fits(p, i, max_width, measure, ctx, out))
				break;
			best = i;
		}
		if (best == 0) {
			/* One word wider than the line: as many characters as fit,
			 * and at least one, so the wrap always moves on. */
			size_t i = text_fit_boundary(p, rest < TEXT_WRAP_LINE_MAX - 1
			                                 ? rest : TEXT_WRAP_LINE_MAX - 1);
			while (i > 0 &&
			       !text_wrap_fits(p, i, max_width, measure, ctx, out))
				i = text_fit_boundary(p, i - 1);
			if (i == 0) {
				i = 1;
				while (i < rest && text_fit_is_continuation(
				                       (unsigned char)p[i]))
					i++;
			}
			best = i;
		}
		memcpy(out, p, best);
		out[best] = '\0';
		n++;
		p += best;
		while (*p == ' ')
			p++;
	}
	return n;
}

#endif
