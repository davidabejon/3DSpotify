/* Fitting UTF-8 text into a width in pixels. Pure C with no 3DS headers, so
 * the host tests in tests/native_logic_test.c can exercise it with a fake
 * measure.
 *
 * Text that does not fit is cut and ends in "...". The cut always falls
 * on a UTF-8 character boundary, so a multibyte character is never split
 * (citro2d would draw the broken bytes as a replacement glyph). */
#ifndef TEXT_FIT_H
#define TEXT_FIT_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Three dots, as docs/STYLES.md 3.3 asks. */
#define TEXT_FIT_ELLIPSIS "..."

/* Width of a NUL-terminated string at the scale the caller will draw it. */
typedef float (*text_measure_fn)(const char *text, void *ctx);

static inline bool text_fit_is_continuation(unsigned char c)
{
	return (c & 0xC0) == 0x80;
}

/* The largest character boundary at or below `at`. */
static inline size_t text_fit_boundary(const char *text, size_t at)
{
	while (at > 0 && text_fit_is_continuation((unsigned char)text[at]))
		at--;
	return at;
}

/* Builds text[0..len) + ellipsis in `out`, dropping trailing spaces of the
 * prefix. False when it does not fit `outlen`. */
static inline bool text_fit_candidate(const char *text, size_t len,
                                      char *out, size_t outlen)
{
	while (len > 0 && text[len - 1] == ' ')
		len--;
	size_t suffix = sizeof(TEXT_FIT_ELLIPSIS) - 1;
	if (len + suffix + 1 > outlen)
		return false;
	memcpy(out, text, len);
	memcpy(out + len, TEXT_FIT_ELLIPSIS, suffix + 1);
	return true;
}

/* Writes into `out` the text itself when it fits `max_width`, otherwise the
 * longest prefix plus an ellipsis that fits, otherwise "". Returns true when
 * the text was cut. `out` may not alias `text`. Prefix widths are assumed to
 * grow with their length, which holds for any font without kerning. */
static inline bool text_fit(const char *text, float max_width,
                            text_measure_fn measure, void *ctx,
                            char *out, size_t outlen)
{
	if (outlen == 0)
		return false;
	out[0] = '\0';
	if (!text)
		return false;
	size_t len = strlen(text);
	if (len < outlen && measure(text, ctx) <= max_width) {
		memcpy(out, text, len + 1);
		return false;
	}

	/* Binary search over byte lengths, each snapped to a boundary: the
	 * longest prefix whose candidate fits. `lo` always fits (0 = bare
	 * ellipsis, checked first), `hi` never does. */
	char *scratch = out;
	if (!text_fit_candidate(text, 0, scratch, outlen) ||
	    measure(scratch, ctx) > max_width) {
		out[0] = '\0';
		return true;
	}
	size_t lo = 0;
	size_t hi = len;
	while (hi - lo > 1) {
		size_t mid = text_fit_boundary(text, lo + (hi - lo) / 2);
		if (mid <= lo) {
			/* Only continuation bytes between lo and the midpoint: try
			 * the next boundary above instead. */
			mid = lo + 1;
			while (mid < hi &&
			       text_fit_is_continuation((unsigned char)text[mid]))
				mid++;
			if (mid >= hi)
				break;
		}
		if (text_fit_candidate(text, mid, scratch, outlen) &&
		    measure(scratch, ctx) <= max_width)
			lo = mid;
		else
			hi = mid;
	}
	text_fit_candidate(text, lo, out, outlen);
	return true;
}

#endif
