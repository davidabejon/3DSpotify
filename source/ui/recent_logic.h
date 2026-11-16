/* The last searches, newest first, as the Search tab suggests them before
 * any results (view_results.h). Pure C with no 3DS headers, tested in
 * tests/native_logic_test.c.
 *
 * They are kept in the config (cfg3ds.h) under a single key, so the file
 * is written once per search: the queries joined by RECENT_SEP, which the
 * keyboard cannot type and recent_push takes out anyway, like any other
 * control character that would break the key=value line. */
#ifndef RECENT_LOGIC_H
#define RECENT_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define RECENT_MAX 3
/* The search query's size (SEARCH_QUERY_SIZE in main.c). */
#define RECENT_QUERY_SIZE 96
#define RECENT_SEP '\x1f'
/* The longest serialized list, its terminator included. */
#define RECENT_SERIAL_SIZE (RECENT_MAX * RECENT_QUERY_SIZE)

typedef struct {
	char items[RECENT_MAX][RECENT_QUERY_SIZE];
	int count;
} recent_list;

/* `src` with control characters as spaces and no blanks at either end,
 * cut to the query size. */
static inline void recent_clean(char out[RECENT_QUERY_SIZE], const char *src)
{
	size_t n = 0;
	for (; src && *src && n < RECENT_QUERY_SIZE - 1; src++) {
		unsigned char c = (unsigned char)*src;
		out[n++] = c < 0x20 || c == 0x7f ? ' ' : (char)c;
	}
	out[n] = '\0';
	while (n > 0 && out[n - 1] == ' ')
		out[--n] = '\0';
	size_t lead = strspn(out, " ");
	if (lead)
		memmove(out, out + lead, n - lead + 1);
}

static inline bool recent_same(const char *a, const char *b)
{
	for (;; a++, b++) {
		char ca = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
		char cb = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
		if (ca != cb)
			return false;
		if (!ca)
			return true;
	}
}

/* Puts `query` first. The same query, whatever its case, moves up rather
 * than showing twice; the oldest falls off past RECENT_MAX. Returns false,
 * changing nothing, for a blank query. */
static inline bool recent_push(recent_list *r, const char *query)
{
	char clean[RECENT_QUERY_SIZE];
	recent_clean(clean, query);
	if (!clean[0])
		return false;
	int from = r->count < RECENT_MAX ? r->count : RECENT_MAX - 1;
	for (int i = 0; i < r->count; i++) {
		if (recent_same(r->items[i], clean)) {
			from = i;
			break;
		}
	}
	if (from == r->count)
		r->count++;
	memmove(r->items[1], r->items[0], (size_t)from * RECENT_QUERY_SIZE);
	memcpy(r->items[0], clean, sizeof(clean));
	return true;
}

/* The list as one config value. False when it does not fit `outlen`. */
static inline bool recent_serialize(const recent_list *r, char *out,
                                    size_t outlen)
{
	size_t n = 0;
	if (outlen == 0)
		return false;
	out[0] = '\0';
	for (int i = 0; i < r->count; i++) {
		size_t len = strlen(r->items[i]);
		if (n + (i > 0) + len + 1 > outlen)
			return false;
		if (i > 0)
			out[n++] = RECENT_SEP;
		memcpy(out + n, r->items[i], len + 1);
		n += len;
	}
	return true;
}

/* The list back from a config value (NULL or "" for none). Whatever was
 * edited into the file by hand goes through recent_push's checks. */
static inline void recent_parse(recent_list *r, const char *in)
{
	r->count = 0;
	if (!in)
		return;
	const char *items[RECENT_MAX];
	size_t lens[RECENT_MAX];
	int found = 0;
	while (found < RECENT_MAX) {
		const char *end = strchr(in, RECENT_SEP);
		items[found] = in;
		lens[found] = end ? (size_t)(end - in) : strlen(in);
		found++;
		if (!end)
			break;
		in = end + 1;
	}
	/* Oldest first, so the newest ends up on top. */
	for (int i = found - 1; i >= 0; i--) {
		char item[RECENT_QUERY_SIZE];
		size_t len = lens[i] < RECENT_QUERY_SIZE - 1 ? lens[i]
		                                            : RECENT_QUERY_SIZE - 1;
		memcpy(item, items[i], len);
		item[len] = '\0';
		recent_push(r, item);
	}
}

#endif
