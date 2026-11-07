/* The spinner on the top screen while a track's download is still at 0%:
 * a ring of SPINNER_CELLS square blocks around an empty centre, in a 3 x 3
 * grid of SPINNER_CELL px blocks SPINNER_GAP px apart, flat and square
 * like the rest of Bisagra (no circles, 1.2 and 4.3). One block lit in
 * `clorofila`, the one behind it in `musgo`, the rest `capa_alta`, all with
 * a 1 px `tinta` outline; the lit block moves one place clockwise every
 * LOADER_STEP frames. Behind it, a square of translucent grey (theme.h,
 * an exception to 2.7) lets what is below show through slightly.
 * Pure C with no 3DS headers, tested in tests/native_logic_test.c. */
#ifndef SPINNER_LOGIC_H
#define SPINNER_LOGIC_H

#include "layout.h"
#include "theme.h"

#include <stdbool.h>

enum {
	SPINNER_CELLS = 8,
	SPINNER_SIZE = 3 * SPINNER_CELL + 2 * SPINNER_GAP,
};

/* Block `i` (0..7) clockwise from the top-left corner, for a spinner
 * centred on (cx, cy). */
static inline ui_rect spinner_cell_rect(int cx, int cy, int i)
{
	/* Column and row of each block in the 3 x 3 grid, clockwise. */
	static const signed char col[SPINNER_CELLS] = { 0, 1, 2, 2, 2, 1, 0, 0 };
	static const signed char row[SPINNER_CELLS] = { 0, 0, 0, 1, 2, 2, 2, 1 };
	int pitch = SPINNER_CELL + SPINNER_GAP;
	int left = cx - SPINNER_SIZE / 2;
	int top = cy - SPINNER_SIZE / 2;
	i = ((i % SPINNER_CELLS) + SPINNER_CELLS) % SPINNER_CELLS;
	ui_rect r = { left + col[i] * pitch, top + row[i] * pitch, SPINNER_CELL,
	              SPINNER_CELL };
	return r;
}

/* The translucent grey backdrop behind the ring: SPINNER_PAD px wider on
 * every side. */
static inline ui_rect spinner_backdrop_rect(int cx, int cy)
{
	int size = SPINNER_SIZE + 2 * SPINNER_PAD;
	ui_rect r = { cx - size / 2, cy - size / 2, size, size };
	return r;
}

/* Which block is lit at animation `step`. */
static inline int spinner_lit(int step)
{
	return ((step % SPINNER_CELLS) + SPINNER_CELLS) % SPINNER_CELLS;
}

#endif
