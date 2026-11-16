/* One playable track: from a URI to decrypted Ogg Vorbis in memory.
 *
 * Ties together the three services that each hold one piece: spclient for the
 * metadata and the CDN location, the access point for the decryption key, and
 * the CDN for the bytes. None of them is useful without the other two.
 */
#ifndef TRACK_H
#define TRACK_H

#include "ap3ds.h"
#include "spclient.h"
#include "stream_chunks.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Spotify puts a decoy Ogg page of this length at the start of every audio
 * file; the real Vorbis stream begins right after it.
 *
 * Do not try to find this by searching for the "OggS" magic: the decoy *is* an
 * OggS page, so the first match is the wrong one and the decoder then fails
 * with nothing more useful than "end of stream". */
#define SPOTIFY_OGG_HEADER 167

typedef struct {
	uint8_t *data;           /* decrypted file, including the decoy header */
	size_t   len;
	sp_track meta;
	int      format;         /* which AudioFile.Format was fetched */
	double   resolve_ms;
	double   key_ms;
	double   download_ms;
	double   decrypt_ms;
	int      key_refused_code; /* on AP_KEY_REFUSED: 0..65535, -1 if absent */
} track_audio;

/* Fetches and decrypts one track. `ap` must be a live session: the audio key
 * comes from there and nowhere else.
 *
 * `limit` caps the download; a track that would exceed it fails rather than
 * being truncated into something that decodes to silence halfway through.
 *
 * Returns 0, AP_KEY_REFUSED when Spotify withholds the key for this account,
 * or -1. */
/* `bytes` asks the CDN for only the first N bytes; 0 fetches the whole file.
 * `progress` may be NULL -- without it, four services hide behind one call. */
int  track_fetch(const char *sp_host, const char *token, ap_session *ap,
                 const char *track_uri, size_t bytes, size_t limit,
                 void (*progress)(const char *step),
                 track_audio *out, char *err, size_t errlen);

void track_free(track_audio *t);

/* A track being streamed rather than fetched whole.
 *
 * The file's full size is known from the first reply's headers, so the
 * buffer is reserved complete up front and a download thread fills it
 * while the decoder walks behind it. It fills it chunk by chunk
 * (stream_chunks.h): in order from the start, or from wherever a seek sent
 * it, and once it reaches the end, or chunks it already has, it goes back
 * for the first missing one after where the decoder reads. The two never
 * race over content: a chunk is published only once decrypted, and the
 * decoder reads only what track_stream_available says is there. */
#define TRACK_STREAM_MAX (12 * 1024 * 1024)

typedef struct {
	uint8_t *data;              /* reserved whole file; NULL until the
	                             * total is known */
	size_t   len;               /* the file's full size */
	size_t   total;             /* what the reply headers reported */
	size_t   write_pos;         /* download cursor, download thread only */
	volatile size_t downloaded; /* decrypted watermark of the live span:
	                             * write_pos, published */
	volatile bool dl_done;      /* every chunk is whole */
	volatile bool dl_failed;    /* gave up; dl_err says why */
	bool     cb_fatal;          /* a real error, not worth retrying */
	char     dl_err[160];
	/* track_stream_seek writes an absolute byte offset here; the download
	 * thread aborts the current request and goes on from that chunk.
	 * -1 = no reposition pending. */
	volatile long dl_restart_at;
	/* Which chunks are whole. Written by the download thread only, each
	 * once its bytes are decrypted. */
	volatile uint8_t chunk_done[TRACK_STREAM_MAX / STREAM_CHUNK];
	size_t   mark_next;         /* download thread: the next chunk to mark */
	bool     relocate;          /* download thread: the span reached bytes
	                             * already there, or the end: move on */
	/* Main thread: where the decoder reads. Gaps after it are filled
	 * first. */
	volatile size_t read_hint;

	sp_track   meta;
	int        format;
	uint8_t    key[16];
	sp_storage storage;
	int        key_refused_code; /* on AP_KEY_REFUSED: 0..65535, -1 if absent */

	/* What storage-resolve needs to hand out fresh CDN URLs mid-download:
	 * they are signed and expire, and an edge refuses a stale one. */
	uint8_t    file_id[20];
	char       sp_host[128];
	char       token[1024];

	/* Running AES-CTR state, owned by track_stream_download for its
	 * lifetime: pieces arrive in file order, so one context decrypts them
	 * as they land and the watermark never exposes encrypted bytes. */
	void    *aes;
	uint8_t  nonce[16];
	uint8_t  stream_block[16];
	size_t   nc_off;
	volatile bool *stop;
} track_stream;

/* Metadata, storage-resolve and the audio key: everything that must happen
 * before the first byte is worth downloading. Same steps and order as
 * track_fetch. Returns 0, AP_KEY_REFUSED, or -1. */
int  track_stream_begin(const char *sp_host, const char *token, ap_session *ap,
                        const char *track_uri, void (*progress)(const char *),
                        track_stream *out, char *err, size_t errlen);

/* Same preparation using metadata already loaded by search/queue. This avoids
 * another extended-metadata request before resolving storage and the key. */
int  track_stream_begin_cached(const char *sp_host, const char *token,
                               ap_session *ap, const sp_track *metadata,
                               void (*progress)(const char *),
                               track_stream *out, char *err, size_t errlen);

/* Fills the reserved buffer, retrying across the CDN URLs and resuming each
 * attempt where the last one stopped. Returns early and quietly when `stop`
 * goes up -- that is the UI skipping, not a failure. */
int  track_stream_download(track_stream *ts, volatile bool *stop);
void track_stream_free(track_stream *ts);

/* Main thread: where the bytes the decoder can read from `pos` on end
 * (`pos` itself when none are there yet). */
size_t track_stream_available(const track_stream *ts, size_t pos);
/* Main thread: the decoder goes on reading from `at`. Unless the download
 * is already writing what comes there, it is sent to the first missing
 * chunk from `at` on. */
void track_stream_seek(track_stream *ts, size_t at);
/* Main thread: the decoder reads from `at` now, so the holes after it are
 * filled first; no reposition. */
void track_stream_hint(track_stream *ts, size_t at);

/* Where the Vorbis stream starts, and how long it is. */
static inline const uint8_t *track_vorbis(const track_audio *t)
{
	return t->len > SPOTIFY_OGG_HEADER ? t->data + SPOTIFY_OGG_HEADER : NULL;
}
static inline size_t track_vorbis_len(const track_audio *t)
{
	return t->len > SPOTIFY_OGG_HEADER ? t->len - SPOTIFY_OGG_HEADER : 0;
}

#endif
