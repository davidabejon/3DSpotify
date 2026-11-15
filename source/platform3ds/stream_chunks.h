/* Which parts of a streamed track are already downloaded, decided without
 * console headers so the native tests can check it.
 *
 * The file is cut into STREAM_CHUNK-byte chunks. A download span always
 * starts at a chunk's first byte and writes forward, so a chunk is either
 * whole (marked done), or being written from its start by the live span,
 * or not worth reading at all. That is what lets a seek jump ahead, and the
 * download go back for the holes afterwards, without the decoder ever
 * reading bytes that are not there.
 *
 * `done[c]` is 1 once chunk c is whole; `live` is the absolute offset the
 * live span has written up to (track_stream.downloaded). */
#ifndef STREAM_CHUNKS_H
#define STREAM_CHUNKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STREAM_CHUNK (16 * 1024)

/* How many chunks a file of `len` bytes has. */
static inline size_t stream_chunk_count(size_t len)
{
	return (len + STREAM_CHUNK - 1) / STREAM_CHUNK;
}

/* The first byte of the chunk holding `pos`. */
static inline size_t stream_chunk_floor(size_t pos)
{
	return pos - pos % STREAM_CHUNK;
}

/* Where the bytes readable from `pos` on end: through every whole chunk
 * from `pos`'s, then into the one the live span is writing, if it is the
 * next. Returns `pos` itself when nothing there is readable yet. */
static inline size_t stream_available(const volatile uint8_t *done,
                                      size_t len, size_t live, size_t pos)
{
	if (pos >= len)
		return pos;
	size_t n = stream_chunk_count(len);
	size_t c = pos / STREAM_CHUNK;
	while (c < n && done[c])
		c++;
	size_t end = c * STREAM_CHUNK;
	if (end > len)
		end = len;
	if (c < n && live > end && live <= end + STREAM_CHUNK)
		end = live > len ? len : live;
	return end > pos ? end : pos;
}

/* The first byte of the first chunk not whole at or after `pos`, looking
 * past the end back to the start; `len` when every chunk is whole. */
static inline size_t stream_next_missing(const volatile uint8_t *done,
                                         size_t len, size_t pos)
{
	size_t n = stream_chunk_count(len);
	if (n == 0)
		return len;
	size_t first = pos < len ? pos / STREAM_CHUNK : 0;
	for (size_t k = 0; k < n; k++) {
		size_t c = (first + k) % n;
		if (!done[c])
			return c * STREAM_CHUNK;
	}
	return len;
}

/* The live span has written up to `written`: marks every chunk it has
 * finished from `*next` (the next one to mark: the chunk the span started
 * at, then onwards), the last, shorter one included. */
static inline void stream_mark(volatile uint8_t *done, size_t len,
                               size_t written, size_t *next)
{
	size_t n = stream_chunk_count(len);
	while (*next < n) {
		size_t end = (*next + 1) * STREAM_CHUNK;
		if (end > len)
			end = len;
		if (written < end)
			break;
		done[*next] = 1;
		(*next)++;
	}
}

#endif
