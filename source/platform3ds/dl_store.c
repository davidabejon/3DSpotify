/* Downloads on the SD card. See dl_store.h for the layout.
 *
 * The audio is kept as the CDN serves it, encrypted with the track's
 * AES-128-CTR audio key; a plain .ogg is never written. The audio key is
 * kept wrapped: AES-128-GCM under a key-encryption key (KEK) derived with
 * HKDF-SHA256 from this console's local friend code seed and PS device id,
 * salted with 16 random bytes kept in the index. Each key has its own
 * random nonce, and the track URI is the additional data, so a wrapped key
 * moved to another track's entry does not open.
 *
 * What that means:
 *  - The card in another console does not play these downloads: they have
 *    to be downloaded again there.
 *  - This is not DRM. Whoever has the console can derive the KEK. It stops
 *    the folder being copied and played somewhere else, nothing more.
 *  - The KEK and an unwrapped key live only in buffers wiped right after
 *    use. Never in the log, on screen or on the card.
 */
#include "dl_store.h"

#include "cfg3ds.h"

#include <3ds.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "mbedtls/gcm.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"

#define KEK_INFO "3dspotify-dl-v1"
/* A cover is a few tens of KB; anything this large is not one. */
#define ART_MAX (1024 * 1024)
/* The index is read and written a line at a time. With stdio's default
 * buffer that is one small transfer per line, and a full index took 2-3 s
 * on the console; in blocks of this size the card goes at its speed
 * (64 KB was its fastest, probe_offline 0.3). */
#define INDEX_BUF (64 * 1024)

static void base_dir(char *out, size_t outlen)
{
	char dir[256];
	cfg_dir(dir, sizeof(dir));
	snprintf(out, outlen, "%sdownloads/", dir[0] ? dir : "sdmc:/");
}

static void index_path(char *out, size_t outlen, bool tmp)
{
	char base[300];
	base_dir(base, sizeof(base));
	snprintf(out, outlen, "%sindex.dat%s", base, tmp ? ".tmp" : "");
}

static bool make_dir(const char *path)
{
	return mkdir(path, 0777) == 0 || errno == EEXIST;
}

bool dl_store_init(void)
{
	char base[300], sub[320];
	base_dir(base, sizeof(base));
	bool ok = make_dir(base);
	snprintf(sub, sizeof(sub), "%st", base);
	ok = make_dir(sub) && ok;
	snprintf(sub, sizeof(sub), "%sa", base);
	ok = make_dir(sub) && ok;
	return ok;
}

/* Reads `path` into `out`. 1 when it was this format and whole, 0 when it
 * is not there, -1 when it is there but refused or cut short. */
static int read_index(const char *path, dl_index *out, bool *refused)
{
	*refused = false;
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	setvbuf(f, NULL, _IOFBF, INDEX_BUF);
	static char line[DL_LINE_MAX];
	dl_index_clear(out);
	dl_parse_state st = {0};
	while (fgets(line, sizeof(line), f)) {
		size_t n = strlen(line);
		if (n == sizeof(line) - 1 && line[n - 1] != '\n') {
			/* Longer than any line this build writes: skip the rest. */
			int c;
			while ((c = fgetc(f)) != EOF && c != '\n')
				;
			continue;
		}
		dl_index_parse_line(out, &st, line);
	}
	fclose(f);
	*refused = st.refused;
	return st.header && st.ended ? 1 : -1;
}

int dl_store_load(dl_index *out, char *err, size_t errlen)
{
	char path[320], tmp[320];
	index_path(path, sizeof(path), false);
	index_path(tmp, sizeof(tmp), true);
	bool refused = false;

	/* A whole .tmp is a save that got past writing but not past the
	 * rename: it is the newest index. A partial one is a save cut short,
	 * and index.dat is still the last whole one. */
	if (read_index(tmp, out, &refused) == 1) {
		remove(path);
		rename(tmp, path);
		return 0;
	}
	remove(tmp);

	int rc = read_index(path, out, &refused);
	if (refused) {
		snprintf(err, errlen, "downloads index from another version");
		return -1;
	}
	if (rc == 0)
		dl_index_clear(out);
	/* rc -1: index.dat without its end, edited by hand or damaged. What
	 * could be read is kept. */
	return 0;
}

int dl_store_save(const dl_index *idx, char *err, size_t errlen)
{
	char path[320], tmp[320];
	index_path(path, sizeof(path), false);
	index_path(tmp, sizeof(tmp), true);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		snprintf(err, errlen, "cannot write the downloads index");
		return -1;
	}
	setvbuf(f, NULL, _IOFBF, INDEX_BUF);
	static char line[DL_LINE_MAX];
	int cursor = 0;
	bool ok = true;
	while (ok && dl_index_line(idx, &cursor, line, sizeof(line)))
		ok = fputs(line, f) >= 0 && fputc('\n', f) != EOF;
	ok = fflush(f) == 0 && ok;
	ok = fclose(f) == 0 && ok;
	if (!ok) {
		remove(tmp);
		snprintf(err, errlen, "cannot write the downloads index");
		return -1;
	}
	/* The SD card's rename does not replace: the old one goes first. */
	remove(path);
	if (rename(tmp, path) != 0) {
		snprintf(err, errlen, "cannot replace the downloads index");
		return -1;
	}
	return 0;
}

/* The base62 id at the end of a spotify:track: URI is safe as a file name.
 * Anything else is named by its hash, so no URI can reach outside t/. */
static void track_name(const char *uri, char *out, size_t outlen)
{
	const char *id = uri ? strrchr(uri, ':') : NULL;
	id = id ? id + 1 : "";
	bool safe = id[0] && strlen(id) <= SP_ID_LEN;
	for (const char *p = id; safe && *p; p++)
		safe = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') ||
		       (*p >= 'A' && *p <= 'Z');
	if (safe) {
		snprintf(out, outlen, "%s", id);
		return;
	}
	uint8_t digest[32];
	char hex[33];
	mbedtls_sha256((const uint8_t *)(uri ? uri : ""), uri ? strlen(uri) : 0,
	               digest, 0);
	dl_hex(digest, 16, hex);
	snprintf(out, outlen, "%s", hex);
}

void dl_store_track_path(const char *uri, bool part, char *out, size_t outlen)
{
	char base[300], name[40];
	base_dir(base, sizeof(base));
	track_name(uri, name, sizeof(name));
	snprintf(out, outlen, "%st/%s.%s", base, name, part ? "part" : "enc");
}

void dl_store_art_path(const char *url, char *out, size_t outlen)
{
	char base[300], hex[33];
	uint8_t digest[32];
	base_dir(base, sizeof(base));
	mbedtls_sha256((const uint8_t *)(url ? url : ""), url ? strlen(url) : 0,
	               digest, 0);
	dl_hex(digest, 16, hex);
	snprintf(out, outlen, "%sa/%s.img", base, hex);
}

bool dl_store_art_has(const char *url)
{
	char path[340];
	struct stat st;
	dl_store_art_path(url, path, sizeof(path));
	return stat(path, &st) == 0 && st.st_size > 0;
}

void *dl_store_art_read(const char *url, size_t *len)
{
	char path[340];
	*len = 0;
	dl_store_art_path(url, path, sizeof(path));
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	uint8_t *data = NULL;
	if (fseek(f, 0, SEEK_END) == 0) {
		long size = ftell(f);
		if (size > 0 && size <= ART_MAX && fseek(f, 0, SEEK_SET) == 0) {
			data = malloc((size_t)size);
			if (data && fread(data, 1, (size_t)size, f) != (size_t)size) {
				free(data);
				data = NULL;
			}
			if (data)
				*len = (size_t)size;
		}
	}
	fclose(f);
	return data;
}

bool dl_store_art_write(const char *url, const void *data, size_t len)
{
	if (!data || !len || len > ART_MAX)
		return false;
	char path[340], tmp[350];
	dl_store_art_path(url, path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return false;
	bool ok = fwrite(data, 1, len, f) == len;
	ok = fclose(f) == 0 && ok;
	/* Same bytes for the same URL: an existing one is as good. */
	if (ok) {
		remove(path);
		ok = rename(tmp, path) == 0;
	}
	if (!ok)
		remove(tmp);
	return ok;
}

bool dl_store_remove_track(const char *uri)
{
	char path[340];
	struct stat st;
	bool ok = true;
	for (int part = 0; part < 2; part++) {
		dl_store_track_path(uri, part, path, sizeof(path));
		if (stat(path, &st) == 0)
			ok = remove(path) == 0 && ok;
	}
	return ok;
}

uint32_t dl_store_part_size(const char *uri)
{
	char path[340];
	struct stat st;
	dl_store_track_path(uri, true, path, sizeof(path));
	return stat(path, &st) == 0 && st.st_size > 0 ? (uint32_t)st.st_size : 0;
}

bool dl_store_finish_track(const char *uri)
{
	char part[340], enc[340];
	dl_store_track_path(uri, true, part, sizeof(part));
	dl_store_track_path(uri, false, enc, sizeof(enc));
	remove(enc);
	return rename(part, enc) == 0;
}

void dl_store_sweep(const dl_index *idx)
{
	char base[300], dir[310];
	base_dir(base, sizeof(base));
	snprintf(dir, sizeof(dir), "%st", base);
	DIR *d = opendir(dir);
	if (!d)
		return;
	/* The names to delete first, then deleted: removing while reading the
	 * directory is not something every FAT driver handles. */
	static char doomed[64][48];
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL && n < 64) {
		const char *dot = strrchr(e->d_name, '.');
		if (!dot || e->d_name[0] == '.')
			continue;
		bool part = strcmp(dot, ".part") == 0;
		if (!part && strcmp(dot, ".enc") != 0)
			continue;
		bool known = false;
		char name[40], mine[340];
		for (int i = 0; i < idx->track_count && !known; i++) {
			track_name(idx->tracks[i].uri, name, sizeof(name));
			snprintf(mine, sizeof(mine), "%s.%s", name, part ? "part" : "enc");
			known = strcmp(mine, e->d_name) == 0;
		}
		if (!known && strlen(e->d_name) < sizeof(doomed[0]))
			snprintf(doomed[n++], sizeof(doomed[0]), "%s", e->d_name);
	}
	closedir(d);
	for (int i = 0; i < n; i++) {
		char path[360];
		snprintf(path, sizeof(path), "%s/%.47s", dir, doomed[i]);
		remove(path);
	}
}

uint64_t dl_store_free_bytes(void)
{
	FS_ArchiveResource res;
	if (R_FAILED(FSUSER_GetArchiveResource(&res, SYSTEM_MEDIATYPE_SD)))
		return 0;
	return (uint64_t)res.freeClusters * res.clusterSize;
}

uint64_t dl_store_used_bytes(const dl_index *idx)
{
	uint64_t used = 0;
	for (int i = 0; i < idx->track_count; i++)
		if (idx->tracks[i].state == DL_DONE)
			used += idx->tracks[i].size;
	return used;
}

/* ------------------------------------------------------------ the keys */

/* The KEK for `salt`, into `kek`. The caller wipes it. */
static bool derive_kek(const uint8_t salt[DL_SALT_LEN], uint8_t kek[16])
{
	u64 lfcs = 0;
	u32 devid = 0;
	if (R_FAILED(PS_GetLocalFriendCodeSeed(&lfcs)) ||
	    R_FAILED(PS_GetDeviceId(&devid)))
		return false;
	uint8_t ikm[12];
	memcpy(ikm, &lfcs, 8);
	memcpy(ikm + 8, &devid, 4);
	const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	int rc = md ? mbedtls_hkdf(md, salt, DL_SALT_LEN, ikm, sizeof(ikm),
	                           (const uint8_t *)KEK_INFO, strlen(KEK_INFO),
	                           kek, 16)
	            : -1;
	mbedtls_platform_zeroize(ikm, sizeof(ikm));
	mbedtls_platform_zeroize(&lfcs, sizeof(lfcs));
	mbedtls_platform_zeroize(&devid, sizeof(devid));
	return rc == 0;
}

bool dl_store_ensure_salt(dl_index *idx)
{
	if (idx->has_salt)
		return true;
	if (R_FAILED(PS_GenerateRandomBytes(idx->salt, DL_SALT_LEN)))
		return false;
	idx->has_salt = true;
	return true;
}

bool dl_store_wrap_key(const uint8_t salt[DL_SALT_LEN], const char *uri,
                       const uint8_t key[16], uint8_t out[DL_WRAPPED_LEN])
{
	uint8_t kek[16];
	if (!derive_kek(salt, kek))
		return false;
	uint8_t *nonce = out;
	uint8_t *sealed = out + DL_NONCE_LEN;
	uint8_t *tag = out + DL_NONCE_LEN + DL_KEY_LEN;
	bool ok = R_SUCCEEDED(PS_GenerateRandomBytes(nonce, DL_NONCE_LEN));

	mbedtls_gcm_context gcm;
	mbedtls_gcm_init(&gcm);
	ok = ok && mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, kek, 128) == 0;
	ok = ok && mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, DL_KEY_LEN,
	                                     nonce, DL_NONCE_LEN,
	                                     (const uint8_t *)uri, strlen(uri),
	                                     key, sealed, DL_TAG_LEN, tag) == 0;
	mbedtls_gcm_free(&gcm);
	mbedtls_platform_zeroize(kek, sizeof(kek));
	if (!ok)
		mbedtls_platform_zeroize(out, DL_WRAPPED_LEN);
	return ok;
}

bool dl_store_unwrap_key(const uint8_t salt[DL_SALT_LEN], const char *uri,
                         const uint8_t in[DL_WRAPPED_LEN], uint8_t key[16])
{
	uint8_t kek[16];
	if (!derive_kek(salt, kek))
		return false;
	mbedtls_gcm_context gcm;
	mbedtls_gcm_init(&gcm);
	bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, kek, 128) == 0 &&
	          mbedtls_gcm_auth_decrypt(&gcm, DL_KEY_LEN, in, DL_NONCE_LEN,
	                                   (const uint8_t *)uri, strlen(uri),
	                                   in + DL_NONCE_LEN + DL_KEY_LEN,
	                                   DL_TAG_LEN, in + DL_NONCE_LEN,
	                                   key) == 0;
	mbedtls_gcm_free(&gcm);
	mbedtls_platform_zeroize(kek, sizeof(kek));
	if (!ok)
		mbedtls_platform_zeroize(key, 16);
	return ok;
}
