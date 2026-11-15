/* The detail stacks: which detail screen each tab shows, and how to go back
 * (docs/plan_vistas_detalle.md 3.1).
 *
 * The Search, Playing and Home tabs each keep a stack of open details
 * (track, album, artist, and on Home the playlist it starts from). While a tab's stack is not empty its top replaces the
 * tab's own view; B pops one level; L/R leave the stacks alone, so coming
 * back to a tab finds it as it was. The graph track -> album -> track ->
 * artist -> ... has no end, so a full stack drops its oldest level.
 *
 * A level is the index of a document in a fixed pool shared by both
 * stacks; a document is free when no stack holds it. Pure C with no 3DS
 * headers, tested in tests/native_logic_test.c. */
#ifndef DETAIL_NAV_H
#define DETAIL_NAV_H

#include "tabs_logic.h"

#include <stdbool.h>
#include <string.h>

#define DETAIL_DEPTH 6
#define DETAIL_STACKS 3
/* Every level of every stack at once, and never more: ~22 KB each. */
#define DETAIL_POOL (DETAIL_STACKS * DETAIL_DEPTH)

typedef struct {
	int slot[DETAIL_DEPTH];      /* pool indices, oldest first */
	int count;
} detail_stack;

/* Which stack `tab` keeps, or -1 for a tab that opens no details. */
static inline int detail_tab_stack(app_tab tab)
{
	return tab == TAB_SEARCH ? 0 : tab == TAB_PLAYER ? 1
	     : tab == TAB_HOME ? 2 : -1;
}

static inline void detail_stack_clear(detail_stack *s)
{
	s->count = 0;
}

/* The level on screen, or -1 when the tab shows its own view. */
static inline int detail_stack_top(const detail_stack *s)
{
	return s->count > 0 ? s->slot[s->count - 1] : -1;
}

/* Removes the oldest level and returns it, or -1 when empty. */
static inline int detail_stack_drop_oldest(detail_stack *s)
{
	if (s->count <= 0)
		return -1;
	int oldest = s->slot[0];
	memmove(s->slot, s->slot + 1, (size_t)(s->count - 1) * sizeof(s->slot[0]));
	s->count--;
	return oldest;
}

/* Puts `slot` on top. On a full stack the oldest level goes first, and is
 * returned so its document can be let go; -1 otherwise. */
static inline int detail_stack_push(detail_stack *s, int slot)
{
	int dropped = s->count >= DETAIL_DEPTH ? detail_stack_drop_oldest(s) : -1;
	s->slot[s->count++] = slot;
	return dropped;
}

/* Removes the top level and returns it, or -1 when empty. */
static inline int detail_stack_pop(detail_stack *s)
{
	return s->count > 0 ? s->slot[--s->count] : -1;
}

static inline bool detail_stack_holds(const detail_stack *s, int slot)
{
	for (int i = 0; i < s->count; i++)
		if (s->slot[i] == slot)
			return true;
	return false;
}

/* The first pool index no stack holds, or -1. With DETAIL_POOL documents
 * there is always one for a stack below DETAIL_DEPTH: make room with
 * detail_stack_drop_oldest before asking for a full one. */
static inline int detail_pool_free(const detail_stack stacks[DETAIL_STACKS])
{
	for (int slot = 0; slot < DETAIL_POOL; slot++) {
		bool held = false;
		for (int i = 0; i < DETAIL_STACKS && !held; i++)
			held = detail_stack_holds(&stacks[i], slot);
		if (!held)
			return slot;
	}
	return -1;
}

#endif
