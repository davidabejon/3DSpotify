/* Downloads and offline mode: what the design takes for granted, measured on
 * the console before any of it is written.
 *
 * One step per question, offline ones first so that a run in flight mode
 * still answers them (and is itself the answer to the Wi-Fi question):
 *
 *   0.5  does AC say "no Wi-Fi" at once, with no timeout to wait out?
 *   0.4  are the LFCS and the PS device id stable across launches? The key
 *        that wraps stored audio keys is derived from them.
 *   0.7  free space on the SD card from FSUSER_GetArchiveResource.
 *   0.3  fwrite speed in 16/32/64 KB blocks from a background thread, and
 *        whether the main loop misses frames while it runs.
 *   0.6  AES-128-CTR in software over ~5 MB in 16 KB blocks, in memory and
 *        read back from the SD: can local playback decrypt as it reads?
 *   F1   the downloads store (dl_store.c) on the card: a full index saved
 *        and loaded, keys wrapped and unwrapped, a cut save recovered.
 *   0.1  two access point sessions with the same device id at once: do they
 *        coexist, or does the second one push the first out?
 *   0.2  how many audio keys in a row before the AP refuses, how fast that
 *        allowance refills, and whether a pace derived from it holds.
 *
 * Nothing secret reaches the screen or the log: no token, no key, and the
 * console ids only as a short hash.
 */
#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/aes.h"
#include "mbedtls/sha256.h"

#include "ap3ds.h"
#include "cfg3ds.h"
#include "http3ds.h"
#include "login5.h"
#include "pair3ds.h"
#include "spclient.h"
#include "tls3ds.h"
#include "dl_store.h"

#include <sys/stat.h>

#define SOC_BUFFER      0x100000

#define SAMPLE_TRACK    "spotify:track:3n3Ppam7vgaVa1iaRUc9Lp"

/* 0.3: what is written per block size. Four megabytes is a typical track at
 * 160 kbps. */
#define WRITE_TOTAL     (4 * 1024 * 1024)
#define WRITE_FILE      "probe_offline.bin"
/* 0.6: the in-memory pass, about one five-minute track. */
#define DECRYPT_TOTAL   (5 * 1024 * 1024)
#define DECRYPT_BLOCK   (16 * 1024)
/* What playback consumes at 160 kbps, to say how far ahead decryption is. */
#define PLAY_BYTES_PER_S 20000.0

/* 0.1: how long both sessions are watched for the AP pushing one out. */
#define WATCH_MS        60000
/* 0.2: keys asked back to back, then with a pause if that failed. */
#define KEY_BURST       60
/* The paced round of 0.2: how many keys, the bounds of the gap between
 * them, and the rest before it so it starts with the bucket refilling. */
#define KEY_PACE_COUNT  12
#define KEY_PACE_MIN_MS 3000
#define KEY_PACE_MAX_MS 60000
#define KEY_PACE_REST_MS 60000

#define IDS_FILE        "probe_offline_ids.txt"

static FILE *logf;

static void say(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fputs(line, stdout);
	fputs(line, stderr);
	if (logf) {
		fputs(line, logf);
		fflush(logf);
	}
}

static void open_log(const char *argv0)
{
	if (argv0 && argv0[0]) {
		char p[320];
		snprintf(p, sizeof(p), "%s", argv0);
		char *slash = strrchr(p, '/');
		if (slash) {
			slash[1] = '\0';
			strncat(p, "spotify-offline.log", sizeof(p) - strlen(p) - 1);
			logf = fopen(p, "w");
		}
	}
	if (!logf)
		logf = fopen("sdmc:/spotify-offline.log", "w");
}

static void frame(void)
{
	gfxFlushBuffers();
	gfxSwapBuffers();
	gspWaitForVBlank();
}

static double now_ms(void)
{
	return (double)svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000.0);
}

static void wait_ms(int ms)
{
	double end = now_ms() + ms;
	while (now_ms() < end && aptMainLoop())
		frame();
}

/* One above the main thread, as the app's workers run. */
static s32 background_priority(void)
{
	s32 priority = 0x30;
	if (R_FAILED(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE)))
		priority = 0x30;
	return priority < 0x3F ? priority + 1 : 0x3F;
}

static void to_hex(const uint8_t *p, size_t n, char *out)
{
	static const char HEX[] = "0123456789abcdef";
	for (size_t i = 0; i < n; i++) {
		out[i * 2] = HEX[p[i] >> 4];
		out[i * 2 + 1] = HEX[p[i] & 15];
	}
	out[n * 2] = '\0';
}

static void data_path(const char *name, char *out, size_t outlen)
{
	char dir[256];
	cfg_dir(dir, sizeof(dir));
	snprintf(out, outlen, "%s%s", dir[0] ? dir : "sdmc:/", name);
}

/* ------------------------------------------------- background job + frames */

/* A job runs on a background thread while the main loop keeps presenting
 * frames, the way the app's workers run beside the UI. The worst gap
 * between two frames is what a user would see as a stutter. */
typedef struct {
	void (*fn)(void *);
	void *arg;
	volatile bool done;
} bg_job;

typedef struct {
	double total_ms;
	double worst_frame_ms;
	int    frames;
	int    late_frames;   /* more than one vblank apart */
} bg_stats;

static void bg_entry(void *p)
{
	bg_job *job = p;
	job->fn(job->arg);
	job->done = true;
}

static bool run_in_background(void (*fn)(void *), void *arg, bg_stats *st)
{
	memset(st, 0, sizeof(*st));
	static bg_job job;
	job.fn = fn;
	job.arg = arg;
	job.done = false;
	double start = now_ms();
	Thread t = threadCreate(bg_entry, &job, 64 * 1024,
	                        background_priority(), -2, false);
	if (!t) {
		say("   threadCreate failed\n");
		return false;
	}
	double last = now_ms();
	while (!job.done) {
		frame();
		double at = now_ms();
		double gap = at - last;
		last = at;
		st->frames++;
		if (gap > st->worst_frame_ms)
			st->worst_frame_ms = gap;
		if (gap > 25.0)
			st->late_frames++;
	}
	st->total_ms = now_ms() - start;
	threadJoin(t, U64_MAX);
	threadFree(t);
	return true;
}

static void say_frames(const bg_stats *st)
{
	say("   main loop: %d frames, worst gap %.1f ms, %d late\n",
	    st->frames, st->worst_frame_ms, st->late_frames);
}

/* ------------------------------------------------------------------ 0.5 */

/* Returns whether AC reports a connection with Internet access. */
static bool step_wifi(void)
{
	say("0.5 Wi-Fi status from AC\n");
	double t0 = now_ms();
	Result rc = acInit();
	double t1 = now_ms();
	if (R_FAILED(rc)) {
		say("   acInit failed: 0x%08lX (%.1f ms)\n", (unsigned long)rc, t1 - t0);
		return false;
	}
	say("   acInit %.1f ms\n", t1 - t0);

	u32 wifi = 0, status = 0;
	t0 = now_ms();
	rc = ACU_GetWifiStatus(&wifi);
	t1 = now_ms();
	if (R_FAILED(rc))
		say("   ACU_GetWifiStatus failed: 0x%08lX (%.1f ms)\n",
		    (unsigned long)rc, t1 - t0);
	else
		say("   ACU_GetWifiStatus = %lu (%.1f ms)  [0 none, 1 O3DS, 2 N3DS]\n",
		    (unsigned long)wifi, t1 - t0);

	t0 = now_ms();
	Result rc2 = ACU_GetStatus(&status);
	t1 = now_ms();
	if (R_FAILED(rc2))
		say("   ACU_GetStatus failed: 0x%08lX (%.1f ms)\n",
		    (unsigned long)rc2, t1 - t0);
	else
		say("   ACU_GetStatus = %lu (%.1f ms)\n", (unsigned long)status,
		    t1 - t0);
	acExit();

	bool up = R_SUCCEEDED(rc) && wifi != 0;
	say("   -> %s\n\n", up ? "connected" : "NO Wi-Fi");
	return up;
}

/* ------------------------------------------------------------------ 0.4 */

static void step_ids(void)
{
	say("0.4 console ids (PS)\n");
	u64 lfcs = 0;
	u32 devid = 0;
	Result r1 = PS_GetLocalFriendCodeSeed(&lfcs);
	Result r2 = PS_GetDeviceId(&devid);
	say("   PS_GetLocalFriendCodeSeed: %s\n",
	    R_SUCCEEDED(r1) ? "ok" : "FAILED");
	say("   PS_GetDeviceId: %s\n", R_SUCCEEDED(r2) ? "ok" : "FAILED");
	if (R_FAILED(r1) || R_FAILED(r2)) {
		say("   (0x%08lX, 0x%08lX)\n\n", (unsigned long)r1, (unsigned long)r2);
		return;
	}
	if (lfcs == 0 || devid == 0)
		say("   WARNING: a value is zero (lfcs %s, device id %s)\n",
		    lfcs ? "set" : "0", devid ? "set" : "0");

	/* Only a hash leaves this function: the ids identify the console. */
	uint8_t in[12], digest[32];
	memcpy(in, &lfcs, 8);
	memcpy(in + 8, &devid, 4);
	mbedtls_sha256(in, sizeof(in), digest, 0);
	char fp[17];
	to_hex(digest, 8, fp);
	const char *kind = envIsHomebrew() ? "3dsx" : "cia";
	say("   fingerprint %s (%s)\n", fp, kind);

	char path[300];
	data_path(IDS_FILE, path, sizeof(path));
	char prev[64] = "";
	FILE *f = fopen(path, "r");
	if (f) {
		if (!fgets(prev, sizeof(prev), f))
			prev[0] = '\0';
		fclose(f);
		prev[strcspn(prev, "\r\n")] = '\0';
	}
	char line[64];
	snprintf(line, sizeof(line), "%s %s", fp, kind);
	if (!prev[0])
		say("   first run: saved, run again to compare\n");
	else if (strncmp(prev, fp, 16) == 0)
		say("   SAME as the last run (%s)\n", prev);
	else
		say("   DIFFERENT from the last run (%s)\n", prev);
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "%s\n", line);
		fclose(f);
	}
	say("\n");
}

/* ------------------------------------------------------------------ 0.7 */

static bool sd_space(u64 *free_bytes, u64 *total_bytes, double *ms)
{
	FS_ArchiveResource res;
	double t0 = now_ms();
	Result rc = FSUSER_GetArchiveResource(&res, SYSTEM_MEDIATYPE_SD);
	*ms = now_ms() - t0;
	if (R_FAILED(rc)) {
		say("   FSUSER_GetArchiveResource failed: 0x%08lX\n", (unsigned long)rc);
		return false;
	}
	*free_bytes = (u64)res.freeClusters * res.clusterSize;
	*total_bytes = (u64)res.totalClusters * res.clusterSize;
	return true;
}

static bool step_free(const char *when, u64 *free_out)
{
	u64 freeb = 0, total = 0;
	double ms;
	if (!sd_space(&freeb, &total, &ms))
		return false;
	say("   %s: %.1f MB free of %.1f MB (%.1f ms)\n", when,
	    freeb / 1048576.0, total / 1048576.0, ms);
	if (free_out)
		*free_out = freeb;
	return true;
}

/* ------------------------------------------------------------------ 0.3 */

typedef struct {
	char   path[300];
	size_t block;
	uint8_t *buf;
	double write_ms;
	double worst_write_ms;
	double close_ms;
	bool   ok;
	char   err[96];
} write_job;

static void write_fn(void *p)
{
	write_job *w = p;
	w->ok = false;
	FILE *f = fopen(w->path, "wb");
	if (!f) {
		snprintf(w->err, sizeof(w->err), "fopen failed");
		return;
	}
	double t0 = now_ms();
	for (size_t at = 0; at < WRITE_TOTAL; at += w->block) {
		double a = now_ms();
		size_t n = fwrite(w->buf, 1, w->block, f);
		double d = now_ms() - a;
		if (d > w->worst_write_ms)
			w->worst_write_ms = d;
		if (n != w->block) {
			snprintf(w->err, sizeof(w->err), "short write at %u",
			         (unsigned)at);
			fclose(f);
			return;
		}
	}
	w->write_ms = now_ms() - t0;
	double c = now_ms();
	fclose(f);
	w->close_ms = now_ms() - c;
	w->ok = true;
}

/* Leaves the last file written (64 KB blocks) for 0.6 to read back. */
static bool step_sd_write(char *file_out, size_t outlen)
{
	say("0.3 SD write from a background thread, %d MB per pass\n",
	    WRITE_TOTAL / 1048576);
	static const size_t BLOCKS[] = { 16 * 1024, 32 * 1024, 64 * 1024 };
	uint8_t *buf = malloc(64 * 1024);
	if (!buf) {
		say("   out of memory\n\n");
		return false;
	}
	PS_GenerateRandomBytes(buf, 64 * 1024);

	static write_job w;
	bool ok = true;
	for (size_t i = 0; i < sizeof(BLOCKS) / sizeof(BLOCKS[0]); i++) {
		memset(&w, 0, sizeof(w));
		data_path(WRITE_FILE, w.path, sizeof(w.path));
		w.block = BLOCKS[i];
		w.buf = buf;
		remove(w.path);
		bg_stats st;
		if (!run_in_background(write_fn, &w, &st)) {
			ok = false;
			break;
		}
		if (!w.ok) {
			say("   %2u KB: %s\n", (unsigned)(w.block / 1024), w.err);
			ok = false;
			break;
		}
		double mbs = (WRITE_TOTAL / 1048576.0) / ((w.write_ms + w.close_ms) / 1000.0);
		say("   %2u KB blocks: %.0f ms (+%.0f close) = %.2f MB/s, "
		    "worst fwrite %.1f ms\n", (unsigned)(w.block / 1024),
		    w.write_ms, w.close_ms, mbs, w.worst_write_ms);
		say_frames(&st);
	}
	free(buf);
	if (ok)
		snprintf(file_out, outlen, "%s", w.path);
	say("\n");
	return ok;
}

/* ------------------------------------------------------------------ 0.6 */

typedef struct {
	const char *path;      /* NULL: in memory */
	uint8_t *buf;
	size_t   total;
	double   ms;
	double   read_ms;
	bool     ok;
} decrypt_job;

static void decrypt_fn(void *p)
{
	decrypt_job *d = p;
	d->ok = false;
	uint8_t key[16], nonce[16], block[16];
	PS_GenerateRandomBytes(key, sizeof(key));
	memset(nonce, 0, sizeof(nonce));
	size_t nc_off = 0;

	mbedtls_aes_context aes;
	mbedtls_aes_init(&aes);
	mbedtls_aes_setkey_enc(&aes, key, 128);

	FILE *f = NULL;
	if (d->path) {
		f = fopen(d->path, "rb");
		if (!f) {
			mbedtls_aes_free(&aes);
			return;
		}
	}
	double t0 = now_ms();
	size_t done = 0;
	while (done < d->total) {
		size_t n = d->total - done < DECRYPT_BLOCK ? d->total - done
		                                           : DECRYPT_BLOCK;
		uint8_t *chunk = d->path ? d->buf : d->buf + done;
		if (f) {
			double r = now_ms();
			size_t got = fread(chunk, 1, n, f);
			d->read_ms += now_ms() - r;
			if (got == 0)
				break;
			n = got;
		}
		mbedtls_aes_crypt_ctr(&aes, n, &nc_off, nonce, block, chunk, chunk);
		done += n;
	}
	d->ms = now_ms() - t0;
	d->total = done;
	if (f)
		fclose(f);
	mbedtls_aes_free(&aes);
	memset(key, 0, sizeof(key));
	d->ok = true;
}

static void say_decrypt(const char *what, const decrypt_job *d,
                        const bg_stats *st)
{
	double mbs = (d->total / 1048576.0) / (d->ms / 1000.0);
	say("   %s: %.2f MB in %.0f ms = %.2f MB/s, %.0fx playback\n", what,
	    d->total / 1048576.0, d->ms, mbs,
	    (d->total / (d->ms / 1000.0)) / PLAY_BYTES_PER_S);
	if (d->path)
		say("   (of which fread %.0f ms)\n", d->read_ms);
	say_frames(st);
}

static void step_decrypt(const char *file)
{
	say("0.6 AES-128-CTR in software, %d KB blocks\n", DECRYPT_BLOCK / 1024);
	static decrypt_job d;
	bg_stats st;

	memset(&d, 0, sizeof(d));
	d.buf = malloc(DECRYPT_TOTAL);
	if (!d.buf) {
		say("   out of memory for %d MB\n", DECRYPT_TOTAL / 1048576);
	} else {
		memset(d.buf, 0x5A, DECRYPT_TOTAL);
		d.total = DECRYPT_TOTAL;
		if (run_in_background(decrypt_fn, &d, &st) && d.ok)
			say_decrypt("in memory", &d, &st);
		free(d.buf);
	}

	if (file && file[0]) {
		memset(&d, 0, sizeof(d));
		d.path = file;
		d.buf = malloc(DECRYPT_BLOCK);
		d.total = WRITE_TOTAL;
		if (d.buf && run_in_background(decrypt_fn, &d, &st) && d.ok)
			say_decrypt("read + decrypt from SD", &d, &st);
		else
			say("   read + decrypt from SD failed\n");
		free(d.buf);
	}
	say("\n");
}

/* ------------------------------------------------------- F1: dl_store */

/* A full index of made-up tracks through the real store: how long saving
 * and loading 1000 take on the card, and whether the keys, the recovery
 * from a cut save and the covers work. Left alone when there is a real
 * index in the folder already; everything it writes is removed after. */
#define STORE_TRACKS DL_TRACKS_MAX

/* A made-up track URI, unique per `i`, in base62 like a real one. */
static void fake_uri(int i, char *out, size_t outlen)
{
	snprintf(out, outlen, "spotify:track:pr%020d", i);
}

static bool same_file(const char *path, const void *data, size_t len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	uint8_t *buf = malloc(len + 1);
	size_t got = buf ? fread(buf, 1, len + 1, f) : 0;
	fclose(f);
	bool same = buf && got == len && memcmp(buf, data, len) == 0;
	free(buf);
	return same;
}

static void step_store(void)
{
	say("F1 downloads store, %d tracks\n", STORE_TRACKS);
	char path[340], tmp[350], err[160];
	if (!dl_store_init()) {
		say("   dl_store_init failed\n\n");
		return;
	}
	char dir[256];
	cfg_dir(dir, sizeof(dir));
	snprintf(path, sizeof(path), "%sdownloads/index.dat", dir[0] ? dir : "sdmc:/");
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *probe = fopen(path, "r");
	if (probe) {
		fclose(probe);
		say("   %s exists: not touching real downloads\n\n", path);
		return;
	}

	dl_index *idx = malloc(sizeof(dl_index));
	dl_index *back = malloc(sizeof(dl_index));
	if (!idx || !back) {
		say("   out of memory for two indexes (%u KB each)\n\n",
		    (unsigned)(sizeof(dl_index) / 1024));
		free(idx);
		free(back);
		return;
	}
	say("   dl_index is %u KB\n", (unsigned)(sizeof(dl_index) / 1024));
	dl_index_clear(idx);
	snprintf(idx->owner, sizeof(idx->owner), "probe");

	/* Long names, so the file is as big as a real full one gets. */
	uint8_t first_key[16], last_key[16];
	double wrap_ms = 0;
	bool wrapped = true;
	for (int i = 0; i < STORE_TRACKS; i++) {
		dl_track t;
		memset(&t, 0, sizeof(t));
		fake_uri(i, t.uri, sizeof(t.uri));
		snprintf(t.name, sizeof(t.name), "Track %d with a long enough name "
		         "to fill a row and then some more", i);
		snprintf(t.artist, sizeof(t.artist), "Artist number %d", i % 50);
		snprintf(t.album, sizeof(t.album), "Album number %d", i % 80);
		snprintf(t.art_small, sizeof(t.art_small),
		         "https://i.scdn.co/image/ab67616d00004851%024d", i);
		snprintf(t.art_large, sizeof(t.art_large),
		         "https://i.scdn.co/image/ab67616d00001e02%024d", i);
		t.duration_ms = 200000 + i;
		t.size = 4000000 + (uint32_t)i;
		int at = dl_add_track(idx, &t, i % 3 == 0, (uint32_t)i);
		if (at < 0)
			break;
		uint8_t key[16];
		PS_GenerateRandomBytes(key, sizeof(key));
		double t0 = now_ms();
		if (!dl_store_wrap_key(idx, idx->tracks[at].uri, key,
		                       idx->tracks[at].wrapped_key))
			wrapped = false;
		wrap_ms += now_ms() - t0;
		idx->tracks[at].has_key = true;
		idx->tracks[at].state = DL_DONE;
		if (i == 0)
			memcpy(first_key, key, 16);
		if (i == STORE_TRACKS - 1)
			memcpy(last_key, key, 16);
		memset(key, 0, sizeof(key));
	}
	say("   wrap %d keys: %s, %.0f ms (%.2f ms each)\n", idx->track_count,
	    wrapped ? "ok" : "FAILED", wrap_ms, wrap_ms / idx->track_count);
	dl_collection c;
	memset(&c, 0, sizeof(c));
	snprintf(c.uri, sizeof(c.uri), "spotify:playlist:probe");
	c.kind = DL_COLL_PLAYLIST;
	snprintf(c.name, sizeof(c.name), "Probe playlist");
	int ci = dl_add_collection(idx, &c, 1);
	for (int i = 0; i < 500 && ci >= 0; i++)
		dl_add_ref(idx, ci, i);

	double t0 = now_ms();
	int rc = dl_store_save(idx, err, sizeof(err));
	double save_ms = now_ms() - t0;
	struct stat st;
	long size = stat(path, &st) == 0 ? (long)st.st_size : -1;
	say("   save: %s in %.0f ms, %ld KB\n", rc == 0 ? "ok" : err, save_ms,
	    size / 1024);

	t0 = now_ms();
	rc = dl_store_load(back, err, sizeof(err));
	double load_ms = now_ms() - t0;
	bool same = rc == 0 && back->track_count == idx->track_count &&
	            back->coll_count == 1 && back->ref_count == idx->ref_count &&
	            strcmp(back->owner, "probe") == 0 &&
	            memcmp(back->salt, idx->salt, DL_SALT_LEN) == 0 &&
	            strcmp(back->tracks[STORE_TRACKS - 1].name,
	                   idx->tracks[STORE_TRACKS - 1].name) == 0;
	say("   load: %s in %.0f ms, %s\n", rc == 0 ? "ok" : err, load_ms,
	    same ? "same index" : "DIFFERENT");

	uint8_t key[16];
	bool k0 = dl_store_unwrap_key(back, back->tracks[0].uri,
	                              back->tracks[0].wrapped_key, key) &&
	          memcmp(key, first_key, 16) == 0;
	int last = back->track_count - 1;
	bool k1 = dl_store_unwrap_key(back, back->tracks[last].uri,
	                              back->tracks[last].wrapped_key, key) &&
	          memcmp(key, last_key, 16) == 0;
	bool moved = dl_store_unwrap_key(back, back->tracks[1].uri,
	                                 back->tracks[0].wrapped_key, key);
	memset(key, 0, sizeof(key));
	memset(first_key, 0, sizeof(first_key));
	memset(last_key, 0, sizeof(last_key));
	say("   unwrap first/last: %s/%s; under another track: %s\n",
	    k0 ? "ok" : "FAILED", k1 ? "ok" : "FAILED",
	    moved ? "OPENED (wrong)" : "refused (right)");

	/* A cut between writing .tmp and renaming it: only the .tmp is left,
	 * and it is the index to keep. */
	remove(tmp);
	rename(path, tmp);
	rc = dl_store_load(back, err, sizeof(err));
	bool restored = stat(path, &st) == 0 && stat(tmp, &st) != 0;
	say("   only a whole .tmp: %s, %d tracks, index.dat %s\n",
	    rc == 0 ? "loaded" : err, back->track_count,
	    restored ? "restored" : "NOT restored");

	/* A cut while writing .tmp: index.dat is still the last whole one. */
	FILE *f = fopen(tmp, "w");
	if (f) {
		fprintf(f, "%s%c%d\nO%cother\n", DL_MAGIC, DL_SEP, DL_VERSION, DL_SEP);
		fclose(f);
	}
	rc = dl_store_load(back, err, sizeof(err));
	say("   half a .tmp: %s, owner %s, .tmp %s\n", rc == 0 ? "loaded" : err,
	    back->owner, stat(tmp, &st) != 0 ? "removed" : "STILL THERE");

	/* Another version is refused, and left as it was. */
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "%s%c%d\nE\n", DL_MAGIC, DL_SEP, DL_VERSION + 1);
		fclose(f);
	}
	rc = dl_store_load(back, err, sizeof(err));
	say("   another version: %s\n", rc < 0 ? "refused (right)" : "LOADED");

	/* A cover in and out. */
	static uint8_t art[12 * 1024];
	PS_GenerateRandomBytes(art, sizeof(art));
	const char *url = "https://i.scdn.co/image/probe";
	bool aw = dl_store_art_write(url, art, sizeof(art));
	size_t alen = 0;
	uint8_t *aread = dl_store_art_read(url, &alen);
	bool ar = aread && alen == sizeof(art) && memcmp(aread, art, alen) == 0;
	free(aread);
	char apath[340];
	dl_store_art_path(url, apath, sizeof(apath));
	say("   cover: write %s, has %s, read %s\n", aw ? "ok" : "FAILED",
	    dl_store_art_has(url) ? "yes" : "NO", ar ? "same" : "DIFFERENT");
	remove(apath);

	/* A part file and its removal. */
	char tpath[340];
	dl_store_track_path(idx->tracks[0].uri, true, tpath, sizeof(tpath));
	f = fopen(tpath, "wb");
	if (f) {
		fwrite(art, 1, 100, f);
		fclose(f);
	}
	bool had = same_file(tpath, art, 100);
	bool gone = dl_store_remove_track(idx->tracks[0].uri) &&
	            stat(tpath, &st) != 0;
	say("   %s: written %s, removed %s\n", strrchr(tpath, '/') + 1,
	    had ? "ok" : "FAILED", gone ? "ok" : "FAILED");

	say("   free %.1f MB; index says %.1f MB used\n",
	    dl_store_free_bytes() / 1048576.0,
	    dl_store_used_bytes(idx) / 1048576.0);

	remove(path);
	remove(tmp);
	free(idx);
	free(back);
	say("\n");
}

/* --------------------------------------------------------------- network */

typedef struct {
	char host[128];
	char token[1024];             /* login5 access token: never printed */
	/* OAuth access token: never printed */
	char access[sizeof(((spotify_tokens *)0)->access_token)];
	char device_id[64];
	char username[128];
	ap_candidate cands[AP_MAX_CANDIDATES];
	int  ncands;
} net_ctx;

static ap_session *open_ap(net_ctx *n, const char *label, ap_info *info)
{
	char err[256];
	int tried = 0;
	double t0 = now_ms();
	ap_session *s = ap_login_any(n->cands, n->ncands, n->access, n->device_id,
	                             info, &tried, err, sizeof(err));
	if (!s)
		say("   %s: login failed: %s\n", label, err);
	else
		say("   %s: %s in %.0f ms\n", label, info->ap, now_ms() - t0);
	return s;
}

/* One key; returns 0, AP_KEY_REFUSED or -1, and how long it took. The key
 * itself is wiped: only whether it came matters here. */
static int one_key(ap_session *s, const sp_track *t, double *ms,
                   int *refused, char *err, size_t errlen)
{
	const sp_file *file = sp_pick_file(t);
	uint8_t key[16];
	double t0 = now_ms();
	int rc = ap_audio_key(s, file->id, t->audio_gid, key, refused, err, errlen);
	*ms = now_ms() - t0;
	memset(key, 0, sizeof(key));
	return rc;
}

static const char *key_result(int rc, int refused)
{
	static char text[32];
	if (rc == 0)
		return "ok";
	if (rc == AP_KEY_REFUSED) {
		snprintf(text, sizeof(text), "REFUSED 0x%04X", (unsigned)refused);
		return text;
	}
	return "ERROR";
}

/* Reads whatever packets are waiting on `s` without blocking, answering
 * pings. Returns false once the connection is gone. */
static bool drain(ap_session *s, const char *label, double since, int *pings)
{
	static uint8_t buf[4096];
	for (;;) {
		int w = ap_wait(s, 0);
		if (w < 0) {
			say("   %s: connection CLOSED at %.1f s\n", label,
			    (now_ms() - since) / 1000.0);
			return false;
		}
		if (w == 0)
			return true;
		uint8_t cmd = 0;
		int len = ap_recv(s, &cmd, buf, sizeof(buf));
		if (len == AP_PKT_TOO_BIG) {
			say("   %s: packet 0x%02X (too big, skipped) at %.1f s\n",
			    label, cmd, (now_ms() - since) / 1000.0);
			continue;
		}
		if (len < 0) {
			say("   %s: connection CLOSED (recv) at %.1f s\n", label,
			    (now_ms() - since) / 1000.0);
			return false;
		}
		if (cmd == AP_PKT_PING) {
			ap_handle_ping(s, buf, (size_t)len);
			(*pings)++;
			continue;
		}
		say("   %s: packet 0x%02X, %d bytes at %.1f s\n", label, cmd, len,
		    (now_ms() - since) / 1000.0);
	}
}

static void step_two_sessions(net_ctx *n, const sp_track *t)
{
	say("0.1 two AP sessions with the same device id\n");
	char err[256];
	int refused = 0;
	double ms;
	ap_info ia, ib;

	ap_session *a = open_ap(n, "A", &ia);
	if (!a) {
		say("\n");
		return;
	}
	int rc = one_key(a, t, &ms, &refused, err, sizeof(err));
	say("   A key before B: %s (%.0f ms)\n", key_result(rc, refused), ms);

	ap_session *b = open_ap(n, "B", &ib);
	if (!b) {
		ap_close(a);
		say("\n");
		return;
	}
	rc = one_key(b, t, &ms, &refused, err, sizeof(err));
	say("   B key: %s (%.0f ms)\n", key_result(rc, refused), ms);

	say("   watching both for %d s (START skips)...\n", WATCH_MS / 1000);
	double since = now_ms();
	bool a_up = true, b_up = true;
	int pa = 0, pb = 0;
	while (now_ms() - since < WATCH_MS && aptMainLoop() && (a_up || b_up)) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
		if (a_up)
			a_up = drain(a, "A", since, &pa);
		if (b_up)
			b_up = drain(b, "B", since, &pb);
		frame();
	}
	say("   after %.0f s: A %s (%d pings), B %s (%d pings)\n",
	    (now_ms() - since) / 1000.0, a_up ? "up" : "DOWN", pa,
	    b_up ? "up" : "DOWN", pb);

	int ra = -1, rb = -1, refa = 0, refb = 0;
	if (a_up) {
		ra = one_key(a, t, &ms, &refa, err, sizeof(err));
		say("   A key after: %s (%.0f ms)\n", key_result(ra, refa), ms);
	}
	if (b_up) {
		rb = one_key(b, t, &ms, &refb, err, sizeof(err));
		say("   B key after: %s (%.0f ms)\n", key_result(rb, refb), ms);
	}
	if (ra == 0 && rb == 0)
		say("   VERDICT: both sessions live and both get keys: they coexist.\n");
	else if (rb == 0)
		say("   VERDICT: B works, A does not: the second session pushes "
		    "the first out.\n");
	else
		say("   VERDICT: unclear; see above.\n");
	ap_close(a);
	ap_close(b);
	say("\n");
}

/* Tracks to ask keys for: Liked Songs, topped up with the sample track's
 * album when there are few, each with a Vorbis file this client would
 * pick. Returns how many. */
static int key_tracks(net_ctx *n, const sp_track *sample, sp_track *out,
                      int max)
{
	static char uris[KEY_BURST][SP_URI_MAX];
	const char *ptrs[KEY_BURST];
	char err[256];
	int count = 0, total = 0;

	http_session *sess = http_session_open(n->host);
	if (!sess) {
		say("   no spclient session\n");
		return 0;
	}
	int got = sp_liked_s(sess, n->token, n->username, uris, max, &total,
	                     err, sizeof(err));
	if (got < 0) {
		say("   liked songs: %s\n", err);
		got = 0;
	} else {
		say("   liked songs: %d of %d\n", got, total);
	}
	count = got;
	if (count < 10 && sample->album_uri[0]) {
		static sp_album album;
		if (sp_album_metadata(sess, n->token, sample->album_uri, &album,
		                      err, sizeof(err)) == 0) {
			for (int i = 0; i < album.track_count && count < max; i++)
				snprintf(uris[count++], SP_URI_MAX, "%s", album.track_uris[i]);
			say("   + %d from the sample album\n", album.track_count);
		} else {
			say("   album: %s\n", err);
		}
	}

	int named = 0;
	static sp_track batch[SP_PLAYLIST_PAGE];
	for (int from = 0; from < count; from += SP_PLAYLIST_PAGE) {
		int k = count - from < SP_PLAYLIST_PAGE ? count - from : SP_PLAYLIST_PAGE;
		for (int i = 0; i < k; i++)
			ptrs[i] = uris[from + i];
		int m = sp_track_metadata_batch_s(sess, n->token, ptrs, k, batch,
		                                  err, sizeof(err));
		if (m < 0) {
			say("   metadata: %s\n", err);
			break;
		}
		for (int i = 0; i < m && named < max; i++)
			if (batch[i].has_gid && sp_pick_file(&batch[i]))
				out[named++] = batch[i];
	}
	http_session_close(sess);
	if (named == 0) {
		out[0] = *sample;
		named = 1;
	}
	say("   %d tracks with a playable file\n", named);
	return named;
}

/* The access point refuses with 0x0002 after a burst, on any track and on a
 * fresh session too, so the limit belongs to the account or the device. On
 * the console: 15 keys in 0.8 s, then refused for about 30 s, and right
 * after that one key, then refused again -- a bucket that holds about 15
 * and refills slowly. This measures the refill: empty it, ask nothing for a
 * while, and count what came back. Then it tries the pace that follows. */

/* Asks keys back to back, `gap_ms` apart, cycling over `tracks` from
 * `*next`, until `max` came or one fails. Returns how many came; `*rc_out`
 * is the failing result (0 when none failed). */
static int key_run(ap_session *s, const sp_track *tracks, int ntracks,
                   int *next, int max, int gap_ms, int *rc_out)
{
	char err[256];
	double sum = 0, worst = 0, start = now_ms();
	int ok = 0;
	*rc_out = 0;
	for (int i = 0; i < max && aptMainLoop(); i++) {
		if (i && gap_ms)
			wait_ms(gap_ms);
		const sp_track *t = &tracks[(*next)++ % ntracks];
		double ms;
		int refused = 0;
		int rc = one_key(s, t, &ms, &refused, err, sizeof(err));
		if (rc != 0) {
			say("   key %d: %s after %.0f ms, at %.1f s\n", i + 1,
			    key_result(rc, refused), ms, (now_ms() - start) / 1000.0);
			if (rc == -1)
				say("   %s\n", err);
			*rc_out = rc;
			break;
		}
		ok++;
		sum += ms;
		if (ms > worst)
			worst = ms;
		frame();
	}
	if (ok)
		say("   %d ok: average %.0f ms, worst %.0f ms, %.1f s in all\n", ok,
		    sum / ok, worst, (now_ms() - start) / 1000.0);
	return ok;
}

/* Waits `ms` asking nothing, while answering the session's pings so it stays
 * up. Returns false when START cuts it short or the session drops. */
static bool idle(ap_session *s, int ms, double since)
{
	say("   %.0f s: %d s asking nothing (START skips the rest)\n",
	    (now_ms() - since) / 1000.0, ms / 1000);
	double end = now_ms() + ms;
	int pings = 0;
	while (now_ms() < end && aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			return false;
		if (!drain(s, "session", since, &pings))
			return false;
		frame();
	}
	return true;
}

/* A burst after a rest: how many came back in `rest_ms`. -1 when cut short
 * or the session broke. */
static int refill(ap_session *s, const sp_track *tracks, int ntracks,
                  int *next, int rest_ms, double since)
{
	if (!idle(s, rest_ms, since))
		return -1;
	int rc;
	int got = key_run(s, tracks, ntracks, next, KEY_BURST, 0, &rc);
	say("   after %d s: %d keys\n", rest_ms / 1000, got);
	return rc == -1 ? -1 : got;
}

static void step_key_burst(net_ctx *n, const sp_track *sample)
{
	say("0.2 audio keys: the limit and its refill\n");
	static sp_track tracks[KEY_BURST];
	int nt = key_tracks(n, sample, tracks, KEY_BURST);
	ap_info info;
	ap_session *s = open_ap(n, "session", &info);
	if (!s) {
		say("\n");
		return;
	}
	double since = now_ms();
	int next = 0, rc;

	say("   empty the bucket: back to back, up to %d\n", KEY_BURST);
	int cap = key_run(s, tracks, nt, &next, KEY_BURST, 0, &rc);
	if (rc == 0) {
		say("   VERDICT: %d keys back to back, no refusal.\n\n", cap);
		ap_close(s);
		return;
	}

	static const int RESTS[] = { 60000, 180000 };
	int got[2] = { -1, -1 };
	for (int i = 0; i < 2; i++) {
		got[i] = refill(s, tracks, nt, &next, RESTS[i], since);
		if (got[i] < 0)
			break;
	}

	/* The pace to try: what came back over the longer rest, spread with a
	 * margin of a half. Bounded so the round ends in a few minutes. */
	int gap = KEY_PACE_MAX_MS;
	if (got[1] > 0)
		gap = (int)(1.5 * RESTS[1] / got[1]);
	else if (got[0] > 0)
		gap = (int)(1.5 * RESTS[0] / got[0]);
	if (gap < KEY_PACE_MIN_MS)
		gap = KEY_PACE_MIN_MS;
	if (gap > KEY_PACE_MAX_MS)
		gap = KEY_PACE_MAX_MS;

	int paced_ok = 0, paced = 0;
	if (got[1] >= 0 && idle(s, KEY_PACE_REST_MS, since)) {
		say("   %.0f s: %d keys %d ms apart, on through refusals\n",
		    (now_ms() - since) / 1000.0, KEY_PACE_COUNT, gap);
		char err[256];
		for (; paced < KEY_PACE_COUNT && aptMainLoop(); paced++) {
			if (paced && !idle(s, gap, since))
				break;
			double ms;
			int refused = 0;
			int r = one_key(s, &tracks[next++ % nt], &ms, &refused, err,
			                sizeof(err));
			say("   key %d: %s\n", paced + 1, key_result(r, refused));
			if (r == 0)
				paced_ok++;
			else if (r == -1)
				break;
		}
	}
	ap_close(s);

	say("   VERDICT: bucket %d; back after 60 s: %d, after 180 s: %d",
	    cap, got[0], got[1]);
	if (paced)
		say("; at %d ms apart %d of %d", gap, paced_ok, paced);
	say(".\n\n");
}

/* Signs in as the app does and leaves `n` ready for the AP steps. */
static bool net_setup(net_ctx *n, sp_track *sample)
{
	char err[256];
	static char stored[512];
	if (!cfg_load_refresh_token(stored, sizeof(stored))) {
		say("Not paired. Pair in the app first.\n");
		return false;
	}
	static spotify_tokens tok;
	say("signing in...\n");
	if (token_refresh(stored, &tok, err, sizeof(err)) != 0) {
		say("   %s\n", err);
		return false;
	}
	if (tok.refresh_token[0] && strcmp(tok.refresh_token, stored) != 0)
		cfg_save_refresh_token(tok.refresh_token, tok.scope);
	snprintf(n->access, sizeof(n->access), "%s", tok.access_token);
	ap_device_id(n->device_id, sizeof(n->device_id));

	n->ncands = ap_resolve_list(n->cands, AP_MAX_CANDIDATES, err, sizeof(err));
	if (n->ncands < 1) {
		say("   %s\n", err);
		return false;
	}
	ap_info info;
	int tried = 0;
	ap_session *ap = ap_login_any(n->cands, n->ncands, n->access,
	                              n->device_id, &info, &tried, err, sizeof(err));
	if (!ap) {
		say("   %s\n", err);
		return false;
	}
	static login5_token l5;
	int rc = login5_from_credentials(info.username, info.reusable,
	                                 info.reusable_len, n->device_id, &l5,
	                                 err, sizeof(err));
	ap_close(ap);
	if (rc != 0) {
		say("   login5: %s\n", err);
		return false;
	}
	snprintf(n->token, sizeof(n->token), "%s", l5.access_token);
	snprintf(n->username, sizeof(n->username), "%s", info.username);
	char ignored[128];
	sp_resolve(n->host, sizeof(n->host), ignored, sizeof(ignored));

	if (sp_track_metadata(n->host, n->token, SAMPLE_TRACK, sample, err,
	                      sizeof(err)) != 0) {
		say("   sample track: %s\n", err);
		return false;
	}
	if (!sp_pick_file(sample)) {
		say("   sample track has no playable file\n");
		return false;
	}
	say("   ok; sample %s - %s\n\n", sample->artist, sample->name);
	return true;
}

int main(int argc, char **argv)
{
	(void)argc;
	gfxInitDefault();
	consoleInit(GFX_TOP, NULL);
	const char *argv0 = (argv && argv[0]) ? argv[0] : NULL;
	cfg_set_path(argv0);
	open_log(argv0);

	say("3DS Spotify -- offline probes\n");
	say("built %s\n\n", BUILD_STAMP);

	bool ps = R_SUCCEEDED(psInit());
	if (!ps)
		say("psInit failed: 0.4, 0.3 and 0.6 need it.\n\n");

	bool wifi = step_wifi();
	if (ps)
		step_ids();

	say("0.7 SD free space\n");
	u64 before = 0, after = 0;
	bool have_before = step_free("before", &before);
	say("\n");

	char written[300] = "";
	if (ps && step_sd_write(written, sizeof(written))) {
		say("0.7 again, with the %d MB file on the card\n",
		    WRITE_TOTAL / 1048576);
		if (step_free("after", &after) && have_before)
			say("   difference %.2f MB\n",
			    ((double)before - (double)after) / 1048576.0);
		say("\n");
	}
	if (ps)
		step_decrypt(written);
	if (written[0])
		remove(written);
	if (ps)
		step_store();

	if (!wifi) {
		say("No Wi-Fi: 0.1 and 0.2 skipped.\n");
		goto hold;
	}

	u32 *soc = memalign(0x1000, SOC_BUFFER);
	if (!soc || R_FAILED(socInit(soc, SOC_BUFFER))) {
		say("socInit failed.\n");
		goto hold;
	}
	int rc = tls_global_init();
	if (rc) {
		char reason[128];
		tls_strerror(rc, reason, sizeof(reason));
		say("tls_global_init: %s\n", reason);
		goto hold;
	}

	static net_ctx net;
	static sp_track sample;
	if (net_setup(&net, &sample)) {
		step_two_sessions(&net, &sample);
		step_key_burst(&net, &sample);
	}
	memset(&net, 0, sizeof(net));
	say("done.\n");

hold:
	say("\nSTART exits.\n");
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
		frame();
	}
	if (logf)
		fclose(logf);
	tls_global_exit();
	if (ps)
		psExit();
	socExit();
	gfxExit();
	return 0;
}
