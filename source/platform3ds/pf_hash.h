/* The persisted-query hash pathfinder's find* operations are asked by.
 *
 * Spotify changes it when the web player is rebuilt, and then answers the
 * old one with HTTP 412. The default below is the one the player declared
 * on 2026-11-17 (probe_search); a newer one, once found, is kept in the
 * config as `pf_hash`.
 */
#ifndef PF_HASH_H
#define PF_HASH_H

#include <stdbool.h>
#include <stddef.h>

/* Checked again with every release: update it if the player changed it. */
#define PF_HASH_DEFAULT \
	"903df2a65d8121e27d73a2be03c01e88ebe6021bb6d4eb82a389e35d87e51d27"

/* The config's hash when it holds a valid one, else the default. */
const char *pf_hash_current(void);

/* Finds the hash again from the mobile web player, the way probe_search did
 * on the console (~2.6 s): its page, the script it names, inflated (~1.4 MB
 * held for a moment), and the hash it declares for findTopResults -- which
 * the other find* must share, or it is not trusted. Kept in the config.
 * Tried once per run at most: a second call fails at once. Returns 0, or
 * -1 with `err` set. */
int pf_hash_refresh(char *err, size_t errlen);

#endif
