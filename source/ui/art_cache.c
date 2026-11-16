#include "art_cache.h"

#include "art_cache_logic.h"

#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define URL_SIZE 128

typedef struct {
	char url[URL_SIZE];
	void *data;
	size_t len;
} entry;

static LightLock lock;
static bool ready;
static entry entries[ART_CACHE_SLOTS];
static bool used[ART_CACHE_SLOTS];
static unsigned stamps[ART_CACHE_SLOTS];
static unsigned clock_now;
static size_t total;

void art_cache_init(void)
{
	LightLock_Init(&lock);
	ready = true;
}

/* Under the lock. */
static void drop(int i)
{
	free(entries[i].data);
	total -= entries[i].len;
	memset(&entries[i], 0, sizeof(entries[i]));
	used[i] = false;
}

void art_cache_exit(void)
{
	if (!ready)
		return;
	LightLock_Lock(&lock);
	for (int i = 0; i < ART_CACHE_SLOTS; i++)
		if (used[i])
			drop(i);
	LightLock_Unlock(&lock);
}

/* Under the lock: the slot holding `url`, or -1. */
static int find(const char *url)
{
	for (int i = 0; i < ART_CACHE_SLOTS; i++)
		if (used[i] && strcmp(entries[i].url, url) == 0)
			return i;
	return -1;
}

void art_cache_put(const char *url, const void *data, size_t len)
{
	if (!ready || !url || !url[0] || strlen(url) >= URL_SIZE || !data ||
	    len == 0 || len > ART_CACHE_ENTRY_MAX)
		return;
	/* The copy is made before taking the lock. */
	void *copy = malloc(len);
	if (!copy)
		return;
	memcpy(copy, data, len);

	LightLock_Lock(&lock);
	int old = find(url);
	if (old >= 0)
		drop(old);
	while (total + len > ART_CACHE_BUDGET) {
		int oldest = art_cache_oldest(used, stamps, ART_CACHE_SLOTS);
		if (oldest < 0)
			break;
		drop(oldest);
	}
	int slot = art_cache_victim(used, stamps, ART_CACHE_SLOTS);
	if (used[slot])
		drop(slot);
	snprintf(entries[slot].url, URL_SIZE, "%s", url);
	entries[slot].data = copy;
	entries[slot].len = len;
	used[slot] = true;
	stamps[slot] = ++clock_now;
	total += len;
	LightLock_Unlock(&lock);
}

bool art_cache_has(const char *url)
{
	if (!ready || !url || !url[0])
		return false;
	LightLock_Lock(&lock);
	bool found = find(url) >= 0;
	LightLock_Unlock(&lock);
	return found;
}

void *art_cache_get(const char *url, size_t *len)
{
	if (!ready || !url || !url[0])
		return NULL;
	void *copy = NULL;
	LightLock_Lock(&lock);
	int i = find(url);
	if (i >= 0) {
		/* At most ART_CACHE_ENTRY_MAX: quick enough under the lock. */
		copy = malloc(entries[i].len);
		if (copy) {
			memcpy(copy, entries[i].data, entries[i].len);
			*len = entries[i].len;
			stamps[i] = ++clock_now;
		}
	}
	LightLock_Unlock(&lock);
	return copy;
}
