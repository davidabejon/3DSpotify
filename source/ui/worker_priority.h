#ifndef WORKER_PRIORITY_H
#define WORKER_PRIORITY_H

#include <3ds.h>

/* A priority just below the calling thread (higher number = lower priority),
 * for every worker that may run while audio plays.
 *
 * Workers share core 0 with the Vorbis decoder on the main thread. Above it,
 * their TLS handshakes, AES-CTR and image decoding starved the decoder: the
 * DSP ran out of queued audio and the track crackled or stopped until the
 * worker finished. Below it, a worker only gets the time the decoder leaves
 * when every wave buffer is full and the main thread waits for VBlank. */
static inline s32 background_priority(void)
{
	s32 priority = 0x30;
	if (R_FAILED(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE)))
		priority = 0x30;
	return priority < 0x3F ? priority + 1 : 0x3F;
}

#endif
