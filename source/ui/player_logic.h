#ifndef PLAYER_LOGIC_H
#define PLAYER_LOGIC_H

#include <stdbool.h>

typedef enum {
	PLAYER_RETURN_RESULTS,
	PLAYER_EXIT_APP,
	PLAYER_TRACK_CHANGED
} playback_result;

static inline int playback_queue_step(int index, int count, int direction)
{
	if (count <= 0 || index < 0 || index >= count ||
	    (direction != -1 && direction != 1))
		return -1;

	int next = index + direction;
	return next >= 0 && next < count ? next : -1;
}

static inline playback_result playback_cancel_result(bool exit_requested,
                                                      bool track_changed)
{
	if (exit_requested)
		return PLAYER_EXIT_APP;
	if (track_changed)
		return PLAYER_TRACK_CHANGED;
	return PLAYER_RETURN_RESULTS;
}

#endif
