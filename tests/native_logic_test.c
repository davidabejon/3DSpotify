#include "player_logic.h"
#include "search_path.h"

#include <assert.h>
#include <string.h>

static void test_search_path_encoding(void)
{
	char path[448];
	assert(sp_search_path("Beyonce Halo", path, sizeof(path)));
	assert(strcmp(path,
	              "/context-resolve/v1/spotify:search:Beyonce+Halo") == 0);
	assert(sp_search_path("rock + roll/100%", path, sizeof(path)));
	assert(strcmp(path,
	              "/context-resolve/v1/spotify:search:rock+%2B+roll%2F100%25")
	       == 0);
	assert(sp_search_path("canci\xc3\xb3n", path, sizeof(path)));
	assert(strcmp(path,
	              "/context-resolve/v1/spotify:search:canci%C3%B3n") == 0);
	assert(!sp_search_path("", path, sizeof(path)));
	assert(!sp_search_path("test", path, 4));
}

static void test_queue_boundaries(void)
{
	assert(playback_queue_step(0, 3, -1) == -1);
	assert(playback_queue_step(0, 3, 1) == 1);
	assert(playback_queue_step(1, 3, -1) == 0);
	assert(playback_queue_step(1, 3, 1) == 2);
	assert(playback_queue_step(2, 3, 1) == -1);
	assert(playback_queue_step(0, 0, 1) == -1);
	assert(playback_queue_step(3, 3, -1) == -1);
	assert(playback_queue_step(1, 3, 0) == -1);
}

static void test_playback_cancel_transitions(void)
{
	assert(playback_cancel_result(false, false) == PLAYER_RETURN_RESULTS);
	assert(playback_cancel_result(false, true) == PLAYER_TRACK_CHANGED);
	assert(playback_cancel_result(true, false) == PLAYER_EXIT_APP);
	assert(playback_cancel_result(true, true) == PLAYER_EXIT_APP);
}

int main(void)
{
	test_search_path_encoding();
	test_queue_boundaries();
	test_playback_cancel_transitions();
	return 0;
}
