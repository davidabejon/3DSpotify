#include "track.h"
#include "http3ds.h"
#include "http_logic.h"
#include "cfg3ds.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/aes.h"

/* Every Spotify audio file uses this IV with the per-file AES-128 key, in
 * counter mode, with the counter running from the start of the file. */
static const uint8_t AUDIO_AESIV[16] = {
	0x72, 0xe0, 0x67, 0xfb, 0xdd, 0xcb, 0xcf, 0x77,
	0xeb, 0xe8, 0xbc, 0x64, 0x3f, 0x63, 0x0d, 0x93,
};

void track_free(track_audio *t)
{
	if (!t)
		return;
	free(t->data);
	t->data = NULL;
	t->len = 0;
}

static bool decrypt(uint8_t *buf, size_t len, const uint8_t key[16])
{
	mbedtls_aes_context aes;
	mbedtls_aes_init(&aes);
	bool ok = mbedtls_aes_setkey_enc(&aes, key, 128) == 0;
	if (ok) {
		uint8_t nonce[16];
		uint8_t stream_block[16];
		size_t nc_off = 0;
		memcpy(nonce, AUDIO_AESIV, sizeof(nonce));
		/* The whole file is one counter run starting at zero, so this single
		 * call is equivalent to decrypting it chunk by chunk in order. */
		ok = mbedtls_aes_crypt_ctr(&aes, len, &nc_off, nonce, stream_block,
		                           buf, buf) == 0;
	}
	mbedtls_aes_free(&aes);
	return ok;
}

int track_fetch(const char *sp_host, const char *token, ap_session *ap,
                const char *track_uri, size_t bytes, size_t limit,
                void (*progress)(const char *step),
                track_audio *out, char *err, size_t errlen)
{
	memset(out, 0, sizeof(*out));
	TickCounter t;

	if (progress) progress("metadata");
	osTickCounterStart(&t);
	if (sp_track_metadata(sp_host, token, track_uri, &out->meta,
	                      err, errlen) != 0)
		return -1;
	if (!out->meta.has_gid) {
		snprintf(err, errlen, "metadata carried no track id");
		return -1;
	}
	const sp_file *file = sp_pick_file(&out->meta);
	if (!file) {
		snprintf(err, errlen, "no Vorbis file this console can play");
		return -1;
	}
	out->format = file->format;

	if (progress) progress("storage-resolve");
	sp_storage storage;
	if (sp_storage_resolve(sp_host, token, file->id, &storage,
	                       err, errlen) < 0)
		return -1;
	osTickCounterUpdate(&t);
	out->resolve_ms = osTickCounterRead(&t);

	/* The key must be asked for before the bytes are worth fetching: without
	 * it the download is just noise. */
	if (progress) progress("audio key");
	osTickCounterStart(&t);
	uint8_t key[16];
	int refused = 0;
	int rc = ap_audio_key(ap, file->id, out->meta.audio_gid, key, &refused,
	                      err, errlen);
	osTickCounterUpdate(&t);
	out->key_ms = osTickCounterRead(&t);
	out->key_refused_code = refused;
	if (rc != 0)
		return rc;

	if (progress) progress("CDN download");
	osTickCounterStart(&t);
	http_reply r;
	int got = -1;
	for (int i = 0; i < storage.count; i++) {
		got = bytes ? http_get_url_range(storage.urls[i], bytes, limit, &r)
		            : http_get_url(storage.urls[i], limit, &r);
		if (got == 0 && (r.status == 200 || r.status == 206))
			break;
		/* A signed URL can have expired or its edge can be down; the other
		 * two serve the same bytes. */
		if (got == 0)
			http_reply_free(&r);
		got = -1;
	}
	osTickCounterUpdate(&t);
	out->download_ms = osTickCounterRead(&t);
	if (got != 0) {
		snprintf(err, errlen, "no CDN url served the file");
		return -1;
	}
	if (r.len <= SPOTIFY_OGG_HEADER) {
		snprintf(err, errlen, "file is only %u bytes", (unsigned)r.len);
		http_reply_free(&r);
		return -1;
	}

	if (progress) progress("decryption");
	osTickCounterStart(&t);
	if (!decrypt((uint8_t *)r.body, r.len, key)) {
		snprintf(err, errlen, "AES-CTR failed");
		http_reply_free(&r);
		return -1;
	}
	osTickCounterUpdate(&t);
	out->decrypt_ms = osTickCounterRead(&t);

	/* Decryption is in place, so the reply's buffer becomes the track's. */
	out->data = (uint8_t *)r.body;
	out->len = r.len;

	if (memcmp(out->data + SPOTIFY_OGG_HEADER, "OggS", 4) != 0) {
		snprintf(err, errlen,
		         "no Ogg stream after the %d byte header -- wrong key?",
		         SPOTIFY_OGG_HEADER);
		track_free(out);
		return -1;
	}
	return 0;
}

static int track_stream_begin_internal(const char *sp_host, const char *token,
                                       ap_session *ap, const char *track_uri,
                                       const sp_track *metadata,
                                       void (*progress)(const char *),
                                       track_stream *out, char *err,
                                       size_t errlen)
{
	memset(out, 0, sizeof(*out));
	out->dl_restart_at = -1;

	if (metadata) {
		if (strcmp(metadata->uri, track_uri) != 0) {
			snprintf(err, errlen, "cached metadata does not match track URI");
			return -1;
		}
		out->meta = *metadata;
	} else {
		/* The key must be asked for before the bytes are worth downloading. */
		if (progress) progress("metadata");
		if (sp_track_metadata(sp_host, token, track_uri, &out->meta,
		                      err, errlen) != 0)
			return -1;
	}
	if (!out->meta.has_gid) {
		snprintf(err, errlen, "metadata carried no track id");
		return -1;
	}
	const sp_file *file = sp_pick_file(&out->meta);
	if (!file) {
		snprintf(err, errlen, "no Vorbis file this console can play");
		return -1;
	}
	out->format = file->format;
	memcpy(out->file_id, file->id, sizeof(out->file_id));
	snprintf(out->sp_host, sizeof(out->sp_host), "%s", sp_host);
	snprintf(out->token, sizeof(out->token), "%s", token);

	if (progress) progress("storage-resolve");
	if (sp_storage_resolve(sp_host, token, file->id, &out->storage,
	                       err, errlen) < 0)
		return -1;

	if (progress) progress("audio key");
	int refused = 0;
	int rc = ap_audio_key(ap, file->id, out->meta.audio_gid, out->key,
	                      &refused, err, errlen);
	out->key_refused_code = refused;
	return rc;
}

int track_stream_begin(const char *sp_host, const char *token, ap_session *ap,
                       const char *track_uri, void (*progress)(const char *),
                       track_stream *out, char *err, size_t errlen)
{
	return track_stream_begin_internal(sp_host, token, ap, track_uri, NULL,
	                                   progress, out, err, errlen);
}

int track_stream_begin_cached(const char *sp_host, const char *token,
                              ap_session *ap, const sp_track *metadata,
                              void (*progress)(const char *),
                              track_stream *out, char *err, size_t errlen)
{
	if (!metadata) {
		snprintf(err, errlen, "cached metadata is required");
		return -1;
	}
	return track_stream_begin_internal(sp_host, token, ap, metadata->uri,
	                                   metadata, progress, out, err, errlen);
}

/* Whether the live span, at write_pos, must stop: it is at the end, or at
 * the start of a chunk that is already whole. */
static bool span_blocked(const track_stream *ts)
{
	if (ts->write_pos >= ts->len)
		return true;
	return ts->write_pos % STREAM_CHUNK == 0 &&
	       ts->chunk_done[ts->write_pos / STREAM_CHUNK];
}

static bool stream_cb(void *ctx, const char *data, size_t n)
{
	track_stream *ts = ctx;
	if (ts->stop && *ts->stop)
		return false;
	if (ts->dl_restart_at >= 0)
		return false;   /* a seek is moving the download; abandon this read */
	if (!ts->data) {
		if (!ts->total || ts->total > TRACK_STREAM_MAX) {
			snprintf(ts->dl_err, sizeof(ts->dl_err),
			         "file size unknown or over the %u MB cap",
			         TRACK_STREAM_MAX / (1024 * 1024));
			ts->cb_fatal = true;
			return false;
		}
		ts->data = malloc(ts->total);
		if (!ts->data) {
			snprintf(ts->dl_err, sizeof(ts->dl_err), "out of memory");
			ts->cb_fatal = true;
			return false;
		}
		/* Zero-filled, so a decoder bug that overran the watermark would
		 * parse as silence instead of as random heap. */
		memset(ts->data, 0, ts->total);
		ts->len = ts->total;
	}
	/* A chunk at a time: the span stops before one that is already whole,
	 * which may be on its way to the decoder (the bytes land encrypted and
	 * are decrypted in place), and at the end of the file. The download
	 * loop then sends it to the next missing chunk. */
	while (n > 0) {
		if (span_blocked(ts)) {
			ts->relocate = true;
			return false;
		}
		size_t piece = STREAM_CHUNK - ts->write_pos % STREAM_CHUNK;
		if (piece > ts->len - ts->write_pos)
			piece = ts->len - ts->write_pos;
		if (piece > n)
			piece = n;
		uint8_t *at = ts->data + ts->write_pos;
		memcpy(at, data, piece);
		if (mbedtls_aes_crypt_ctr(ts->aes, piece, &ts->nc_off, ts->nonce,
		                          ts->stream_block, at, at) != 0) {
			snprintf(ts->dl_err, sizeof(ts->dl_err), "AES-CTR failed");
			ts->cb_fatal = true;
			return false;
		}
		ts->write_pos += piece;
		data += piece;
		n -= piece;
		/* The bytes before the marks that publish them. */
		__sync_synchronize();
		stream_mark(ts->chunk_done, ts->len, ts->write_pos, &ts->mark_next);
		ts->downloaded = ts->write_pos;
	}
	if (span_blocked(ts)) {
		ts->relocate = true;
		return false;
	}
	return true;
}

/* Moves the download cursor to an absolute, 16-byte-aligned file offset.
 * CTR mode is a keystream: the counter for byte N is the IV plus N/16 as a
 * 128-bit big-endian integer, so any offset is reachable without decrypting
 * what came before. */
static void stream_rewind(track_stream *ts, size_t at)
{
	at &= ~(size_t)15;
	if (ts->len && at >= ts->len)
		at = ts->len & ~(size_t)15;
	memcpy(ts->nonce, AUDIO_AESIV, sizeof(ts->nonce));
	uint64_t add = at / 16;
	for (int i = 15; i >= 0 && add; i--) {
		unsigned v = ts->nonce[i] + (unsigned)(add & 0xFF);
		ts->nonce[i] = (uint8_t)v;
		add = (add >> 8) + (v >> 8);
	}
	ts->nc_off = 0;
	ts->write_pos = at;
	ts->mark_next = at / STREAM_CHUNK;
	__sync_synchronize();
	ts->downloaded = at;
}

/* download.log beside the config: one line per attempt of the current
 * track, so a failure seen on the console can be explained afterwards.
 * Hosts and statuses only; never a URL's signature or a token. */
static FILE *dl_log_open(const track_stream *ts)
{
	char path[256];
	cfg_dir(path, sizeof(path));
	size_t n = strlen(path);
	snprintf(path + n, sizeof(path) - n, "download.log");
	FILE *f = fopen(path, "w");
	if (f) {
		fprintf(f, "track %s, %u bytes, %d url(s)\n", ts->meta.uri,
		        (unsigned)ts->len, ts->storage.count);
		fflush(f);
	}
	return f;
}

static void dl_log(FILE *f, const char *fmt, ...)
{
	if (!f)
		return;
	fprintf(f, "[%llu ms] ", (unsigned long long)osGetTime());
	va_list args;
	va_start(args, fmt);
	vfprintf(f, fmt, args);
	va_end(args);
	fputc('\n', f);
	fflush(f);
}

static void url_host(const char *url, char *out, size_t outlen)
{
	const char *p = strncmp(url, "https://", 8) == 0 ? url + 8 : url;
	size_t n = strcspn(p, "/?");
	if (n >= outlen)
		n = outlen - 1;
	memcpy(out, p, n);
	out[n] = '\0';
}

/* Asks storage-resolve again for this file: the edges answer stale signed
 * URLs with a redirect or a refusal, and a resume after a long stall (a
 * closed lid) meets exactly that. */
static bool refresh_urls(track_stream *ts, FILE *log)
{
	if (!ts->token[0] || !ts->sp_host[0])
		return false;
	sp_storage fresh;
	char err[160];
	if (sp_storage_resolve(ts->sp_host, ts->token, ts->file_id, &fresh,
	                       err, sizeof(err)) <= 0) {
		dl_log(log, "refresh failed: %s", err);
		return false;
	}
	ts->storage = fresh;
	dl_log(log, "refreshed: %d url(s)", fresh.count);
	return true;
}

/* Sleeps before the next pass, in slices, so a skip or a seek is not kept
 * waiting. */
static void retry_wait(track_stream *ts, unsigned ms)
{
	for (unsigned waited = 0; waited < ms; waited += 100) {
		if ((ts->stop && *ts->stop) || ts->dl_restart_at >= 0)
			return;
		svcSleepThread(100 * 1000000LL);
	}
}

int track_stream_download(track_stream *ts, volatile bool *stop)
{
	ts->stop = stop;
	mbedtls_aes_context aes;
	mbedtls_aes_init(&aes);
	if (mbedtls_aes_setkey_enc(&aes, ts->key, 128) != 0) {
		snprintf(ts->dl_err, sizeof(ts->dl_err), "AES-CTR setup failed");
		ts->dl_failed = true;
		mbedtls_aes_free(&aes);
		return -1;
	}
	memcpy(ts->nonce, AUDIO_AESIV, sizeof(ts->nonce));
	ts->nc_off = 0;
	ts->aes = &aes;

	/* Passes over the URL list until the file is whole, every attempt
	 * resuming where the last one stopped. A closed lid sleeps the console
	 * and drops Wi-Fi mid-request; on waking, the network takes a while to
	 * come back, so empty passes back off and the download only gives up
	 * after several in a row. Any byte gained, or a reposition, resets the
	 * count. */
	FILE *log = dl_log_open(ts);
	int failures = 0;
	int refreshes = 0;
	while (!ts->dl_done && !ts->cb_fatal && !(stop && *stop)) {
		if (ts->dl_restart_at >= 0) {
			size_t at = stream_chunk_floor((size_t)ts->dl_restart_at);
			ts->dl_restart_at = -1;
			ts->relocate = false;
			stream_rewind(ts, at);
			dl_log(log, "seek to %u", (unsigned)at);
			failures = 0;
			continue;
		}
		/* The span ran into chunks it already has, or the end: on with the
		 * first missing one after where the decoder reads. */
		if (ts->relocate || (ts->len && span_blocked(ts))) {
			ts->relocate = false;
			size_t next = stream_next_missing(ts->chunk_done, ts->len,
			                                  ts->read_hint);
			if (next >= ts->len) {
				ts->dl_done = true;
				continue;
			}
			if (next != ts->write_pos) {
				stream_rewind(ts, next);
				dl_log(log, "fill from %u", (unsigned)next);
			}
		}
		size_t before = ts->write_pos;
		bool refused = false;   /* an edge answered, but not with bytes */
		for (int i = 0; i < ts->storage.count; i++) {
			if ((stop && *stop) || ts->cb_fatal || ts->dl_restart_at >= 0 ||
			    ts->relocate)
				break;
			int status = 0;
			size_t from = ts->write_pos;
			ts->dl_err[0] = '\0';
			int rc = http_get_url_stream(ts->storage.urls[i], from,
			                             stream_cb, ts, &ts->total,
			                             &status, ts->dl_err,
			                             sizeof(ts->dl_err));
			char host[96];
			url_host(ts->storage.urls[i], host, sizeof(host));
			dl_log(log, "url %d %s from %u: rc %d, status %d, got %u%s%s",
			       i, host, (unsigned)from, rc, status,
			       (unsigned)(ts->write_pos - from),
			       rc ? ", " : "", rc ? ts->dl_err : "");
			if (rc != 0 && status != 0 && status != 206 &&
			    ts->write_pos == from)
				refused = true;
			if (ts->relocate || (stop && *stop))
				break;
		}
		/* Moved on by a seek, or by reaching what is there: progress, not
		 * a failure. */
		if (ts->cb_fatal || (stop && *stop) || ts->dl_restart_at >= 0 ||
		    ts->relocate)
			continue;
		if (ts->write_pos != before) {
			failures = 0;
			refreshes = 0;
		} else if (++failures >= DOWNLOAD_MAX_FAILED_PASSES) {
			break;
		}
		/* The network is up but the URLs are refused: fresh ones, then
		 * straight back to it. */
		if (refused && refreshes < DOWNLOAD_MAX_REFRESHES) {
			refreshes++;
			if (refresh_urls(ts, log))
				continue;
		}
		retry_wait(ts, download_retry_delay_ms(failures));
	}
	if (log) {
		dl_log(log, "end: %s, %u of %u bytes%s%s",
		       ts->dl_done ? "done" : (stop && *stop) ? "stopped"
		                                             : "failed",
		       (unsigned)ts->write_pos, (unsigned)ts->len,
		       ts->dl_done ? "" : ", last error: ",
		       ts->dl_done ? "" : ts->dl_err);
		fclose(log);
	}
	ts->aes = NULL;
	mbedtls_aes_free(&aes);

	if (stop && *stop)
		return 0;   /* the UI moved on; not an error */
	if (!ts->dl_done) {
		ts->dl_failed = true;
		if (!ts->dl_err[0])
			snprintf(ts->dl_err, sizeof(ts->dl_err),
			         "no CDN url served the file");
		return -1;
	}
	return 0;
}

/* --- Playing a track from the card --------------------------------------- */

/* Read in blocks the card serves at its speed (probe_offline 0.6: reading
 * dominated decrypting, and 64 KB beat 16 KB). */
#define LOCAL_READ (64 * 1024)

int track_stream_open_local(const char *path, const uint8_t key[16],
                            const sp_track *metadata, int format,
                            track_stream *out, char *err, size_t errlen)
{
	memset(out, 0, sizeof(*out));
	out->dl_restart_at = -1;
	if (metadata)
		out->meta = *metadata;
	out->format = format;
	FILE *f = fopen(path, "rb");
	long size = -1;
	if (f) {
		if (fseek(f, 0, SEEK_END) == 0)
			size = ftell(f);
		fclose(f);
	}
	if (size <= SPOTIFY_OGG_HEADER || size > TRACK_STREAM_MAX) {
		snprintf(err, errlen, size < 0 ? "the downloaded file is missing"
		                               : "the downloaded file is %ld bytes",
		         size);
		return -1;
	}
	out->total = (size_t)size;
	memcpy(out->key, key, sizeof(out->key));
	out->local = true;
	snprintf(out->local_path, sizeof(out->local_path), "%s", path);
	return 0;
}

int track_stream_load_local(track_stream *ts, volatile bool *stop)
{
	ts->stop = stop;
	mbedtls_aes_context aes;
	mbedtls_aes_init(&aes);
	uint8_t *buf = malloc(LOCAL_READ);
	FILE *f = fopen(ts->local_path, "rb");
	if (!buf || !f || mbedtls_aes_setkey_enc(&aes, ts->key, 128) != 0) {
		snprintf(ts->dl_err, sizeof(ts->dl_err), "%s",
		         !buf ? "out of memory" : !f ? "the downloaded file is missing"
		                                     : "AES-CTR setup failed");
		ts->dl_failed = true;
		free(buf);
		if (f)
			fclose(f);
		mbedtls_aes_free(&aes);
		return -1;
	}
	memcpy(ts->nonce, AUDIO_AESIV, sizeof(ts->nonce));
	ts->nc_off = 0;
	ts->aes = &aes;

	/* The download loop with the card for a network: stream_cb decrypts
	 * and publishes, stops at a chunk already whole or at the end, and a
	 * seek moves the cursor. Only a read that brings nothing is fatal. */
	while (!ts->dl_done && !ts->cb_fatal && !(stop && *stop)) {
		if (ts->dl_restart_at >= 0) {
			size_t at = stream_chunk_floor((size_t)ts->dl_restart_at);
			ts->dl_restart_at = -1;
			ts->relocate = false;
			stream_rewind(ts, at);
			continue;
		}
		if (ts->relocate || (ts->len && span_blocked(ts))) {
			ts->relocate = false;
			size_t next = stream_next_missing(ts->chunk_done, ts->len,
			                                  ts->read_hint);
			if (next >= ts->len) {
				ts->dl_done = true;
				continue;
			}
			if (next != ts->write_pos)
				stream_rewind(ts, next);
		}
		size_t want = ts->total - ts->write_pos;
		if (want > LOCAL_READ)
			want = LOCAL_READ;
		size_t got = fseek(f, (long)ts->write_pos, SEEK_SET) == 0
		             ? fread(buf, 1, want, f) : 0;
		if (got == 0) {
			snprintf(ts->dl_err, sizeof(ts->dl_err),
			         "the downloaded file ends at %u",
			         (unsigned)ts->write_pos);
			break;
		}
		stream_cb(ts, (const char *)buf, got);
	}
	fclose(f);
	memset(buf, 0, LOCAL_READ);
	free(buf);
	ts->aes = NULL;
	mbedtls_aes_free(&aes);

	if (stop && *stop)
		return 0;
	if (!ts->dl_done) {
		ts->dl_failed = true;
		return -1;
	}
	return 0;
}

/* --- Saving a track to the card ------------------------------------------ */

/* The .part is written in blocks the card takes at its speed
 * (probe_offline 0.3: 64 KB was the fastest). */
#define SAVE_BUF (64 * 1024)
/* Passes over the URLs that bring nothing before giving up; the engine
 * then waits and tries again (downloads_logic.h, dl_net_retry_ms). */
#define SAVE_MAX_FAILED_PASSES 3
#define SAVE_MAX_REFRESHES 2

typedef struct {
	FILE *f;
	volatile bool *stop;
	volatile uint32_t *done;
	volatile uint32_t *total_out;
	size_t   total;            /* the reply headers', set before data */
	uint32_t have;             /* bytes in the .part */
	uint64_t free_bytes;
	bool     checked;          /* size and space looked at, this request */
	bool     reset;            /* the .part is longer than the file */
	int      fail;             /* TRACK_SAVE_NO_SPACE or TRACK_SAVE_WRITE */
	char    *err;
	size_t   errlen;
} save_ctx;

static bool save_cb(void *p, const char *data, size_t n)
{
	save_ctx *c = p;
	if (c->stop && *c->stop)
		return false;
	if (!c->checked) {
		c->checked = true;
		if (!c->total || c->total > TRACK_STREAM_MAX) {
			snprintf(c->err, c->errlen, "file size unknown or over the %u MB cap",
			         TRACK_STREAM_MAX / (1024 * 1024));
			c->fail = -1;
			return false;
		}
		if (c->have > c->total) {
			c->reset = true;
			return false;
		}
		uint64_t need = (uint64_t)(c->total - c->have) + TRACK_SAVE_MARGIN;
		if (c->free_bytes < need) {
			snprintf(c->err, c->errlen, "not enough space on the SD card");
			c->fail = TRACK_SAVE_NO_SPACE;
			return false;
		}
		*c->total_out = (uint32_t)c->total;
	}
	if (c->have + n > c->total) {
		c->reset = true;
		return false;
	}
	if (fwrite(data, 1, n, c->f) != n) {
		snprintf(c->err, c->errlen, "could not write to the SD card");
		c->fail = TRACK_SAVE_WRITE;
		return false;
	}
	c->have += (uint32_t)n;
	*c->done = c->have;
	return true;
}

static void save_wait(volatile bool *stop, unsigned ms)
{
	for (unsigned waited = 0; waited < ms; waited += 100) {
		if (stop && *stop)
			return;
		svcSleepThread(100 * 1000000LL);
	}
}

static FILE *save_open(const char *path, bool truncate, uint32_t *have)
{
	FILE *f = fopen(path, truncate ? "wb" : "ab");
	if (!f)
		return NULL;
	setvbuf(f, NULL, _IOFBF, SAVE_BUF);
	long at = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
	*have = at > 0 ? (uint32_t)at : 0;
	return f;
}

int track_save_download(track_stream *ts, const char *part_path,
                        volatile bool *stop, volatile uint32_t *done,
                        volatile uint32_t *total, uint64_t free_bytes)
{
	save_ctx c;
	memset(&c, 0, sizeof(c));
	c.stop = stop;
	c.done = done;
	c.total_out = total;
	c.free_bytes = free_bytes;
	c.err = ts->dl_err;
	c.errlen = sizeof(ts->dl_err);
	ts->dl_err[0] = '\0';
	c.f = save_open(part_path, false, &c.have);
	if (!c.f) {
		snprintf(ts->dl_err, sizeof(ts->dl_err),
		         "could not write to the SD card");
		return TRACK_SAVE_WRITE;
	}
	*done = c.have;
	*total = 0;

	int failures = 0, refreshes = 0, rc = -1;
	bool whole = false;
	while (!whole) {
		if (stop && *stop) {
			rc = TRACK_SAVE_STOPPED;
			break;
		}
		uint32_t before = c.have;
		bool refused = false;
		for (int i = 0; i < ts->storage.count && !whole; i++) {
			if (stop && *stop)
				break;
			int status = 0;
			uint32_t from = c.have;
			c.checked = false;
			c.reset = false;
			int got = http_get_url_stream(ts->storage.urls[i], from, save_cb,
			                              &c, &c.total, &status, ts->dl_err,
			                              sizeof(ts->dl_err));
			if (c.fail)
				break;
			if (c.reset) {
				/* Not this file's bytes: start it over. */
				fclose(c.f);
				c.f = save_open(part_path, true, &c.have);
				if (!c.f) {
					c.fail = TRACK_SAVE_WRITE;
					break;
				}
				*done = 0;
				continue;
			}
			if (got == 0 && c.total && c.have == c.total)
				whole = true;
			/* A whole .part from a run that stopped before renaming it:
			 * nothing is left to ask for. The check after says if it is
			 * right. */
			else if (status == 416 && from > 0) {
				c.total = from;
				*total = from;
				whole = true;
			} else if (got != 0 && status != 0 && status != 200 &&
			           status != 206 && c.have == from) {
				refused = true;
			}
		}
		if (c.fail) {
			rc = c.fail;
			/* Not a file this client keeps: retrying will not change it. */
			ts->cb_fatal = c.fail == -1;
			break;
		}
		if (whole || (stop && *stop))
			continue;
		if (c.have != before) {
			failures = 0;
			refreshes = 0;
		} else if (++failures >= SAVE_MAX_FAILED_PASSES) {
			if (!ts->dl_err[0])
				snprintf(ts->dl_err, sizeof(ts->dl_err),
				         "no CDN url served the file");
			break;
		}
		if (refused && refreshes < SAVE_MAX_REFRESHES) {
			refreshes++;
			if (refresh_urls(ts, NULL))
				continue;
		}
		save_wait(stop, 1000u << (failures < 3 ? failures : 3));
	}
	if (whole)
		rc = 0;
	if (c.f && fclose(c.f) != 0 && rc == 0) {
		snprintf(ts->dl_err, sizeof(ts->dl_err),
		         "could not write to the SD card");
		rc = TRACK_SAVE_WRITE;
	}
	return rc;
}

bool track_verify_file(const char *path, uint32_t size, const uint8_t key[16])
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	uint8_t head[SPOTIFY_OGG_HEADER + 4];
	bool ok = fseek(f, 0, SEEK_END) == 0 && ftell(f) == (long)size &&
	          size > sizeof(head) && fseek(f, 0, SEEK_SET) == 0 &&
	          fread(head, 1, sizeof(head), f) == sizeof(head);
	fclose(f);
	ok = ok && decrypt(head, sizeof(head), key) &&
	     memcmp(head + SPOTIFY_OGG_HEADER, "OggS", 4) == 0;
	memset(head, 0, sizeof(head));
	return ok;
}

void track_stream_free(track_stream *ts)
{
	if (!ts)
		return;
	free(ts->data);
	ts->data = NULL;
	ts->len = ts->total = ts->write_pos = 0;
	ts->downloaded = 0;
	memset(ts->key, 0, sizeof(ts->key));
	memset(ts->token, 0, sizeof(ts->token));
	memset(ts->nonce, 0, sizeof(ts->nonce));
	memset(ts->stream_block, 0, sizeof(ts->stream_block));
}

size_t track_stream_available(const track_stream *ts, size_t pos)
{
	if (!ts->data || !ts->len)
		return pos;
	size_t live = ts->downloaded;
	size_t end = stream_available(ts->chunk_done, ts->len, live, pos);
	/* The marks and the watermark before the bytes they publish. */
	__sync_synchronize();
	return end;
}

void track_stream_hint(track_stream *ts, size_t at)
{
	ts->read_hint = at;
}

void track_stream_seek(track_stream *ts, size_t at)
{
	ts->read_hint = at;
	if (!ts->data || !ts->len || ts->dl_done)
		return;
	/* What the decoder needs first: the first missing chunk from `at`.
	 * Left alone when the live span is about to write it. */
	size_t need = stream_next_missing(ts->chunk_done, ts->len, at);
	if (need >= ts->len)
		return;
	size_t live = ts->downloaded;
	if (live >= need && live < need + STREAM_CHUNK)
		return;
	ts->dl_restart_at = (long)need;
}
