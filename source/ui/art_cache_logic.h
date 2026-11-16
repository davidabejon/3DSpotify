/* Choices of the cover cache (art_cache.h) and of the covers kept decoded
 * (artwork.h). Pure C with no 3DS headers, tested in
 * tests/native_logic_test.c. */
#ifndef ART_CACHE_LOGIC_H
#define ART_CACHE_LOGIC_H

#include <stdbool.h>

/* The least recently used entry (the smallest stamp), -1 with none. */
static inline int art_cache_oldest(const bool *used, const unsigned *stamps,
                                   int n)
{
	int oldest = -1;
	for (int i = 0; i < n; i++) {
		/* Stamps grow by one per use: the difference survives their
		 * wrapping round. */
		if (used[i] &&
		    (oldest < 0 || (int)(stamps[i] - stamps[oldest]) < 0))
			oldest = i;
	}
	return oldest;
}

/* The slot a new entry goes to: the first free one, else the least
 * recently used. -1 when `n` is 0. */
static inline int art_cache_victim(const bool *used, const unsigned *stamps,
                                   int n)
{
	for (int i = 0; i < n; i++)
		if (!used[i])
			return i;
	return art_cache_oldest(used, stamps, n);
}

#endif
