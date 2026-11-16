#include "cfg3ds.h"

#include <3ds.h>

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define CFG_NAME "spotify.cfg"
/* The app uses one data directory regardless of how it was launched. */
#define APP_DATA_DIR "sdmc:/3ds/3DSpotify/"
#define APP_CONFIG_PATH APP_DATA_DIR CFG_NAME
/* The refresh token and its scope, the language, the shuffle, the volume,
 * the recent searches, the profile and its picture, with room to grow. */
#define CFG_MAX_KEYS 16
#define CFG_VAL_MAX 512

static char path[320];
/* The file is read and rewritten whole, through one fixed temp file: the
 * main thread (the language) and the player's preparation thread (a renewed
 * refresh token) must not do it at once. 1 is LightLock_Init's unlocked
 * value, so the lock works before anything has run. */
static LightLock file_lock = 1;

void cfg_set_path(const char *argv0)
{
	if (argv0 && argv0[0]) {
		snprintf(path, sizeof(path), "%s", argv0);
		char *slash = strrchr(path, '/');
		if (slash) {
			slash[1] = '\0';
			/* Confirm the directory is really there before using it: under an
			 * emulator argv[0] is a host path the guest cannot reach. Asking
			 * opendir rather than opening the file for append, which would
			 * leave an empty spotify.cfg behind on every first run. */
			DIR *dir = opendir(path);
			if (dir) {
				closedir(dir);
				strncat(path, CFG_NAME, sizeof(path) - strlen(path) - 1);
				return;
			}
		}
	}
	/* Keep CIA data in the same folder used by the 3dsx release. */
	mkdir("sdmc:/3ds", 0777);
	mkdir("sdmc:/3ds/3DSpotify", 0777);
	snprintf(path, sizeof(path), "%s", APP_CONFIG_PATH);
}

const char *cfg_path(void)
{
	if (!path[0])
		cfg_set_path(NULL);
	return path;
}

/* The directory the config lives in, for files that belong beside it
 * (native.log, a user-supplied avatar). */
void cfg_dir(char *out, size_t outlen)
{
	snprintf(out, outlen, "%s", cfg_path());
	char *slash = strrchr(out, '/');
	if (slash)
		slash[1] = '\0';
	else
		out[0] = '\0';
}

static bool get_unlocked(const char *key, char *out, size_t outlen)
{
	FILE *f = fopen(cfg_path(), "r");
	if (!f)
		return false;
	size_t klen = strlen(key);
	char line[1024];
	bool found = false;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, key, klen) != 0 || line[klen] != '=')
			continue;
		char *v = line + klen + 1;
		char *nl = strpbrk(v, "\r\n");
		if (nl)
			*nl = '\0';
		if (v[0] && strlen(v) < outlen) {
			snprintf(out, outlen, "%s", v);
			found = true;
		}
		break;
	}
	fclose(f);
	return found;
}

/* Rewrites the whole file with one key set or replaced.
 *
 * This matters more than it looks: Spotify rotates the refresh token on every
 * exchange, so the moment a refresh succeeds the stored one is dead. A write
 * that is interrupted -- a closed lid, a yanked card, a full filesystem --
 * would then leave no usable token at all and force the user to pair again.
 * The temp-file-plus-rename is the step that cannot half-happen. */
static bool set_unlocked(const char *key, const char *value)
{
	if (!key || !value || !value[0])
		return false;

	/* Collect the existing pairs, replacing the one being set. Static,
	 * under file_lock: 8 KB is too much for the stack of every thread
	 * that may write. */
	static char keys[CFG_MAX_KEYS][32], vals[CFG_MAX_KEYS][CFG_VAL_MAX];
	int n = 0;
	FILE *f = fopen(cfg_path(), "r");
	if (f) {
		char line[1024];
		while (fgets(line, sizeof(line), f) && n < CFG_MAX_KEYS) {
			char *eq = strchr(line, '=');
			if (!eq || line[0] == '#')
				continue;
			*eq = '\0';
			char *v = eq + 1;
			char *nl = strpbrk(v, "\r\n");
			if (nl)
				*nl = '\0';
			if (strcmp(line, key) == 0)
				continue;   /* rewritten below */
			if (strlen(line) < sizeof(keys[0]) &&
			    strlen(v) < sizeof(vals[0]) && v[0]) {
				snprintf(keys[n], sizeof(keys[0]), "%s", line);
				snprintf(vals[n], sizeof(vals[0]), "%s", v);
				n++;
			}
		}
		fclose(f);
	}

	char tmp[sizeof(path) + 8];
	snprintf(tmp, sizeof(tmp), "%s.new", cfg_path());

	f = fopen(tmp, "w");
	if (!f)
		return false;
	fprintf(f, "# 3ds-spotify. This file holds a Spotify refresh token:\n"
	           "# treat it like a password and delete it to unpair.\n");
	fprintf(f, "%s=%s\n", key, value);
	for (int i = 0; i < n; i++)
		fprintf(f, "%s=%s\n", keys[i], vals[i]);

	bool ok = fflush(f) == 0 && ferror(f) == 0;
	if (fclose(f) != 0)
		ok = false;
	if (!ok) {
		remove(tmp);
		return false;
	}

	/* FAT has no atomic replace, so the old file goes first; the window
	 * between the two is the price, and it is far smaller than a partial
	 * write over the only copy of the token. */
	remove(cfg_path());
	if (rename(tmp, cfg_path()) != 0) {
		remove(tmp);
		return false;
	}
	return true;
}

static bool pending_get(const char *key, char *out, size_t outlen);

bool cfg_get(const char *key, char *out, size_t outlen)
{
	/* A value still waiting for the writer is the current one. */
	if (pending_get(key, out, outlen))
		return true;
	LightLock_Lock(&file_lock);
	bool found = get_unlocked(key, out, outlen);
	LightLock_Unlock(&file_lock);
	return found;
}

bool cfg_set(const char *key, const char *value)
{
	LightLock_Lock(&file_lock);
	bool ok = set_unlocked(key, value);
	LightLock_Unlock(&file_lock);
	return ok;
}

/* --- Writes in the background ---------------------------------------- */

#define PENDING_MAX CFG_MAX_KEYS

typedef struct {
	char key[32];
	char value[CFG_VAL_MAX];
	bool used;
} pending_entry;

/* Under pending_lock. 1 is LightLock_Init's unlocked value. */
static LightLock pending_lock = 1;
static pending_entry pending[PENDING_MAX];
static LightEvent writer_wake;
static Thread writer;
static volatile bool writer_stop;

static bool pending_get(const char *key, char *out, size_t outlen)
{
	bool found = false;
	LightLock_Lock(&pending_lock);
	for (int i = 0; i < PENDING_MAX; i++) {
		if (pending[i].used && strcmp(pending[i].key, key) == 0) {
			if (strlen(pending[i].value) < outlen) {
				snprintf(out, outlen, "%s", pending[i].value);
				found = true;
			}
			break;
		}
	}
	LightLock_Unlock(&pending_lock);
	return found;
}

/* Writes one queued value; false with none left. The entry stays queued
 * while it is written, so cfg_get never falls back to the old value in
 * between, and goes only if no newer value replaced it meanwhile. */
static bool write_one(void)
{
	static pending_entry entry;   /* only the writer uses it */
	int slot = -1;
	LightLock_Lock(&pending_lock);
	for (int i = 0; i < PENDING_MAX && slot < 0; i++)
		if (pending[i].used)
			slot = i;
	if (slot >= 0)
		entry = pending[slot];
	LightLock_Unlock(&pending_lock);
	if (slot < 0)
		return false;

	cfg_set(entry.key, entry.value);

	LightLock_Lock(&pending_lock);
	if (pending[slot].used && strcmp(pending[slot].key, entry.key) == 0 &&
	    strcmp(pending[slot].value, entry.value) == 0)
		pending[slot].used = false;
	LightLock_Unlock(&pending_lock);
	return true;
}

static void writer_main(void *arg)
{
	(void)arg;
	for (;;) {
		while (write_one())
			;
		if (writer_stop)
			break;
		LightEvent_Wait(&writer_wake);
	}
}

/* Just below the caller, like every worker that may run beside the
 * audio (worker_priority.h). */
static s32 writer_priority(void)
{
	s32 priority = 0x30;
	if (R_FAILED(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE)))
		priority = 0x30;
	return priority < 0x3F ? priority + 1 : 0x3F;
}

void cfg_set_async(const char *key, const char *value)
{
	if (!key || !value || !value[0] || strlen(key) >= sizeof(pending[0].key) ||
	    strlen(value) >= sizeof(pending[0].value))
		return;
	bool queued = false;
	LightLock_Lock(&pending_lock);
	int slot = -1;
	for (int i = 0; i < PENDING_MAX; i++) {
		if (pending[i].used && strcmp(pending[i].key, key) == 0) {
			slot = i;
			break;
		}
		if (!pending[i].used && slot < 0)
			slot = i;
	}
	if (slot >= 0) {
		snprintf(pending[slot].key, sizeof(pending[slot].key), "%s", key);
		snprintf(pending[slot].value, sizeof(pending[slot].value), "%s",
		         value);
		pending[slot].used = true;
		queued = true;
	}
	LightLock_Unlock(&pending_lock);

	if (queued && !writer) {
		/* Callers are the main thread: no race to start it. */
		LightEvent_Init(&writer_wake, RESET_ONESHOT);
		writer_stop = false;
		/* Room for set_unlocked's line buffer and stdio. */
		writer = threadCreate(writer_main, NULL, 16 * 1024,
		                      writer_priority(), -2, false);
	}
	if (queued && writer) {
		LightEvent_Signal(&writer_wake);
		return;
	}
	/* No room or no thread: as before, at once. */
	if (writer == NULL)
		while (write_one())
			;
	if (!queued)
		cfg_set(key, value);
}

void cfg_flush(void)
{
	if (writer) {
		writer_stop = true;
		LightEvent_Signal(&writer_wake);
		threadJoin(writer, U64_MAX);
		threadFree(writer);
		writer = NULL;
	}
	/* Anything queued after the writer left. */
	while (write_one())
		;
}

bool cfg_load_refresh_token(char *out, size_t outlen)
{
	return cfg_get("refresh_token", out, outlen);
}

bool cfg_save_refresh_token(const char *token, const char *scope)
{
	if (!token || !token[0])
		return false;
	if (!cfg_set("refresh_token", token))
		return false;
	if (scope && scope[0])
		cfg_set("scope", scope);
	return true;
}

bool cfg_load_profile_art(char *out, size_t outlen)
{
	return cfg_get("profile_art", out, outlen);
}

bool cfg_save_profile_art(const char *url)
{
	return url && url[0] && cfg_set("profile_art", url);
}

bool cfg_clear(void)
{
	LightLock_Lock(&file_lock);
	bool ok = remove(cfg_path()) == 0;
	LightLock_Unlock(&file_lock);
	return ok;
}
