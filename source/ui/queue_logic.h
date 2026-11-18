/* The playback queue's order, decided without console headers so the
 * native tests can check it.
 *
 * The queue's tracks stay in the order of the list they came from; what
 * plays when is `order`, a permutation of their indexes, and the player
 * walks it by position. Shuffling only rewrites `order`, so turning it off
 * finds the list's own order again. */
#ifndef QUEUE_LOGIC_H
#define QUEUE_LOGIC_H

#include "spclient.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* A radio asks for more once this many tracks or fewer are left after the
 * one playing. */
#define QUEUE_RADIO_LEFT 2

/* xorshift32: plenty for a shuffle, and the same on the console and in
 * the tests. A zero state would stay zero, so it never is. */
static inline uint32_t queue_rng_next(uint32_t *state)
{
	uint32_t x = *state ? *state : 0x9E3779B9u;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

static inline void queue_order_identity(int *order, int count)
{
	for (int i = 0; i < count; i++)
		order[i] = i;
}

/* Fisher-Yates over order[from..count-1]; the rest keeps its place. */
static inline void queue_shuffle_tail(int *order, int count, int from,
                                      uint32_t *rng)
{
	if (from < 0)
		from = 0;
	for (int i = count - 1; i > from; i--) {
		int j = from + (int)(queue_rng_next(rng) % (uint32_t)(i - from + 1));
		int t = order[i];
		order[i] = order[j];
		order[j] = t;
	}
}

/* A new queue of `count` tracks that starts at track `track`. In order, the
 * list plays from there, with the earlier tracks behind it for "previous";
 * shuffled, `track` plays first and every other one follows in a random
 * order. Returns the position to play, or -1 for a track outside it. */
static inline int queue_order_start(int *order, int count, int track,
                                    bool shuffle, uint32_t *rng)
{
	if (track < 0 || track >= count)
		return -1;
	queue_order_identity(order, count);
	if (!shuffle)
		return track;
	order[0] = track;
	order[track] = 0;
	queue_shuffle_tail(order, count, 1, rng);
	return 0;
}

/* Shuffle switched while position `pos` plays: the queue starts again from
 * that track, as queue_order_start would. Returns the track's new
 * position. */
static inline int queue_order_reshuffle(int *order, int count, int pos,
                                        bool shuffle, uint32_t *rng)
{
	if (pos < 0 || pos >= count)
		return pos;
	return queue_order_start(order, count, order[pos], shuffle, rng);
}

/* `added` tracks were put at the end of a queue that had `count`: they
 * play after the rest, or, shuffled, anywhere after position `pos`.
 * Returns the new count. */
static inline int queue_order_append(int *order, int count, int added,
                                     int pos, bool shuffle, uint32_t *rng)
{
	for (int i = 0; i < added; i++)
		order[count + i] = count + i;
	count += added;
	if (shuffle && added > 0)
		queue_shuffle_tail(order, count, pos + 1, rng);
	return count;
}

/* The position `direction` (-1 or +1) steps from `pos`, or -1: the queue
 * does not wrap. */
static inline int queue_step(int pos, int count, int direction)
{
	if (count <= 0 || pos < 0 || pos >= count ||
	    (direction != -1 && direction != 1))
		return -1;
	int next = pos + direction;
	return next >= 0 && next < count ? next : -1;
}

/* A radio queue playing position `pos` wants its next batch. */
static inline bool queue_wants_more(int pos, int count)
{
	return pos >= 0 && count - pos - 1 <= QUEUE_RADIO_LEFT;
}

/* Keeps, in their order, the tracks of `fresh` that are neither in `queue`
 * nor earlier in `fresh` (a radio often brings its seed back, and the
 * next batch overlaps the last). Returns how many are left. */
static inline int queue_drop_known(const sp_track *queue, int count,
                                   sp_track *fresh, int n)
{
	int kept = 0;
	for (int i = 0; i < n; i++) {
		bool known = !fresh[i].uri[0];
		for (int j = 0; j < count && !known; j++)
			known = strcmp(queue[j].uri, fresh[i].uri) == 0;
		for (int j = 0; j < kept && !known; j++)
			known = strcmp(fresh[j].uri, fresh[i].uri) == 0;
		if (known)
			continue;
		if (kept != i)
			fresh[kept] = fresh[i];
		kept++;
	}
	return kept;
}

#endif
