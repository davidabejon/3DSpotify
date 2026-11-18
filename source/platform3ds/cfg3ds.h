/* The app config on the SD card: a tiny key=value file.
 *
 * An SD card has no permissions to hide behind, so what protects this is
 * keeping it small and keeping it off the screen and out of the log: the file
 * holds the one long-lived secret, and the access token is never written down.
 */
#ifndef CFG3DS_H
#define CFG3DS_H

#include <stdbool.h>
#include <stddef.h>

/* Resolves the config path next to the running .3dsx, from argv[0], falling
 * back to the card root when argv[0] is not a path the console can open. */
void cfg_set_path(const char *argv0);
const char *cfg_path(void);
/* The directory the config lives in (for native.log, a user avatar file). */
void cfg_dir(char *out, size_t outlen);

/* Generic key access: every writer preserves the other keys, so the refresh
 * token, the cached avatar URL, and the UI preferences survive each other. */
bool cfg_get(const char *key, char *out, size_t outlen);
bool cfg_set(const char *key, const char *value);

/* cfg_set off the calling thread: rewriting the file on the SD card stalls
 * for a moment, which the main loop shows as a frozen frame. The value is
 * queued (a newer one for the same key replaces it) for a writer thread;
 * cfg_get sees it at once. For preferences, whose write may fail
 * unnoticed; the refresh token keeps cfg_save_refresh_token. */
void cfg_set_async(const char *key, const char *value);
/* Writes what is still queued and stops the writer. As the app closes. */
void cfg_flush(void);

bool cfg_load_refresh_token(char *out, size_t outlen);
bool cfg_save_refresh_token(const char *token, const char *scope);
/* The avatar URL is not a secret, but it rides in the same file: the Web API
 * rate-limits /v1/me so hard (shared client id) that once the fetch succeeds
 * the answer is worth keeping across restarts. */
bool cfg_load_profile_art(char *out, size_t outlen);
bool cfg_save_profile_art(const char *url);
bool cfg_clear(void);

#endif
