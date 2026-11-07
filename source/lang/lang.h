/* User-facing text in every supported language.
 *
 * The tables live in lang_en.c and lang_es.c, indexed by str_id. Entries that
 * end in _FMT are printf formats: every language must use the same conversion
 * specifiers in the same order (tests/native_logic_test.c checks this).
 * Entries that start with STR_KEYS_ are button hints for the text screens and
 * must fit one line of the 40-column grid. STR_HINT_ entries label one
 * action each in the button bar of the redesigned views (docs/STYLES.md
 * 5.6), next to the button's glyph; there the limit is the width in pixels.
 * Capitals only start a sentence (STYLES.md 3.3).
 * The tables stay ASCII (no accents in Spanish) until that restriction is
 * lifted on purpose; citro2d itself could draw them.
 *
 * This module has no 3DS dependencies so the host tests can link it.
 */
#ifndef LANG_H
#define LANG_H

#include <stdbool.h>

typedef enum {
	LANG_EN,
	LANG_ES,
	LANG_COUNT
} lang_id;

typedef enum {
	/* Generic. STR_ERROR is first so a zeroed startup_error reads "Error". */
	STR_ERROR,
	STR_APP_NAME,
	STR_LANG_NAME, /* the language's own name, for the SELECT bubble */
	STR_UNTITLED,
	STR_UNKNOWN_ARTIST,
	STR_NO_VALID_RESPONSE,
	STR_CHECK_SD_SPACE,
	STR_APP_CLOSING,
	STR_LABEL_CODE,

	/* Button hints (one text-screen line each). */
	STR_KEYS_LANGUAGE,
	STR_KEYS_EXIT,
	STR_KEYS_STARTUP_RETRY, /* the last STR_KEYS_ entry */

	/* Home view. */
	STR_HOME_SEARCH,
	STR_HOME_LAST_SEARCH,
	STR_HINT_SEARCH,

	/* Results view. */
	STR_RESULTS_SEARCHING,
	STR_RESULTS_NEW_SEARCH,
	STR_RESULTS_RETRY,
	STR_HINT_PLAY,
	STR_HINT_BACK,
	STR_HINT_CANCEL,
	STR_HINT_RETRY,
	STR_HINT_PAUSE,
	STR_HINT_CHOOSE,
	STR_HINT_PAIR,
	STR_HINT_EXIT,

	/* Settings tab. */
	STR_SETTINGS_LANGUAGE_FMT,
	STR_SETTINGS_RECONNECT,
	STR_SETTINGS_PAIR,

	/* Session start-up. */
	STR_LOGIN_TITLE,
	STR_LOGIN_CONNECTING,
	STR_CREDENTIAL_RENEWING,
	STR_CREDENTIAL_EXPIRED,
	STR_PAIR_AGAIN_NEEDED,
	STR_AP_SEARCHING,
	STR_AP_AUTHENTICATING,
	STR_LIBRARY_OPENING,
	STR_SERVER_RESOLVING,
	STR_CHECK_NETWORK_FMT,
	STR_ERROR_CODE_FMT,
	STR_NO_AP_RESPONDED,
	STR_INVALID_SERVER_RESPONSE,
	STR_CHECK_WIFI_CONNECTED,
	STR_PS_ENTROPY,
	STR_SESSION_FAILED_TITLE,

	/* Start-up error categories. */
	STR_CAT_NO_NETWORK,
	STR_CAT_PS_INIT,
	STR_CAT_TLS_INIT,
	STR_CAT_AP_RESOLVE,
	STR_CAT_AP_REJECTED,
	STR_CAT_AP_CONNECT,
	STR_CAT_LOGIN_REFUSED,
	STR_CAT_LOGIN_FAILED,
	STR_CAT_SERVER_RESOLVE,
	STR_CAT_OAUTH_RENEW,
	STR_CAT_SAVE_RENEWED,
	STR_CAT_PAIRING,
	STR_CAT_PAIR_CANCELLED,
	STR_CAT_PAIR_EXPIRED,
	STR_CAT_PAIR_DENIED,
	STR_CAT_PAIR_FAILED,
	STR_CAT_PAIR_INTERRUPTED,
	STR_CAT_SAVE_CREDENTIALS,

	/* Pairing. */
	STR_PAIR_TITLE,
	STR_PAIR_OPEN_URL,
	STR_PAIR_DIRECT_LINK,
	STR_PAIR_POLL_FMT,
	STR_PAIR_REQUESTING,
	STR_PAIR_WAITING,
	STR_PAIR_STILL_WAITING,
	STR_PAIR_SLOW_DOWN_FMT,
	STR_PAIR_CREDENTIALS_UNCHANGED,
	STR_PAIR_RETRY_NEW_CODE,
	/* The login screens (feature_plan.md, phase 4). */
	STR_LOGIN_STEP_FMT,
	STR_PAIR_HEADING,
	STR_PAIR_HEADING_EXPIRED,
	STR_PAIR_YOUR_CODE,
	STR_PAIR_INSTRUCTIONS,
	STR_PAIR_QR_BODY,
	STR_PAIR_OFFLINE,

	/* Search. */
	STR_SEARCH_HINT,
	STR_SEARCH_CANCELLING,
	STR_SEARCH_FAILED_TITLE,
	STR_SEARCH_HTTP_401,
	STR_SEARCH_HTTP_403,
	STR_SEARCH_HTTP_429,
	STR_SEARCH_CHECK_WIFI,
	STR_ERR_SEARCH_LOAD,
	STR_ERR_SEARCH_THREAD,
	STR_RESULTS_EMPTY,
	STR_RESULTS_IDLE,

	/* Playback. */
	STR_PLAYER_ART_LOADING,
	STR_PLAYER_AUDIO_PREPARING,
	STR_PLAYER_AUDIO_KEY,
	STR_PLAYER_DOWNLOAD_CANCELLING,
	STR_PLAYER_FAILED_TITLE,
	STR_PLAYER_INTERRUPTED,
	STR_PLAYER_FINISHED_TITLE,
	STR_PLAYER_IDLE,
	STR_PLAYER_POSITION_FMT,
	STR_PLAYER_VOLUME,
	STR_ERR_NO_REFRESH_CREDENTIAL,
	STR_ERR_NO_ACCESS_TOKEN,
	STR_ERR_SAVE_RENEWED_OAUTH,
	STR_ERR_AP_SESSION,
	STR_ERR_NDSP,
	STR_ERR_AUDIO_MEMORY,
	STR_ERR_KEY_REFUSED,
	STR_ERR_DOWNLOAD_THREAD,
	STR_ERR_DECODER_OFFSET,
	STR_ERR_VORBIS_CHANNELS_FMT,
	STR_ERR_VORBIS_OPEN_FMT,
	STR_ERR_DECODER_BLOCK,
	STR_ERR_CHANNELS_CHANGED,
	STR_ERR_DOWNLOAD_FAILED_FMT,
	STR_ERR_NO_VORBIS,
	STR_ERR_VORBIS_TOO_LARGE,
	STR_ERR_NO_AUDIO,

	STR_COUNT
} str_id;

/* The current language; English until lang_set is called. */
lang_id lang_get(void);
void lang_set(lang_id lang);
/* The language after `lang`, wrapping around: what SELECT switches to. */
lang_id lang_next(lang_id lang);

/* Short codes ("en", "es") for the config file. */
const char *lang_code(lang_id lang);
bool lang_from_code(const char *code, lang_id *out);

/* Text in a given language, falling back to English (then "") when missing. */
const char *lang_text(lang_id lang, str_id id);
/* Text in the current language. */
const char *tr(str_id id);

#endif
