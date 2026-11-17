/* User-facing text in every supported language.
 *
 * The tables live in lang_en.c and lang_es.c, indexed by str_id. Entries that
 * end in _FMT are printf formats: every language must use the same conversion
 * specifiers in the same order (tests/native_logic_test.c checks this).
 * STR_HINT_ entries label one action each in the button bar (docs/STYLES.md
 * 5.6), next to the button's glyph; the limit is the width in pixels.
 * Capitals only start a sentence (STYLES.md 3.3).
 * The tables are UTF-8, accents and all: ui_parse (ui_font.h) draws any
 * character the console's fonts have. No control characters.
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


	/* Home view. */
	STR_HOME_SEARCH,
	STR_HINT_SEARCH,
	STR_HOME_RECENT,
	STR_HOME_YOUR_PLAYLISTS,
	STR_HOME_SHOW_MORE,
	STR_HOME_EMPTY,
	STR_HOME_FAILED_TITLE,
	STR_HINT_OPEN,
	STR_PLAYLIST_TYPE,
	STR_LIKED_SONGS,
	STR_RESUME_FMT,

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

	/* Detail views (docs/plan_vistas_detalle.md). */
	STR_HINT_DETAIL,
	/* Y in a list: the order its queue plays in, as it is now, and the
	 * bubble when it changes. */
	STR_HINT_SHUFFLE_ON,
	STR_HINT_SHUFFLE_OFF,
	STR_SHUFFLE_ON,
	STR_SHUFFLE_OFF,
	STR_DETAIL_PLAY,
	STR_DETAIL_ARTIST_FMT,
	STR_DETAIL_ALBUM_FMT,
	STR_UNKNOWN_ALBUM,
	STR_DETAIL_LOADING,
	STR_DETAIL_EMPTY,
	STR_DETAIL_FAILED_TITLE,
	/* The detail card on the top screen. Lowercase: they follow a duration
	 * on the same line ("3:42 - track 2 - disc 2"). */
	STR_DETAIL_TRACK_NUMBER_FMT,
	STR_DETAIL_DISC_FMT,
	STR_ALBUM_TYPE_ALBUM,
	STR_ALBUM_TYPE_SINGLE,
	STR_ALBUM_TYPE_COMPILATION,
	STR_ALBUM_TYPE_EP,
	STR_ALBUM_TRACK_ONE,
	STR_ALBUM_TRACKS_FMT,
	STR_ALBUM_TRACKS_CUT_FMT,
	STR_ARTIST_TOP_TRACKS,

	/* Settings tab. */
	STR_SETTINGS_LANGUAGE_FMT,
	STR_SETTINGS_RECONNECT,
	STR_SETTINGS_PAIR,
	/* The top screen of Settings: the account, the credits, the licence
	 * (top_about.h). Names stay as they are, outside the tables. */
	STR_ABOUT_USER_FMT,
	STR_ABOUT_PLAN_PREMIUM,
	STR_ABOUT_PLAN_FREE,
	STR_ABOUT_DEVELOPER,
	STR_ABOUT_BASED_ON,
	STR_ABOUT_QR,
	STR_ABOUT_LICENSE,
	STR_ABOUT_DISCLAIMER,

	/* Session start-up. */
	STR_LOGIN_CONNECTING,
	STR_CREDENTIAL_RENEWING,
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
	STR_CAT_SAVE_CREDENTIALS,

	/* Pairing. */
	STR_PAIR_REQUESTING,
	STR_PAIR_WAITING,
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
	STR_ERR_DETAIL_CONNECT_FMT,
	STR_ERR_DETAIL_THREAD,
	STR_RESULTS_EMPTY,
	STR_RESULTS_IDLE,
	STR_RESULTS_RECENT,

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
	/* The bubble after touching the progress bar before the whole track
	 * has downloaded: it cannot move yet. */
	STR_PLAYER_SEEK_WAIT,
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
	/* Why a download stopped for good, kept with the track in the
	 * downloads index (in the language of the moment it failed). */
	STR_DL_ERR_UNAVAILABLE,
	STR_DL_ERR_NETWORK,
	STR_DL_ERR_DAMAGED,
	STR_DL_ERR_WRITE,
	STR_DL_ERR_NO_SPACE,
	/* What asking for a download says at once (downloads_add_track). */
	STR_DL_ADDED,
	STR_DL_ALREADY,
	STR_DL_NOT_READY,
	STR_DL_UNREADABLE,
	STR_DL_OTHER_ACCOUNT,
	STR_DL_FULL,
	/* The Playing tab while a downloaded track opens from the card. */
	STR_PLAYER_LOCAL_OPENING,
	/* The download buttons: a track's state, a collection's, and the bar. */
	STR_DL_DOWNLOAD,
	STR_DL_QUEUED,
	STR_DL_PROGRESS_FMT,
	STR_DL_RETRY,
	STR_DL_REMOVE,
	STR_DL_REMOVE_CONFIRM,
	STR_DL_REMOVED,
	STR_DL_ALL_ALBUM,
	STR_DL_ALL_PLAYLIST,
	STR_DL_COLL_WAITING,
	STR_DL_COLL_PROGRESS_FMT,
	STR_DL_DONE,
	STR_HINT_DOWNLOAD,
	/* The downloads manager in Settings, and its top screen. */
	STR_SETTINGS_DOWNLOADS,
	STR_SETTINGS_DOWNLOADS_FMT,
	STR_DLV_SECTION_ACTIVE,
	STR_DLV_SECTION_QUEUED_FMT,
	STR_DLV_SECTION_FAILED_FMT,
	STR_DLV_SECTION_DONE_FMT,
	STR_DLV_EMPTY,
	STR_DLV_TRACKS_FMT,
	STR_DLV_CANCEL_CONFIRM,
	STR_DLV_REMOVE_CONFIRM,
	STR_DLV_CANCELLED,
	STR_DLV_PAUSED_NOTICE,
	STR_DLV_RESUMED_NOTICE,
	STR_DLV_SUMMARY_FMT,
	STR_DLV_USED_FMT,
	STR_DLV_FREE_FMT,
	STR_DLV_STATE_RUNNING,
	STR_DLV_STATE_IDLE,
	STR_DLV_STATE_PAUSED,
	STR_DLV_STATE_SD_FULL,
	STR_DLV_STATE_OFFLINE,
	STR_HINT_REMOVE,
	STR_HINT_PAUSE_DOWNLOADS,
	STR_HINT_RESUME_DOWNLOADS,
	/* The Home tab's Downloads and the collections in it. */
	STR_DLH_ALBUM_FMT,
	STR_DLH_PLAYLIST_FMT,
	STR_DLH_ALBUM_PROGRESS_FMT,
	STR_DLH_PLAYLIST_PROGRESS_FMT,
	STR_DLH_SUMMARY_FMT,
	STR_DLH_COLL_SIZE_FMT,
	STR_DLH_EMPTY,
	/* Without a connection: the tabs with the downloads only. */
	STR_OFFLINE_NOTICE,
	STR_OFFLINE_RETRY,
	STR_OFFLINE_STILL,
	STR_OFFLINE_CONNECTING,
	STR_ONLINE_AGAIN,
	STR_OFFLINE_SEARCH,
	STR_ERR_OFFLINE_NOT_DOWNLOADED,

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
