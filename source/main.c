#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds/thread.h>

#include "ap3ds.h"
#include "art_cache.h"
#include "artwork.h"
#include "cfg3ds.h"
#include "details.h"
#include "dl_store.h"
#include "downloads.h"
#include "home_feed.h"
#include "icons.h"
#include "lang.h"
#include "login_logic.h"
#include "login5.h"
#include "logo.h"
#include "pair3ds.h"
#include "player_logic.h"
#include "power_logic.h"
#include "queue_feed.h"
#include "queue_logic.h"
#include "pathfinder.h"
#include "pf_hash.h"
#include "search_results.h"
#include "spclient.h"
#include "tabbar.h"
#include "thumbs.h"
#include "track.h"
#include "tls3ds.h"
#include "top.h"
#include "ui.h"
#include "ui_font.h"
#include "view_detail.h"
#include "view_home.h"
#include "view_login.h"
#include "view_player.h"
#include "view_results.h"
#include "view_downloads.h"
#include "view_settings.h"
#include "settings_logic.h"
#include "worker_priority.h"
#include "stb_vorbis.h"

#define SOC_BUFFER 0x100000
#define SEARCH_LIMIT 24
#define SEARCH_QUERY_SIZE 96
/* 8 x 2048 frames is ~370 ms of audio at 44.1 kHz: enough cushion for the
 * decoder to ride out a burst of TLS work in the download thread. */
#define PLAYER_WAVE_BUFFERS 8
#define PLAYER_FRAMES_PER_BUFFER 2048
#define PLAYER_INPUT_CAPACITY (128 * 1024)
/* Decoding steps per frame at most, so input and drawing stay responsive
 * even if the DSP keeps taking buffers. */
#define PLAYER_PUMP_MAX_STEPS 256

typedef struct {
	char host[128];
	char library_token[1024];
	/* The canonical username, which the playlist paths name. Not a secret,
	 * but personal: never logged, and drawn only on the Settings top
	 * screen, the user's own (top_about.h). */
	char username[128];
	char oauth_token[1280];
	char refresh_token[512];
	time_t oauth_expires_at;
	ap_candidate ap_candidates[AP_MAX_CANDIDATES];
	int ap_candidate_count;
	/* False when the app runs on its downloads alone: the start-up could
	 * not reach Spotify, so `host` and the tokens are empty and `username`
	 * comes from the config. */
	bool online;
} spotify_session;

typedef struct {
	track_stream stream;
	volatile bool stop;
	volatile bool done;
	int result;
	Thread thread;
} download_job;

typedef struct {
	char query[SEARCH_QUERY_SIZE];
	char host[128];
	char library_token[1024];
	sp_track tracks[SEARCH_LIMIT];
	int count;
	/* The other kinds (search_results.h). `offered`: what the source
	 * gives at all -- pathfinder every kind, the fallback no playlists.
	 * The best result is one of the four lists, or best_kind -1. */
	sp_track artists[SEARCH_ITEMS_MAX];
	sp_track albums[SEARCH_ITEMS_MAX];
	sp_track playlists[SEARCH_ITEMS_MAX];
	int kind_count[RESULT_KINDS];
	bool offered[RESULT_KINDS];
	int best_kind;
	int best_index;
	int result;
	char error[192];
	volatile bool cancel_requested;
	volatile bool done;
} search_job;

typedef struct {
	/* An id rather than text so the screen follows a language switch. */
	str_id category;
	char detail[256];
	/* No answer came at all: the network failed, not the account
	 * (login_error_allows_offline). */
	bool transport;
} startup_error;

static search_job search;
static Thread search_thread;
/* The results (SEARCH_LIMIT) or a detail's list, a playlist's loaded pages
 * included (DETAIL_PLAYLIST_MAX, ~1 KB a track). */
#define PLAYBACK_QUEUE_MAX DETAIL_PLAYLIST_MAX
static sp_track playback_queue[PLAYBACK_QUEUE_MAX];
static int playback_queue_count;
/* What plays when: playback_queue's indexes in the order they play, and
 * the position playing (queue_logic.h). The tracks keep their list's
 * order, so shuffling only rewrites this. */
static int playback_order[PLAYBACK_QUEUE_MAX];
static int queue_pos;
static uint32_t queue_rng;
/* Lists play shuffled: the setting, kept in the config. It applies to a
 * queue that came from a list (an album, an artist, a playlist). */
static bool shuffle_on;
/* Where the queue came from, which says what it grows with (queue_feed.h):
 * a list is all there is; a playlist still has pages its detail had not
 * loaded; a radio, started from a single track, asks for more as it runs
 * out. */
typedef enum { QUEUE_LIST, QUEUE_PLAYLIST, QUEUE_RADIO } queue_kind;
static queue_kind queue_source;
static char queue_playlist[SP_URI_MAX];   /* QUEUE_PLAYLIST: its URI... */
static int queue_next_from;               /* ...and the first item not read */
/* QUEUE_RADIO: the seed is this many tracks before the last; it moves back
 * while batches bring nothing new. */
static int queue_seed_back;
/* Bumped by every new queue, so a load asked for an old one is dropped. */
static unsigned queue_serial;
/* After a load failed, or brought nothing, not again before this. */
static uint64_t queue_retry_at;
#define QUEUE_RETRY_MS 5000
static float playback_volume = 0.7f;
/* The volume, in whole percent, is kept in the config. The bar changes it
 * every frame while dragged, so it is written once, on leaving, with
 * whatever it ended at. */
static int volume_saved_pct = -1;
static artwork_state cover_art;
/* The image of the detail card on the top screen, apart from cover_art:
 * the player waits for that one before preparing a track, so a detail
 * must never ask it for another image. `detail_art_url` is what it holds,
 * so going back to the same detail costs no download. */
static artwork_state detail_art;
static char detail_art_url[80];
/* The account signed in and its picture, for the Settings top screen
 * (account_start). */
static about_account account;
static artwork_state account_art;
static char account_art_url[80];
/* The last confirmed query, for the home view's repeat button. */
static char home_query[SEARCH_QUERY_SIZE];
/* Who owns the screens: the login view for the session start-up, pairing
 * and errors; the tabs once the session is up. */
typedef enum { VIEW_LOGIN, VIEW_TABS } screen_view;
static screen_view active_view = VIEW_LOGIN;
static app_tab active_tab = TAB_HOME;
/* The tabs run with a session that reached Spotify. */
static bool net_online = true;
/* Sockets and TLS came up: without them no connection can be tried. */
static bool net_ready = true;
/* Settings shows the downloads manager instead of its buttons. */
static bool manager_open;
/* An applet (the keyboard) drew over both screens. */
static bool screens_lost;

/* Closing the lid over music darkens the screens instead of sleeping. */
static bool ptmu_ready;
static bool lcd_ready;   /* gsp::Lcd can be opened; never kept open */

/* gsp::Lcd is opened only around each call, as ftpd does: a session held
 * for the whole run left the HOME menu hung once the app handed it the
 * console, and the console had to be rebooted. */
static void set_backlights(bool on)
{
	if (R_FAILED(gspLcdInit()))
		return;
	if (on)
		GSPLCD_PowerOnAllBacklights();
	else
		GSPLCD_PowerOffAllBacklights();
	gspLcdExit();
}
static bool screens_dark;
/* NDM drops the Wi-Fi when the lid shuts, sleep or not, unless the app
 * holds the connection in the exclusive infrastructure state. */
static bool ndm_ready;
static bool wifi_held;

static void wifi_hold(bool hold)
{
	if (!ndm_ready || hold == wifi_held)
		return;
	if (hold) {
		if (R_FAILED(NDMU_EnterExclusiveState(
		        NDM_EXCLUSIVE_STATE_INFRASTRUCTURE)))
			return;
		NDMU_LockState();
	} else {
		NDMU_UnlockState();
		NDMU_LeaveExclusiveState();
	}
	wifi_held = hold;
}

static void frame(void);
static void switch_tab(app_tab tab);
static void account_start(const spotify_session *session);
static void account_poll(void);

static void secure_clear(void *data, size_t len)
{
	volatile unsigned char *p = data;
	while (len--)
		*p++ = 0;
}

/* Starts with the language saved in the config, else the console's system
 * language when it is Spanish, else English. */
static void language_init(void)
{
	char code[8];
	lang_id lang;
	if (cfg_get("language", code, sizeof(code)) &&
	    lang_from_code(code, &lang)) {
		lang_set(lang);
		return;
	}
	u8 system = CFG_LANGUAGE_EN;
	if (R_SUCCEEDED(cfguInit())) {
		if (R_FAILED(CFGU_GetSystemLanguage(&system)))
			system = CFG_LANGUAGE_EN;
		cfguExit();
	}
	lang_set(system == CFG_LANGUAGE_ES ? LANG_ES : LANG_EN);
}

/* Shuffle, as it was left. */
static void shuffle_init(void)
{
	char value[4];
	shuffle_on = cfg_get("shuffle", value, sizeof(value)) &&
	             strcmp(value, "1") == 0;
	view_detail_set_shuffle(shuffle_on);
}

/* The volume, as it was left; the default with none kept or a bad one. */
static void volume_init(void)
{
	char value[8];
	if (!cfg_get("volume", value, sizeof(value)))
		return;
	char *end;
	long pct = strtol(value, &end, 10);
	if (end == value || *end || pct < 0 || pct > 100)
		return;
	volume_saved_pct = (int)pct;
	playback_volume = pct / 100.0f;
}

/* Writes the volume if it changed. Only as the app closes. */
static void volume_save(void)
{
	int pct = (int)(playback_volume * 100.0f + 0.5f);
	if (pct == volume_saved_pct)
		return;
	char value[8];
	snprintf(value, sizeof(value), "%d", pct);
	cfg_set_async("volume", value);
	volume_saved_pct = pct;
}

/* The last searches, newest first, kept in the config (recent_logic.h). */
_Static_assert(SEARCH_QUERY_SIZE == RECENT_QUERY_SIZE,
               "a recent search holds a whole query");
static recent_list recent_searches;

static void recent_init(void)
{
	char value[RECENT_SERIAL_SIZE];
	recent_parse(&recent_searches,
	             cfg_get("searches", value, sizeof(value)) ? value : NULL);
	view_results_set_recent(&recent_searches);
}

/* `query` on top of the recent searches, on the card at once. */
static void recent_remember(const char *query)
{
	if (!recent_push(&recent_searches, query))
		return;
	char value[RECENT_SERIAL_SIZE];
	if (recent_serialize(&recent_searches, value, sizeof(value)))
		cfg_set_async("searches", value);
	view_results_set_recent(&recent_searches);
}

/* SELECT, or the language button in Settings, switches the language from
 * any screen and remembers the choice. */
static void switch_language(void)
{
	lang_set(lang_next(lang_get()));
	cfg_set_async("language", lang_code(lang_get()));
}

/* Holding up or down, on the D-pad or the Circle Pad, keeps moving through
 * the lists and between a view's buttons: a first step at once, then one
 * more after NAV_REPEAT_DELAY frames and every NAV_REPEAT_INTERVAL after
 * that (hidKeysDownRepeat). */
#define NAV_REPEAT_DELAY 24
#define NAV_REPEAT_INTERVAL 6
#define NAV_REPEAT_KEYS (KEY_UP | KEY_DOWN)

/* This frame's new presses, with up and down again while held. */
static u32 keys_down(void)
{
	return hidKeysDown() | (hidKeysDownRepeat() & NAV_REPEAT_KEYS);
}

static void download_worker(void *arg)
{
	download_job *job = arg;
	/* A download plays from the card; the same buffer, chunks and seek. */
	job->result = job->stream.local
	              ? track_stream_load_local(&job->stream, &job->stop)
	              : track_stream_download(&job->stream, &job->stop);
	__sync_synchronize();
	job->done = true;
}

static bool ensure_oauth_token(spotify_session *session, char *error,
                               size_t error_size)
{
	if (session->oauth_token[0] &&
	    session->oauth_expires_at > time(NULL) + 60)
		return true;
	if (!session->refresh_token[0]) {
		snprintf(error, error_size, "%s", tr(STR_ERR_NO_REFRESH_CREDENTIAL));
		return false;
	}

	spotify_tokens tokens;
	memset(&tokens, 0, sizeof(tokens));
	if (token_refresh(session->refresh_token, &tokens, error, error_size) != 0) {
		secure_clear(&tokens, sizeof(tokens));
		return false;
	}
	if (!tokens.access_token[0]) {
		snprintf(error, error_size, "%s", tr(STR_ERR_NO_ACCESS_TOKEN));
		secure_clear(&tokens, sizeof(tokens));
		return false;
	}
	if (tokens.refresh_token[0] &&
	    strcmp(tokens.refresh_token, session->refresh_token) != 0) {
		if (!cfg_save_refresh_token(tokens.refresh_token, tokens.scope)) {
			snprintf(error, error_size, "%s",
			         tr(STR_ERR_SAVE_RENEWED_OAUTH));
			secure_clear(&tokens, sizeof(tokens));
			return false;
		}
		snprintf(session->refresh_token, sizeof(session->refresh_token),
		         "%s", tokens.refresh_token);
	}
	snprintf(session->oauth_token, sizeof(session->oauth_token), "%s",
	         tokens.access_token);
	session->oauth_expires_at = tokens.expires_at;
	secure_clear(&tokens, sizeof(tokens));
	return true;
}

/* Two workers may log in to an access point at once: the player's
 * preparation and the downloads' (downloads.h). The session's OAuth token
 * and its access point list are what they share; this lock is held while
 * one renews or copies them, never across a login. */
static LightLock session_lock;

static ap_session *open_playback_ap(spotify_session *session, char *error,
                                    size_t error_size)
{
	/* Off the stack of either worker: the token is 1.3 KB. */
	char *oauth = malloc(sizeof(session->oauth_token));
	if (!oauth) {
		snprintf(error, error_size, "%s", tr(STR_ERR_AP_SESSION));
		return NULL;
	}
	ap_candidate candidates[AP_MAX_CANDIDATES];
	LightLock_Lock(&session_lock);
	bool token_ok = ensure_oauth_token(session, error, error_size);
	int count = session->ap_candidate_count;
	bool had_candidates = count > 0;
	if (token_ok) {
		memcpy(oauth, session->oauth_token, sizeof(session->oauth_token));
		if (count > 0)
			memcpy(candidates, session->ap_candidates,
			       (size_t)count * sizeof(candidates[0]));
	}
	LightLock_Unlock(&session_lock);
	if (!token_ok) {
		free(oauth);
		return NULL;
	}
	if (count < 1)
		count = ap_resolve_list(candidates, AP_MAX_CANDIDATES,
		                        error, error_size);
	if (count < 1) {
		secure_clear(oauth, sizeof(session->oauth_token));
		free(oauth);
		return NULL;
	}

	char device_id[64];
	ap_device_id(device_id, sizeof(device_id));
	ap_info info;
	memset(&info, 0, sizeof(info));
	int tried = 0;
	ap_session *ap = ap_login_any(candidates, count, oauth, device_id, &info,
	                              &tried, error, error_size);
	if (!ap && info.login_error == 0 && had_candidates) {
		count = ap_resolve_list(candidates, AP_MAX_CANDIDATES,
		                        error, error_size);
		if (count > 0) {
			memset(&info, 0, sizeof(info));
			ap = ap_login_any(candidates, count, oauth, device_id, &info,
			                  &tried, error, error_size);
			if (ap) {
				LightLock_Lock(&session_lock);
				memcpy(session->ap_candidates, candidates,
				       (size_t)count * sizeof(candidates[0]));
				session->ap_candidate_count = count;
				LightLock_Unlock(&session_lock);
			}
		}
	}
	secure_clear(oauth, sizeof(session->oauth_token));
	free(oauth);
	secure_clear(device_id, sizeof(device_id));
	secure_clear(&info, sizeof(info));
	if (!ap && !error[0])
		snprintf(error, error_size, "%s", tr(STR_ERR_AP_SESSION));
	return ap;
}

/* The session the downloads log in with, while the tabs are open. */
static spotify_session *downloads_session;

static ap_session *downloads_open_ap(char *error, size_t error_size)
{
	return open_playback_ap(downloads_session, error, error_size);
}

/* --- Player ------------------------------------------------------------
 *
 * The audio engine. It is not a screen any more: player_play starts a
 * track, player_pump keeps the DSP fed once per frame from the main loop
 * whichever tab is showing, and the Playing tab (view_player.h) only reads
 * player_snapshot. A track's network steps (token, access point, audio
 * key) run in a preparation thread that player_pump polls, so the input
 * and the screens keep answering while they take their time. Only a
 * session going away waits for them. */

/* Everything one track holds; zeroed between tracks. */
typedef struct {
	ndspWaveBuf waves[PLAYER_WAVE_BUFFERS];
	bool wave_submitted[PLAYER_WAVE_BUFFERS];
	uint8_t *input;
	int16_t *audio;
	bool ndsp_ready;
	stb_vorbis *decoder;
	bool decoder_open;
	bool eof;
	bool final_wave_submitted;
	size_t input_len;
	size_t source_pos;
	size_t pcm_frames;
	size_t total_audio_frames;
	size_t completed_audio_frames;
	size_t displayed_played_frames;
	int16_t pcm[PLAYER_FRAMES_PER_BUFFER * 2];
	float **decoded;
	int decoded_samples;
	int decoded_offset;
	stb_vorbis_info info;
	/* This track's preparation thread has been started. */
	bool preparing;
	/* After a seek: the clock shows `seek_frame`, the sample asked for,
	 * until the decoder finds a page and knows where it really is; the
	 * frames it decoded since the seek, to work that out; the byte the
	 * decoder was sent to, and whether that was already corrected once. */
	bool seek_sync;
	long long seek_frame;
	long long seek_decoded;
	size_t seek_byte;
	bool seek_refined;
} player_stream;

typedef struct {
	player_phase phase;
	int index;              /* into playback_queue: playback_order[queue_pos] */
	unsigned serial;        /* bumped by every player_play */
	str_id stage;           /* while loading */
	char error[256];        /* when failed */
	bool paused;            /* by the user */
	bool held;              /* by the app, while the keyboard is open */
	spotify_session *session;
	player_stream s;
} player_engine;

static player_engine player;

/* The DSP is set up once and kept for the whole run: starting it again for
 * every track meant starting it with the lid already shut whenever the
 * user closed it during loading, and then the track never sounded. */
static bool ndsp_up;

static bool audio_open(void)
{
	if (!ndsp_up)
		ndsp_up = R_SUCCEEDED(ndspInit());
	return ndsp_up;
}

/* A track's network steps, off the main thread: everything that may wait
 * on the network before the first byte of audio. One at a time: a skipped
 * track's preparation is left to finish ("abandoned") and reaped, and the
 * next one only starts after it, so two never touch the session at once. */
typedef struct {
	spotify_session *session;
	sp_track track;          /* a copy: the queue may change meanwhile */
	/* The track is downloaded: played from the card, with no network. */
	bool local;
	downloads_local local_src;
	track_stream stream;     /* out, taken by the player when it is used */
	char error[256];
	int rc;                  /* 0, AP_KEY_REFUSED or -1 */
	int key_rc;              /* the key request's own: -1 when not asked */
	int key_code;            /* the refusal's code, when refused */
	volatile str_id stage;   /* what it is doing, for the Playing tab */
	volatile bool done;
	bool abandoned;          /* main thread: nobody wants the result */
	Thread thread;
} prepare_job;
static prepare_job prep;
#define PREPARE_STACK (64 * 1024)

/* The current track's audio download. Not part of player_stream: its
 * worker holds this address, and a skipped track's download is left to
 * notice `stop` ("abandoned") and reaped later rather than waited for.
 * The next track only starts preparing once it is gone, so there is never
 * more than one, nor two audio buffers at once. */
static download_job dl;
static bool dl_abandoned;

static void set_playback_volume(float volume)
{
	if (volume < 0.0f)
		volume = 0.0f;
	if (volume > 1.0f)
		volume = 1.0f;
	playback_volume = volume;
	if (!player.s.ndsp_ready)
		return;
	float mix[12] = {0};
	mix[0] = mix[1] = playback_volume;
	ndspChnSetMix(0, mix);
}

static void player_stage(str_id stage)
{
	player.stage = stage;
	frame();
}

/* Joins the download (it has returned, or never started) and frees its
 * buffer. */
static void download_reap(void)
{
	__sync_synchronize();
	if (dl.thread) {
		threadJoin(dl.thread, U64_MAX);
		threadFree(dl.thread);
	}
	if (dl.stream.data)
		secure_clear(dl.stream.data, dl.stream.len);
	track_stream_free(&dl.stream);
	secure_clear(&dl, sizeof(dl));
	dl_abandoned = false;
}

/* The session is going away: the download must be over. */
static void download_wait(void)
{
	dl.stop = true;
	while (dl.thread && !dl.done && aptMainLoop())
		player_stage(STR_PLAYER_DOWNLOAD_CANCELLING);
	download_reap();
}

static void prepare_worker(void *arg)
{
	prepare_job *job = arg;
	job->key_rc = -1;
	if (job->local) {
		/* Downloaded: the key is on the card, wrapped, and the file too.
		 * It also saves the data and the access point's handshake when
		 * there is a connection. */
		job->stage = STR_PLAYER_LOCAL_OPENING;
		uint8_t key[16];
		bool opened = dl_store_unwrap_key(job->local_src.salt,
		                                  job->track.uri,
		                                  job->local_src.wrapped, key) &&
		              track_stream_open_local(job->local_src.path, key,
		                                      &job->track,
		                                      job->local_src.format,
		                                      &job->stream, job->error,
		                                      sizeof(job->error)) == 0;
		secure_clear(key, sizeof(key));
		secure_clear(&job->local_src, sizeof(job->local_src));
		if (opened) {
			job->rc = 0;
			__sync_synchronize();
			job->done = true;
			return;
		}
		/* A file that went missing, or a key from another console: the
		 * network still has the track. */
		job->error[0] = '\0';
	}
	/* Without a connection there is nowhere else to get it from. */
	if (!job->session->online) {
		snprintf(job->error, sizeof(job->error), "%s",
		         tr(STR_ERR_OFFLINE_NOT_DOWNLOADED));
		job->rc = -1;
		__sync_synchronize();
		job->done = true;
		return;
	}
	job->stage = STR_AP_AUTHENTICATING;
	ap_session *ap = open_playback_ap(job->session, job->error,
	                                  sizeof(job->error));
	if (!ap) {
		job->rc = -1;
	} else {
		job->stage = STR_PLAYER_AUDIO_KEY;
		job->rc = track_stream_begin_cached(job->session->host,
		                                    job->session->library_token,
		                                    ap, &job->track, NULL,
		                                    &job->stream, job->error,
		                                    sizeof(job->error));
		ap_close(ap);
		/* The key counts against the allowance the downloads share. */
		if (job->rc == 0 || job->rc == AP_KEY_REFUSED) {
			job->key_rc = job->rc;
			job->key_code = job->stream.key_refused_code;
		}
	}
	__sync_synchronize();
	job->done = true;
}

/* Joins a finished preparation (it costs nothing then) and frees whatever
 * of its result nobody took. */
static void prepare_reap(void)
{
	__sync_synchronize();
	threadJoin(prep.thread, U64_MAX);
	threadFree(prep.thread);
	downloads_note_player_key(prep.key_rc, prep.key_code);
	track_stream_free(&prep.stream);
	secure_clear(&prep, sizeof(prep));
}

/* The session is going away: the preparation must be over before it is
 * cleared. The only place that waits for it. */
static void prepare_wait(void)
{
	while (prep.thread && !prep.done && aptMainLoop())
		player_stage(STR_PLAYER_DOWNLOAD_CANCELLING);
	if (prep.thread)
		prepare_reap();
}

/* Frees what the current track holds, in this order, and forgets it. */
static void player_release(void)
{
	player_stream *s = &player.s;
	/* A preparation still on the network is not waited for: it is left
	 * to finish and reaped by player_pump. */
	if (prep.thread) {
		if (prep.done)
			prepare_reap();
		else
			prep.abandoned = true;
	}
	/* Same for the download: asked to stop, it is reaped by player_pump
	 * once its request in flight lets it notice. An abandoned one is not
	 * this track's. */
	if (!dl_abandoned) {
		if (dl.thread && !dl.done) {
			dl.stop = true;
			dl_abandoned = true;
		} else {
			download_reap();
		}
	}
	if (s->decoder)
		stb_vorbis_close(s->decoder);
	if (s->ndsp_ready) {
		/* The DSP stays up for the next track; only the channel goes. */
		ndspChnWaveBufClear(0);
		ndspChnReset(0);
	}
	if (s->input) {
		secure_clear(s->input, PLAYER_INPUT_CAPACITY);
		free(s->input);
	}
	if (s->audio)
		linearFree(s->audio);
	secure_clear(s, sizeof(*s));
}

static void player_fail(const char *error)
{
	player_release();
	snprintf(player.error, sizeof(player.error), "%s", error);
	player.phase = PLAYER_PHASE_FAILED;
}

static const sp_track *player_track(void)
{
	return player.phase == PLAYER_PHASE_IDLE
	       ? NULL : &playback_queue[player.index];
}

/* The DSP is paused by the user or by the app. */
static void player_apply_pause(void)
{
	if (player.s.ndsp_ready)
		ndspChnSetPaused(0, player.paused || player.held);
}

/* Offline: the first position from `pos` (itself included) going
 * `direction` whose track is on the card, or -1. */
static int offline_playable(int pos, int direction)
{
	static bool playable[PLAYBACK_QUEUE_MAX];
	static downloads_local local;
	for (int i = 0; i < playback_queue_count; i++)
		playable[i] = downloads_local_track(playback_queue[i].uri, &local);
	secure_clear(&local, sizeof(local));
	return playback_next_playable(pos - direction, playback_queue_count,
	                              direction, playback_order, playable);
}

/* Starts the track at position `pos` of the queue's order: the cover
 * first, then player_pump goes on with the audio once it has arrived. */
static void player_play(spotify_session *session, int pos)
{
	if (pos < 0 || pos >= playback_queue_count)
		return;
	/* Offline only downloads play: the next one on the card the way the
	 * queue was going, or none (the track asked for says so). */
	bool none = false;
	if (session && !session->online) {
		int found = offline_playable(pos, pos < queue_pos ? -1 : 1);
		if (found >= 0)
			pos = found;
		else
			none = true;
	}
	int index = playback_order[pos];
	player_release();
	player.session = session;
	player.serial++;
	queue_pos = pos;
	player.index = index;
	player.paused = false;
	player.error[0] = '\0';
	player.phase = PLAYER_PHASE_LOADING;
	player.stage = STR_PLAYER_ART_LOADING;
	artwork_request(&cover_art, playback_queue[index].art_large);
	artwork_set_preview(&cover_art, playback_queue[index].art_small);
	if (none)
		player_fail(tr(STR_ERR_OFFLINE_NOT_DOWNLOADED));
}

/* Starts the track's preparation thread. Not while a skipped track's is
 * still running, nor a skipped track's download, nor while the thumbnails
 * finish their last request or the downloads their current one (they must
 * not compete with the preparation, STYLES.md 9): it waits, a frame at a
 * time, without blocking. */
static void player_start_prepare(void)
{
	player.stage = STR_PLAYER_AUDIO_PREPARING;
	if (prep.thread || dl.thread || thumbs_stopping() || downloads_busy())
		return;
	memset(&prep, 0, sizeof(prep));
	prep.session = player.session;
	prep.track = playback_queue[player.index];
	prep.local = downloads_local_track(prep.track.uri, &prep.local_src);
	prep.stage = STR_PLAYER_AUDIO_PREPARING;
	prep.thread = threadCreate(prepare_worker, &prep, PREPARE_STACK,
	                           background_priority(), -2, false);
	if (!prep.thread) {
		player_fail(tr(STR_ERR_DOWNLOAD_THREAD));
		return;
	}
	player.s.preparing = true;
}

/* Once the preparation has finished, takes its stream and sets the audio up.
 * Nothing here waits on the network. */
static void player_begin_audio(void)
{
	player_stream *s = &player.s;
	char error[256];
	/* `done` was seen: the worker's writes before it are visible. */
	__sync_synchronize();
	int rc = prep.rc;
	snprintf(error, sizeof(error), "%s", prep.error);
	/* The stream is the player's from here: player_release frees it. */
	dl.stream = prep.stream;
	memset(&prep.stream, 0, sizeof(prep.stream));
	prepare_reap();
	if (rc != 0) {
		if (!error[0])
			snprintf(error, sizeof(error), "%s",
			         tr(rc == AP_KEY_REFUSED ? STR_ERR_KEY_REFUSED
			                                 : STR_ERR_AP_SESSION));
		player_fail(error);
		return;
	}

	if (!audio_open()) {
		player_fail(tr(STR_ERR_NDSP));
		return;
	}
	s->ndsp_ready = true;
	ndspChnSetPaused(0, false);

	s->input = malloc(PLAYER_INPUT_CAPACITY);
	s->audio = linearAlloc(PLAYER_FRAMES_PER_BUFFER * 4 * PLAYER_WAVE_BUFFERS);
	if (!s->input || !s->audio) {
		player_fail(tr(STR_ERR_AUDIO_MEMORY));
		return;
	}
	for (int i = 0; i < PLAYER_WAVE_BUFFERS; i++) {
		s->waves[i].data_vaddr = (uint8_t *)s->audio +
		                         i * PLAYER_FRAMES_PER_BUFFER * 4;
		s->waves[i].nsamples = PLAYER_FRAMES_PER_BUFFER;
		s->waves[i].status = NDSP_WBUF_DONE;
	}

	s->source_pos = SPOTIFY_OGG_HEADER;

	dl.thread = threadCreate(download_worker, &dl, 32 * 1024,
	                         background_priority(), -2, false);
	if (!dl.thread) {
		player_fail(tr(STR_ERR_DOWNLOAD_THREAD));
		return;
	}

	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
	ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
	set_playback_volume(playback_volume);
	player_apply_pause();
	player.phase = PLAYER_PHASE_PLAYING;
}

static int free_wave_index(ndspWaveBuf waves[PLAYER_WAVE_BUFFERS])
{
	for (int i = 0; i < PLAYER_WAVE_BUFFERS; i++)
		if (waves[i].status == NDSP_WBUF_DONE ||
		    waves[i].status == NDSP_WBUF_FREE)
			return i;
	return -1;
}

static void submit_wave(ndspWaveBuf *wave, size_t frames)
{
	wave->nsamples = (u32)frames;
	DSP_FlushDataCache(wave->data_vaddr,
	                   PLAYER_FRAMES_PER_BUFFER * 4);
	ndspChnWaveBufAdd(0, wave);
}

/* Queues the PCM gathered so far in wave buffer `index`. */
static void submit_pcm(int index)
{
	player_stream *s = &player.s;
	s->waves[index].data_pcm16 =
		(int16_t *)((uint8_t *)s->audio +
		index * PLAYER_FRAMES_PER_BUFFER * 4);
	memcpy(s->waves[index].data_pcm16, s->pcm, s->pcm_frames * 4);
	submit_wave(&s->waves[index], s->pcm_frames);
	s->wave_submitted[index] = true;
	s->total_audio_frames += s->pcm_frames;
	s->pcm_frames = 0;
}

typedef enum {
	PUMP_AGAIN,   /* made progress: go on in this frame */
	PUMP_YIELD,   /* the DSP is full or the download is behind */
	PUMP_END      /* finished or failed: player.phase says which */
} pump_step;

static pump_step pump_end(bool success, const char *error)
{
	if (success) {
		player_release();
		player.phase = PLAYER_PHASE_FINISHED;
	} else {
		player_fail(error);
	}
	return PUMP_END;
}

/* Sends the decoder to byte `at` and asks the download for what comes
 * there (track_stream_seek). The audio queued so far is dropped; the clock
 * shows `frame` until the decoder finds where it landed. */
static void seek_to_byte(size_t at, long long frame)
{
	player_stream *s = &player.s;
	ndspChnWaveBufClear(0);
	for (int i = 0; i < PLAYER_WAVE_BUFFERS; i++) {
		s->waves[i].status = NDSP_WBUF_DONE;
		s->wave_submitted[i] = false;
	}
	/* From here it scans for the next Ogg page, which tells it the
	 * sample it is at. */
	stb_vorbis_flush_pushdata(s->decoder);
	s->decoded = NULL;
	s->decoded_samples = s->decoded_offset = 0;
	s->input_len = 0;
	s->pcm_frames = 0;
	s->eof = false;
	s->final_wave_submitted = false;
	s->source_pos = at;
	s->completed_audio_frames = (size_t)frame;
	s->displayed_played_frames = (size_t)frame;
	s->seek_sync = true;
	s->seek_frame = frame;
	s->seek_decoded = 0;
	s->seek_byte = at;
	track_stream_seek(&dl.stream, at);
}

/* The byte where sample `frame` should be, as if the bit rate were even:
 * Vorbis is not, so the guess is corrected once the decoder knows where it
 * landed (seek_settle). */
static size_t seek_guess(long long frame, long long total_frames)
{
	size_t body = dl.stream.len - SPOTIFY_OGG_HEADER;
	if (total_frames <= 0 || frame <= 0)
		return SPOTIFY_OGG_HEADER;
	if (frame >= total_frames)
		return dl.stream.len;
	return SPOTIFY_OGG_HEADER +
	       (size_t)((unsigned long long)body * (unsigned long long)frame /
	                (unsigned long long)total_frames);
}

/* A frame of `samples` came out after a seek: once the decoder knows the
 * sample it ends at, the clock follows the real position. Landed more
 * than PLAYER_SEEK_SLACK_S off, it seeks once more, from where it landed,
 * at the file's average rate. */
#define PLAYER_SEEK_SLACK_S 2
static void seek_settle(int samples)
{
	player_stream *s = &player.s;
	s->seek_decoded += samples;
	int offset = stb_vorbis_get_sample_offset(s->decoder);
	if (offset < 0)
		return;
	s->seek_sync = false;
	long long landed = (long long)offset - s->seek_decoded;
	if (landed < 0)
		landed = 0;
	long long off = landed - s->seek_frame;
	long long slack = (long long)PLAYER_SEEK_SLACK_S * s->info.sample_rate;
	const sp_track *track = player_track();
	long long total = track && track->duration_ms > 0
	                  ? (long long)track->duration_ms * s->info.sample_rate /
	                    1000 : 0;
	if (!s->seek_refined && total > 0 && (off > slack || off < -slack)) {
		s->seek_refined = true;
		long long body = (long long)(dl.stream.len - SPOTIFY_OGG_HEADER);
		long long at = (long long)s->seek_byte - off * body / total;
		if (at < SPOTIFY_OGG_HEADER)
			at = SPOTIFY_OGG_HEADER;
		if (at > (long long)dl.stream.len)
			at = (long long)dl.stream.len;
		seek_to_byte((size_t)at, s->seek_frame);
		return;
	}
	/* The clock counts from where the audio after the seek starts. */
	long long base = (long long)s->completed_audio_frames + off;
	s->completed_audio_frames = base < 0 ? 0 : (size_t)base;
	s->displayed_played_frames = s->completed_audio_frames;
}

/* Moves the track playing to `permille` of its length, paused or not.
 * Only once the whole file is downloaded: seeking into a part still on
 * its way left the track silent on the console. */
static void player_seek(int permille)
{
	player_stream *s = &player.s;
	const sp_track *track = player_track();
	if (player.phase != PLAYER_PHASE_PLAYING || !s->decoder_open ||
	    dl_abandoned || !dl.stream.data || !dl.stream.dl_done ||
	    dl.stream.len <= SPOTIFY_OGG_HEADER || !track ||
	    track->duration_ms <= 0)
		return;
	if (permille < 0)
		permille = 0;
	if (permille > 1000)
		permille = 1000;
	long long total = (long long)track->duration_ms * s->info.sample_rate /
	                  1000;
	long long frame = total * permille / 1000;
	s->seek_refined = false;
	seek_to_byte(seek_guess(frame, total), frame);
}

/* One step of download -> Vorbis -> PCM -> wave buffers. */
static pump_step pump_once(void)
{
	player_stream *s = &player.s;
	char error[256];

	for (int i = 0; i < PLAYER_WAVE_BUFFERS; i++) {
		if (s->wave_submitted[i] && s->waves[i].status == NDSP_WBUF_DONE) {
			s->completed_audio_frames += s->waves[i].nsamples;
			s->wave_submitted[i] = false;
		}
	}

	size_t available = track_stream_available(&dl.stream, s->source_pos);
	if (dl.stream.data && available > s->source_pos &&
	    s->input_len < PLAYER_INPUT_CAPACITY) {
		size_t copy = available - s->source_pos;
		if (copy > PLAYER_INPUT_CAPACITY - s->input_len)
			copy = PLAYER_INPUT_CAPACITY - s->input_len;
		memcpy(s->input + s->input_len, dl.stream.data + s->source_pos,
		       copy);
		s->input_len += copy;
		s->source_pos += copy;
	}

	if (!s->decoder && s->input_len > 0) {
		int consumed = 0;
		int vorbis_error = 0;
		s->decoder = stb_vorbis_open_pushdata(s->input, (int)s->input_len,
		                                      &consumed, &vorbis_error,
		                                      NULL);
		if (s->decoder) {
			if (consumed < 0 || (size_t)consumed > s->input_len)
				return pump_end(false, tr(STR_ERR_DECODER_OFFSET));
			memmove(s->input, s->input + consumed,
			        s->input_len - (size_t)consumed);
			s->input_len -= (size_t)consumed;
			s->info = stb_vorbis_get_info(s->decoder);
			if ((s->info.channels != 1 && s->info.channels != 2) ||
			    s->info.sample_rate == 0) {
				snprintf(error, sizeof(error),
				         tr(STR_ERR_VORBIS_CHANNELS_FMT), s->info.channels);
				return pump_end(false, error);
			}
			ndspChnSetRate(0, (float)s->info.sample_rate);
			s->decoder_open = true;
		} else if (vorbis_error != VORBIS_need_more_data) {
			snprintf(error, sizeof(error), tr(STR_ERR_VORBIS_OPEN_FMT),
			         vorbis_error);
			return pump_end(false, error);
		}
	}

	int wave_index = free_wave_index(s->waves);
	if (s->pcm_frames == PLAYER_FRAMES_PER_BUFFER) {
		if (wave_index < 0)
			return PUMP_YIELD;
		submit_pcm(wave_index);
		return PUMP_AGAIN;
	}

	if (s->decoded && s->decoded_offset < s->decoded_samples) {
		size_t amount = (size_t)(s->decoded_samples - s->decoded_offset);
		size_t room = PLAYER_FRAMES_PER_BUFFER - s->pcm_frames;
		if (amount > room)
			amount = room;
		for (size_t i = 0; i < amount; i++) {
			for (int channel = 0; channel < 2; channel++) {
				float sample = s->decoded[s->info.channels == 1 ? 0 : channel]
				                         [s->decoded_offset + (int)i];
				if (sample > 1.0f) sample = 1.0f;
				if (sample < -1.0f) sample = -1.0f;
				int value = (int)(sample * 32767.0f);
				s->pcm[(s->pcm_frames + i) * 2 + channel] = (int16_t)value;
			}
		}
		s->decoded_offset += (int)amount;
		s->pcm_frames += amount;
		if (s->decoded_offset == s->decoded_samples) {
			s->decoded = NULL;
			s->decoded_samples = s->decoded_offset = 0;
		}
		return PUMP_AGAIN;
	}

	bool decoder_needs_more = s->decoder_open && s->input_len == 0;
	if (s->decoder && s->input_len > 0) {
		int channels = 0;
		int samples = 0;
		int used = stb_vorbis_decode_frame_pushdata(
		               s->decoder, s->input, (int)s->input_len, &channels,
		               &s->decoded, &samples);
		if (used < 0 || (size_t)used > s->input_len)
			return pump_end(false, tr(STR_ERR_DECODER_BLOCK));
		if (used > 0) {
			memmove(s->input, s->input + used, s->input_len - (size_t)used);
			s->input_len -= (size_t)used;
		}
		if (samples > 0) {
			if (channels != s->info.channels)
				return pump_end(false, tr(STR_ERR_CHANNELS_CHANGED));
			s->decoded_samples = samples;
			s->decoded_offset = 0;
			if (s->seek_sync)
				seek_settle(samples);
			return PUMP_AGAIN;
		}
		if (used > 0)
			return PUMP_AGAIN;
		decoder_needs_more = true;
	}

	available = track_stream_available(&dl.stream, s->source_pos);
	/* The end of the file, or a download that stopped before the bytes the
	 * decoder needs next. */
	bool at_end = dl.stream.len && s->source_pos >= dl.stream.len;
	bool stuck = dl.done && !at_end && available <= s->source_pos;
	bool no_more_data = (at_end || stuck) &&
	                    (!s->decoder_open || decoder_needs_more);
	if (no_more_data && !s->eof) {
		if (stuck || dl.stream.dl_failed) {
			snprintf(error, sizeof(error), tr(STR_ERR_DOWNLOAD_FAILED_FMT),
			         dl.stream.dl_err);
			return pump_end(false, error);
		}
		if (!s->decoder_open)
			return pump_end(false, tr(STR_ERR_NO_VORBIS));
		s->input_len = 0;
		s->eof = true;
	}

	if (s->input_len == PLAYER_INPUT_CAPACITY)
		return pump_end(false, tr(STR_ERR_VORBIS_TOO_LARGE));
	if (s->eof && s->pcm_frames == 0 && !s->final_wave_submitted)
		s->final_wave_submitted = true;
	if (s->eof && s->pcm_frames > 0 && !s->final_wave_submitted) {
		wave_index = free_wave_index(s->waves);
		if (wave_index >= 0) {
			submit_pcm(wave_index);
			s->final_wave_submitted = true;
			return PUMP_AGAIN;
		}
	}
	if (s->eof && s->pcm_frames == 0 && s->final_wave_submitted) {
		bool playing = false;
		for (int i = 0; i < PLAYER_WAVE_BUFFERS; i++)
			if (s->waves[i].status == NDSP_WBUF_QUEUED ||
			    s->waves[i].status == NDSP_WBUF_PLAYING)
				playing = true;
		if (!playing)
			return pump_end(s->total_audio_frames > 0, tr(STR_ERR_NO_AUDIO));
	}
	return PUMP_YIELD;
}

/* Once per frame from the main loop, on every tab: decodes until the
 * wave buffers are full or the download is behind, so the frame's wait for
 * VBlank only comes once the DSP has its cushion. */
static void player_pump(void)
{
	/* A skipped track's preparation or download that has finished: nobody
	 * wants it. */
	if (prep.thread && prep.abandoned && prep.done)
		prepare_reap();
	if (dl_abandoned && dl.done)
		download_reap();
	if (player.phase == PLAYER_PHASE_LOADING) {
		if (artwork_pending(&cover_art))
			return;
		if (!player.s.preparing) {
			player_start_prepare();
		} else if (!prep.abandoned) {
			player.stage = prep.stage;
			if (prep.done)
				player_begin_audio();
		}
		return;
	}
	if (player.phase == PLAYER_PHASE_FINISHED) {
		/* The next track follows on its own. At the end of the queue the
		 * player waits: a radio's next batch, once it comes, starts it. A
		 * failed track does not skip: without a connection that would
		 * run through the whole queue. */
		int next = queue_step(queue_pos, playback_queue_count, 1);
		if (next >= 0)
			player_play(player.session, next);
		return;
	}
	if (player.phase != PLAYER_PHASE_PLAYING)
		return;
	/* The download fills the holes after where the decoder reads first. */
	if (!dl_abandoned)
		track_stream_hint(&dl.stream, player.s.source_pos);
	for (int step = 0; step < PLAYER_PUMP_MAX_STEPS; step++)
		if (pump_once() != PUMP_AGAIN)
			break;
}

/* A or the middle button: pause and resume, or play the track again once
 * it has finished or failed. */
static void player_toggle(void)
{
	if (player.phase == PLAYER_PHASE_PLAYING) {
		player.paused = !player.paused;
		player_apply_pause();
	} else if (player.phase == PLAYER_PHASE_FINISHED ||
	           player.phase == PLAYER_PHASE_FAILED) {
		player_play(player.session, queue_pos);
	}
}

static void player_skip(int direction)
{
	int next = queue_step(queue_pos, playback_queue_count, direction);
	if (next >= 0)
		player_play(player.session, next);
}

/* The keyboard takes the main thread over: the DSP would run dry, so the
 * track pauses while it is open. */
static void player_hold(bool hold)
{
	player.held = hold;
	player_apply_pause();
}

/* Stops the track and forgets it: the session is going away. */
static void player_stop(void)
{
	player_release();
	prepare_wait();
	download_wait();
	player.phase = PLAYER_PHASE_IDLE;
	player.error[0] = '\0';
}

/* The track's network steps are running (token, access point, audio key):
 * the thumbnails wait (STYLES.md 9). During the audio download itself they
 * go on: a 64 px cover is a few KB next to megabytes of audio, and the
 * list would otherwise stay empty for the whole download. */
static bool player_preparing(void)
{
	return player.phase == PLAYER_PHASE_LOADING;
}

static unsigned player_elapsed(void)
{
	player_stream *s = &player.s;
	if (player.phase != PLAYER_PHASE_PLAYING || !s->decoder_open ||
	    !s->info.sample_rate)
		return 0;
	size_t played_frames = s->completed_audio_frames;
	if (!player.paused && !player.held) {
		u16 current_sequence = ndspChnGetWaveBufSeq(0);
		for (int i = 0; i < PLAYER_WAVE_BUFFERS; i++)
			if (s->wave_submitted[i] &&
			    s->waves[i].sequence_id == current_sequence) {
				u32 position = ndspChnGetSamplePos(0);
				if (position < s->waves[i].nsamples)
					played_frames += position;
				break;
			}
		s->displayed_played_frames = played_frames;
	} else {
		played_frames = s->displayed_played_frames;
	}
	return (unsigned)(played_frames / s->info.sample_rate);
}

static player_info player_snapshot(void)
{
	const sp_track *track = player_track();
	/* An abandoned download is the skipped track's, not this one's. */
	const download_job *job = dl_abandoned ? NULL : &dl;
	/* What is there from where the decoder reads: after a seek ahead, the
	 * bar shows how far the audio can go on, not the holes behind. */
	size_t ahead = job ? track_stream_available(&job->stream,
	                                            player.s.source_pos) : 0;
	unsigned percent = job && job->stream.total
	                   ? (unsigned)((unsigned long long)ahead * 100 /
	                                job->stream.total) : 0;
	if ((job && job->done && job->stream.dl_done) ||
	    player.phase == PLAYER_PHASE_FINISHED)
		percent = 100;
	unsigned duration = track && track->duration_ms > 0
	                    ? (unsigned)track->duration_ms / 1000 : 0;
	/* Where the track is in the downloads, for its icon and X. */
	dl_state dl_now = DL_QUEUED;
	bool dl_alone = false;
	int dl_pct = 0;
	bool dl_known = track && downloads_track_state(track->uri, &dl_now,
	                                               &dl_pct, &dl_alone);
	player_info info = {
		.phase = player.phase,
		.track = track,
		.serial = player.serial,
		.index = queue_pos,
		.count = playback_queue_count,
		.stage = player.stage,
		.paused = player.paused,
		.elapsed_s = player_elapsed(),
		.duration_s = duration,
		.download_pct = percent > 100 ? 100 : percent,
		.volume_pct = (unsigned)(playback_volume * 100.0f + 0.5f),
		.seekable = player.phase == PLAYER_PHASE_PLAYING &&
		            player.s.decoder_open && !dl_abandoned &&
		            dl.stream.dl_done &&
		            dl.stream.len > SPOTIFY_OGG_HEADER && duration > 0,
		.error = player.error,
		.saved = dl_track_button(dl_known, dl_now, dl_alone),
		.saved_pct = dl_pct,
		.offline = !net_online,
	};
	return info;
}

/* --- Screens ------------------------------------------------------------ */

/* Renders a GPU frame only when something changed, else just waits for
 * VBlank: rendering shares the main thread with the Vorbis decoder. While a
 * track plays, the clock changes once a second: the Playing tab's bottom
 * screen and the top screen's "now playing" card on the other tabs show
 * it, so that is one GPU frame per second on any tab (the big cover on the
 * Playing tab has no clock). With nothing playing, the top screen's logo
 * costs no frame at all (STYLES.md 9).
 *
 * Both screens are drawn together. Until the F0 spike confirms on hardware
 * that a target left out of a frame keeps its last image, skipping one is
 * not worth the risk; the top screen costs a few rectangles and quads.
 *
 * With the tabs, top.h draws the top screen (logo, card or big cover, and
 * the search bubble), the active tab's view draws the bottom one and the
 * tab bar goes over it; the artwork's own changes only count while the
 * cover is on screen, and switching tabs always repaints. Before the
 * session, view_login.h draws both screens. */
/* Called every frame: keeps the console awake while music is due, and
 * darkens the screens while the lid is shut over it. */
static void power_poll(void)
{
	if (!ptmu_ready || !lcd_ready)
		return;   /* without both, closing the lid just sleeps as before */
	bool awake = power_keep_awake(player.phase, player.paused || player.held,
	                              osIsHeadsetConnected());
	u8 shell = 1;
	if (R_FAILED(PTMU_GetShellState(&shell)))
		shell = 1;
	bool lit = power_screens_lit(shell != 0, awake);
	/* Before sleep is allowed again: that sleeps at once if the lid is
	 * shut, and NDM should take the connection back first. */
	wifi_hold(awake);
	if (lit && screens_dark) {
		set_backlights(true);
		screens_dark = false;
		screens_lost = true;
	} else if (!lit && !screens_dark) {
		set_backlights(false);
		screens_dark = true;
	}
	/* Allowing sleep again with the lid shut sleeps at once, which is why
	 * the screens were lit first. */
	if (aptIsSleepAllowed() == awake)
		aptSetSleepAllowed(!awake);
}

/* The detail on top of the active tab, if any, for the top screen; its
 * image is asked for when it changes. Opening a detail with no image (an
 * album still loading) drops the last one, so no other detail's picture
 * shows on its card. */
static const detail_doc *top_detail_sync(void)
{
	const detail_doc *doc = NULL;
	if (active_view == VIEW_TABS && details_active(active_tab))
		doc = details_top(active_tab);
	else if (active_view == VIEW_TABS && active_tab == TAB_HOME)
		/* The focused tile's card: a preview of what A opens. */
		doc = view_home_preview();
	if (doc && strcmp(doc->art, detail_art_url) != 0) {
		snprintf(detail_art_url, sizeof(detail_art_url), "%s", doc->art);
		artwork_request(&detail_art, detail_art_url);
		/* The list's thumbnail of it, scaled up, until it comes. */
		artwork_set_preview(&detail_art, doc->art_small);
	}
	return doc;
}

static void frame(void)
{
	/* 0: the login view; 1 + tab: that tab. */
	static int drawn_screen = -1;
	artwork_poll(&cover_art);
	artwork_poll(&detail_art);
	artwork_poll(&account_art);
	power_poll();
	if (screens_dark) {
		/* Nobody can see them: drawing would only cost battery. The
		 * repaint comes when the lid opens. */
		gspWaitForVBlank();
		return;
	}
	bool restored = ui_take_restored() || screens_lost;
	screens_lost = false;
	bool tabs = active_view == VIEW_TABS;
	int screen = tabs ? 1 + (int)active_tab : 0;
	bool changed;
	if (!tabs) {
		changed = view_login_dirty();
	} else {
		player_info info = player_snapshot();
		view_player_set(&info);
		top_set(&info);
		top_set_detail(top_detail_sync(), &detail_art);
		top_set_manager(manager_open);
		view_settings_set_waiting(downloads_waiting());
		changed = tabbar_dirty() || top_dirty(active_tab, &cover_art);
		if (details_active(active_tab))
			changed |= view_detail_dirty(details_top(active_tab));
		else switch (active_tab) {
		case TAB_HOME:
			changed |= view_home_dirty();
			break;
		case TAB_SEARCH:
			changed |= view_results_dirty();
			break;
		case TAB_PLAYER:
			changed |= view_player_dirty();
			break;
		default:
			changed |= manager_open ? view_downloads_dirty()
			                        : view_settings_dirty();
			break;
		}
	}
	if (!restored && screen == drawn_screen && !changed) {
		gspWaitForVBlank();
		return;
	}
	ui_begin();
	ui_scene(GFX_TOP);
	if (!tabs)
		view_login_draw_top();
	else
		top_draw(active_tab, &cover_art);
	ui_scene(GFX_BOTTOM);
	if (!tabs) {
		view_login_draw_bottom();
	} else if (details_active(active_tab)) {
		view_detail_draw_bottom(details_top(active_tab));
		tabbar_draw();
	} else {
		switch (active_tab) {
		case TAB_HOME:
			view_home_draw_bottom();
			break;
		case TAB_SEARCH:
			view_results_draw_bottom();
			break;
		case TAB_PLAYER:
			view_player_draw_bottom();
			break;
		default:
			if (manager_open)
				view_downloads_draw_bottom();
			else
				view_settings_draw_bottom();
			break;
		}
		tabbar_draw();
	}
	ui_end();
	drawn_screen = screen;
}

/* --- Search ------------------------------------------------------------ */

/* Pathfinder off until the app restarts: its hash could not be mended, or
 * it refused this client (403). Only the search thread touches it, and
 * one search runs at a time. A 429 does not count: that search alone goes
 * the old way. */
static bool pathfinder_off;

#ifdef APP_DEBUG
/* One line per search in search_trace.log, next to the config, for the
 * console checks: where the results came from, why not pathfinder, and
 * what each step took. Never the token, never the query. Debug builds
 * only; the search thread alone writes it, one search at a time. */
static struct {
	char line[256];
	size_t len;
} trace;

static void trace_add(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));
static void trace_add(const char *fmt, ...)
{
	if (trace.len >= sizeof(trace.line) - 1)
		return;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(trace.line + trace.len, sizeof(trace.line) - trace.len,
	                  fmt, ap);
	va_end(ap);
	if (n > 0)
		trace.len += (size_t)n;
	if (trace.len > sizeof(trace.line) - 1)
		trace.len = sizeof(trace.line) - 1;
}

static void trace_write(void)
{
	char path[320];
	cfg_dir(path, sizeof(path));
	size_t at = strlen(path);
	snprintf(path + at, sizeof(path) - at, "search_trace.log");
	FILE *f = fopen(path, "a");
	if (!f)
		return;
	fprintf(f, "%s\n", trace.line);
	fclose(f);
}
#define TRACE(...) trace_add(__VA_ARGS__)
#else
#define TRACE(...) ((void)0)
#endif

/* Names `n` track URIs into job->tracks with one metadata batch. Returns
 * how many, or -1. */
static int search_name_tracks(search_job *job, char uris[][64], int n,
                              char *error, size_t errlen)
{
	const char *uri_ptrs[SEARCH_LIMIT];
	if (n <= 0)
		return 0;
	for (int i = 0; i < n; i++)
		uri_ptrs[i] = uris[i];
	int named = sp_track_metadata_batch(job->host, job->library_token,
	                                    uri_ptrs, n, job->tracks, error,
	                                    errlen);
	secure_clear(uri_ptrs, sizeof(uri_ptrs));
	return named;
}

/* One sp_search_typed, traced. */
static int search_typed_once(search_job *job, sp_typed_search *typed,
                             char *error, size_t errlen)
{
	int rc = sp_search_typed(job->library_token, job->query,
	                         pf_hash_current(), typed,
	                         &job->cancel_requested, error, errlen);
	TRACE(" top=%d tracks=%d artists=%d albums=%d playlists=%d",
	      typed->ms[0], typed->ms[1 + RESULT_TRACKS],
	      typed->ms[1 + RESULT_ARTISTS], typed->ms[1 + RESULT_ALBUMS],
	      typed->ms[1 + RESULT_PLAYLISTS]);
	return rc;
}

/* Every kind through pathfinder, the tracks then named for their files.
 * Mends a stale hash once. Returns 0, PF_CANCELLED or -1. */
static int search_pathfinder(search_job *job, char uris[][64], char *error,
                             size_t errlen)
{
	sp_typed_search typed = {
		.items = { job->tracks, job->artists, job->albums, job->playlists },
		.max = { SEARCH_LIMIT, SEARCH_ITEMS_MAX, SEARCH_ITEMS_MAX,
		         SEARCH_ITEMS_MAX },
	};
	TRACE("pathfinder");
	int rc = search_typed_once(job, &typed, error, errlen);
	if (rc == PF_STALE_HASH && !job->cancel_requested) {
		u64 t0 = osGetTime();
		bool mended = pf_hash_refresh(error, errlen) == 0;
		TRACE(" hash=%s:%d", mended ? "mended" : "FAILED",
		      (int)(osGetTime() - t0));
		(void)t0;
		if (mended)
			rc = search_typed_once(job, &typed, error, errlen);
		/* No new hash, or the new one is stale too: not again this
		 * run. */
		if (rc == PF_STALE_HASH)
			pathfinder_off = true;
	}
	if (rc == PF_STALE_HASH)
		rc = -1;
	if (rc == -1) {
		if (typed.status)
			TRACE(" FAILED HTTP %d", typed.status);
		else
			TRACE(" FAILED %s", error);
		if (typed.status == 403)
			pathfinder_off = true;
	}
	if (rc != 0)
		return rc;

	/* The tracks come by URI only: the batch names them and brings the
	 * files to play. */
	int n = typed.count[RESULT_TRACKS];
	for (int i = 0; i < n; i++)
		snprintf(uris[i], 64, "%s", job->tracks[i].uri);
	u64 t0 = osGetTime();
	job->count = search_name_tracks(job, uris, n, error, errlen);
	TRACE(" meta=%d", (int)(osGetTime() - t0));
	(void)t0;
	if (job->cancel_requested)
		return PF_CANCELLED;
	if (job->count < 0)
		return -1;

	job->kind_count[RESULT_TRACKS] = job->count;
	for (int k = RESULT_ARTISTS; k < RESULT_KINDS; k++)
		job->kind_count[k] = typed.count[k];
	for (int k = 0; k < RESULT_KINDS; k++)
		job->offered[k] = true;
	job->best_kind = -1;
	int best = typed.best_kind;
	if (best >= 0 && best < RESULT_KINDS && job->kind_count[best] > 0) {
		int at = search_find_uri(typed.items[best], job->kind_count[best],
		                         typed.best_uri);
		/* Not among the rows kept: the list is still ours, so its first
		 * one. */
		job->best_kind = best;
		job->best_index = at >= 0 ? at : 0;
	}
	return 0;
}

/* Today's search: tracks from context-resolve, artists and albums made up
 * from them, no playlists. */
static int search_fallback(search_job *job, char uris[][64], char *error,
                           size_t errlen)
{
	int n = sp_search_tracks(job->host, job->library_token, job->query,
	                         uris, SEARCH_LIMIT, error, errlen);
	if (job->cancel_requested)
		return PF_CANCELLED;
	if (n < 0)
		return -1;
	job->count = search_name_tracks(job, uris, n, error, errlen);
	if (job->cancel_requested)
		return PF_CANCELLED;
	if (job->count < 0)
		return -1;
	job->kind_count[RESULT_TRACKS] = job->count;
	job->kind_count[RESULT_ARTISTS] =
	    search_derive_artists(job->tracks, job->count, job->artists,
	                          SEARCH_ITEMS_MAX);
	job->kind_count[RESULT_ALBUMS] =
	    search_derive_albums(job->tracks, job->count, job->albums,
	                         SEARCH_ITEMS_MAX);
	job->kind_count[RESULT_PLAYLISTS] = 0;
	job->offered[RESULT_TRACKS] = true;
	job->offered[RESULT_ARTISTS] = true;
	job->offered[RESULT_ALBUMS] = true;
	job->offered[RESULT_PLAYLISTS] = false;
	job->best_kind = -1;
	return 0;
}

static void search_worker(void *arg)
{
	search_job *job = arg;
	char uris[SEARCH_LIMIT][64];
	char error[sizeof(job->error)] = {0};
	int rc = -1;
	bool use_pathfinder = !pathfinder_off;
#ifdef APP_DEBUG
	trace.len = 0;
	trace.line[0] = '\0';
	/* To try the fallback on the console without breaking anything. */
	char forced[8];
	if (cfg_get("pf_force_fallback", forced, sizeof(forced)) &&
	    strcmp(forced, "1") == 0)
		use_pathfinder = false;
#endif
	u64 start = osGetTime();

	if (use_pathfinder)
		rc = search_pathfinder(job, uris, error, sizeof(error));
	else
		TRACE("pathfinder %s", pathfinder_off ? "off" : "forced off");

	/* Anything but a cancel goes the old way, without a word: a search
	 * fails only when that fails too. */
	if (rc != 0 && rc != PF_CANCELLED && !job->cancel_requested) {
		memset(job->kind_count, 0, sizeof(job->kind_count));
		job->count = 0;
		error[0] = '\0';
		u64 t0 = osGetTime();
		rc = search_fallback(job, uris, error, sizeof(error));
		TRACE(" | fallback=%d%s", (int)(osGetTime() - t0),
		      rc == 0 ? "" : " FAILED");
		(void)t0;
	}
	TRACE(" | total=%d", (int)(osGetTime() - start));
	(void)start;

	if (job->cancel_requested || rc == PF_CANCELLED) {
		job->result = -2;
	} else if (rc != 0) {
		snprintf(job->error, sizeof(job->error), "%s",
		         error[0] ? error : tr(STR_ERR_SEARCH_LOAD));
		job->result = -1;
	} else {
		job->result = 0;
	}
#ifdef APP_DEBUG
	if (job->result != -2)
		trace_write();
#endif
	secure_clear(uris, sizeof(uris));
	secure_clear(error, sizeof(error));
	__sync_synchronize();
	job->done = true;
}

/* What to suggest for a failed search, from the HTTP status in `error`. */
static str_id search_error_advice(const char *error)
{
	if (strstr(error, "HTTP 401"))
		return STR_SEARCH_HTTP_401;
	if (strstr(error, "HTTP 403"))
		return STR_SEARCH_HTTP_403;
	if (strstr(error, "HTTP 429"))
		return STR_SEARCH_HTTP_429;
	return STR_SEARCH_CHECK_WIFI;
}

static void search_join(void)
{
	threadJoin(search_thread, U64_MAX);
	threadFree(search_thread);
	search_thread = NULL;
	secure_clear(search.library_token, sizeof(search.library_token));
	secure_clear(search.host, sizeof(search.host));
}

/* Cancels a running search and waits for its request in flight. The
 * wait blocks the decoder, so the track pauses meanwhile instead of
 * running dry. */
static void search_stop(void)
{
	if (!search_thread)
		return;
	search.cancel_requested = true;
	view_results_cancelling();
	bool held = player.held;
	player_hold(true);
	search_join();
	player_hold(held);
	view_results_idle();
}

/* Starts searching `query` in the background: the Search tab shows it
 * running, and the main loop, the audio included, goes on meanwhile. */
static void search_start(const spotify_session *session, const char *query)
{
	search_stop();
	memset(&search, 0, sizeof(search));
	snprintf(search.query, sizeof(search.query), "%s", query);
	recent_remember(search.query);
	snprintf(search.host, sizeof(search.host), "%s", session->host);
	snprintf(search.library_token, sizeof(search.library_token), "%s",
	         session->library_token);
	/* Its details came from the results being replaced. */
	details_clear(TAB_SEARCH);
	view_results_searching();
	/* Below the decoder, like every worker that can overlap playback. */
	search_thread = threadCreate(search_worker, &search, 16 * 1024,
	                             background_priority(), -2, false);
	if (!search_thread) {
		snprintf(search.error, sizeof(search.error), "%s",
		         tr(STR_ERR_SEARCH_THREAD));
		secure_clear(search.library_token, sizeof(search.library_token));
		view_results_error(search_error_advice(search.error));
	}
}

/* Once per frame: shows a finished search. */
static void search_poll(void)
{
	if (!search_thread || !search.done)
		return;
	__sync_synchronize();
	search_join();
	if (search.cancel_requested) {
		view_results_idle();
		/* Cancelled from its tab: back home, as before the tabs. */
		if (active_tab == TAB_SEARCH)
			switch_tab(TAB_HOME);
	} else if (!net_online) {
		/* The connection went while it searched. */
		view_results_error(STR_OFFLINE_SEARCH);
	} else if (search.result == 0) {
		view_results_show(search.tracks, search.count, 0);
	} else {
		view_results_error(search_error_advice(search.error));
	}
}

/* Only a confirmed query replaces `query`: cancelling the keyboard keeps the
 * last search for the home view's repeat chip. */
static bool ask_search_query(char query[SEARCH_QUERY_SIZE])
{
	char typed[SEARCH_QUERY_SIZE] = {0};
	SwkbdState keyboard;
	swkbdInit(&keyboard, SWKBD_TYPE_WESTERN, 1, SEARCH_QUERY_SIZE - 1);
	swkbdSetValidation(&keyboard, SWKBD_NOTEMPTY_NOTBLANK,
	                   SWKBD_FILTER_PROFANITY, -1);
	swkbdSetHintText(&keyboard, tr(STR_SEARCH_HINT));
	/* The applet blocks this thread, and with it the decoder. */
	player_hold(true);
	bool confirmed = swkbdInputText(&keyboard, typed, sizeof(typed)) ==
	                 SWKBD_BUTTON_CONFIRM;
	player_hold(false);
	if (confirmed)
		memcpy(query, typed, sizeof(typed));
	/* The keyboard applet takes over both screens. */
	artwork_invalidate(&cover_art);
	artwork_invalidate(&detail_art);
	screens_lost = true;
	return confirmed;
}

/* The keyboard, then the search on its tab. */
static void keyboard_search(const spotify_session *session,
                            char query[SEARCH_QUERY_SIZE])
{
	if (!ask_search_query(query) || !query[0])
		return;
	search_start(session, query);
	switch_tab(TAB_SEARCH);
}

/* Makes `count` tracks the queue, a copy, and plays track `index` of it.
 * A list's queue follows the shuffle setting; a radio's does not. A load
 * for the queue before is not wanted any more. */
static void queue_play(spotify_session *session, const sp_track *tracks,
                       int count, int index, queue_kind source)
{
	if (count > PLAYBACK_QUEUE_MAX)
		count = PLAYBACK_QUEUE_MAX;
	if (index < 0 || index >= count)
		return;
	memcpy(playback_queue, tracks, (size_t)count * sizeof(playback_queue[0]));
	playback_queue_count = count;
	queue_source = source;
	queue_seed_back = 0;
	queue_serial++;
	queue_retry_at = 0;
	queue_feed_cancel();
	queue_rng ^= (uint32_t)svcGetSystemTick();
	bool shuffled = source != QUEUE_RADIO && shuffle_on;
	/* A new queue: offline, the search for a downloaded track goes on
	 * forwards from the one chosen. */
	queue_pos = -1;
	player_play(session, queue_order_start(playback_order, count, index,
	                                       shuffled, &queue_rng));
}

/* A single track, and a radio from it to follow. */
static void queue_play_radio(spotify_session *session, const sp_track *track)
{
	queue_play(session, track, 1, 0, QUEUE_RADIO);
}

/* A list's tracks from its focused one; a playlist with pages left goes
 * on loading them into the queue (queue_feed.h). */
static void queue_play_list(spotify_session *session, const detail_doc *doc)
{
	bool pages = doc->kind == DETAIL_PLAYLIST && doc->more;
	queue_play(session, doc->tracks, doc->count, doc->selected,
	           pages ? QUEUE_PLAYLIST : QUEUE_LIST);
	if (pages) {
		snprintf(queue_playlist, sizeof(queue_playlist), "%s", doc->uri);
		queue_next_from = doc->next_from;
	}
}

/* A list from the downloads: its tracks on the card, from the focused
 * one; the collections and what is not downloaded yet stay out. */
static void queue_play_offline(spotify_session *session, const detail_doc *doc)
{
	sp_track *list = malloc((size_t)doc->count * sizeof(sp_track));
	if (!list)
		return;
	int n = 0, at = 0;
	for (int i = 0; i < doc->count; i++) {
		const track_row *r = &doc->rows[i];
		if (r->kind != TRACK_ROW_TRACK || r->off)
			continue;
		if (i == doc->selected)
			at = n;
		list[n++] = doc->tracks[i];
	}
	if (n > 0)
		queue_play(session, list, n, at, QUEUE_LIST);
	free(list);
}

/* What a load brought, onto the end of the queue: in a shuffled list,
 * among the tracks still to play. */
static void queue_take(const queue_feed_result *r)
{
	int n = r->count;
	if (queue_source == QUEUE_RADIO)
		n = queue_drop_known(playback_queue, playback_queue_count, r->tracks,
		                     n);
	int room = PLAYBACK_QUEUE_MAX - playback_queue_count;
	if (n > room)
		n = room;
	if (n > 0)
		memcpy(playback_queue + playback_queue_count, r->tracks,
		       (size_t)n * sizeof(playback_queue[0]));
	bool shuffled = queue_source != QUEUE_RADIO && shuffle_on;
	playback_queue_count = queue_order_append(playback_order,
	                                          playback_queue_count, n,
	                                          queue_pos, shuffled, &queue_rng);
	if (queue_source == QUEUE_PLAYLIST) {
		/* An empty page means the end, whatever the total said. */
		queue_next_from += r->items;
		if (r->items <= 0 || queue_next_from >= r->total)
			queue_source = QUEUE_LIST;
	} else if (n == 0) {
		/* All known already: the next batch from an earlier seed, and not
		 * at once. */
		queue_seed_back++;
		queue_retry_at = osGetTime() + QUEUE_RETRY_MS;
	} else {
		queue_seed_back = 0;
	}
}

/* Once per frame: takes what a load brought, and starts the next one the
 * queue needs. Never while a track prepares (STYLES.md 9): a radio's first
 * batch comes once its seed is playing. */
static void queue_feed_tick(const spotify_session *session)
{
	queue_feed_result r;
	if (queue_feed_poll(&r) && r.tag == queue_serial) {
		if (r.count < 0)
			queue_retry_at = osGetTime() + QUEUE_RETRY_MS;
		else
			queue_take(&r);
	}
	if (!queue_feed_idle() || player.phase == PLAYER_PHASE_IDLE ||
	    player_preparing() || osGetTime() < queue_retry_at ||
	    playback_queue_count >= PLAYBACK_QUEUE_MAX)
		return;
	bool started = true;
	if (queue_source == QUEUE_PLAYLIST) {
		started = queue_feed_playlist(session->host, session->library_token,
		                              session->username, queue_playlist,
		                              queue_next_from, queue_serial);
	} else if (queue_source == QUEUE_RADIO &&
	           queue_wants_more(queue_pos, playback_queue_count)) {
		int seed = playback_queue_count - 1 - queue_seed_back;
		if (seed < 0)
			seed = 0;
		started = queue_feed_radio(session->host, session->library_token,
		                           playback_queue[seed].uri, queue_serial);
	}
	if (!started)
		queue_retry_at = osGetTime() + QUEUE_RETRY_MS;
}

/* Y in a list: the setting switches and is kept. A list's queue playing
 * now follows at once, from the track playing, which goes on. */
static void toggle_shuffle(void)
{
	shuffle_on = !shuffle_on;
	cfg_set_async("shuffle", shuffle_on ? "1" : "0");
	view_detail_set_shuffle(shuffle_on);
	if (queue_source == QUEUE_RADIO || player.phase == PLAYER_PHASE_IDLE)
		return;
	queue_rng ^= (uint32_t)svcGetSystemTick();
	queue_pos = queue_order_reshuffle(playback_order, playback_queue_count,
	                                  queue_pos, shuffle_on, &queue_rng);
}

/* Plays result `index`, straight from the list: there is no selection
 * screen in between. A result stands alone, so a radio from it follows;
 * the other results are only what else the search found. */
static void play_from_results(spotify_session *session, int index)
{
	if (index < 0 || index >= search.count)
		return;
	/* The thumbnails must not compete with the track's preparation
	 * (STYLES.md 9); they resume, kept, once its download starts. */
	view_results_allow_thumbs(false);
	queue_play_radio(session, &search.tracks[index]);
	switch_tab(TAB_PLAYER);
}

/* --- Details ----------------------------------------------------------- */

/* Plays the detail on top of `tab` from its focused track: with the whole
 * list it holds as the queue (an album, an artist, a playlist, or the list
 * a track's detail was opened from), or, for a track that stands alone, a
 * radio from it. The queue is a copy, whatever the document does
 * afterwards. */
static void play_from_detail(spotify_session *session, app_tab tab)
{
	const detail_doc *doc = details_top(tab);
	if (!doc || doc->count <= 0)
		return;
	const sp_track *pick = &doc->tracks[doc->selected];
	/* The track already playing: show it rather than start it again. */
	const sp_track *now = player_track();
	bool same = now && strcmp(now->uri, pick->uri) == 0 &&
	            (player.phase == PLAYER_PHASE_LOADING ||
	             player.phase == PLAYER_PHASE_PLAYING);
	if (!same) {
		/* As from the results: the thumbnails wait for the preparation
		 * (STYLES.md 9). */
		view_results_allow_thumbs(false);
		if (doc->kind == DETAIL_TRACK && doc->single)
			queue_play_radio(session, pick);
		else if (doc->rows)
			queue_play_offline(session, doc);
		else
			queue_play_list(session, doc);
	}
	/* The Playing tab shows the player, not a detail left open there; the
	 * queue is copied, so this may be the document just played. */
	details_clear(TAB_PLAYER);
	switch_tab(TAB_PLAYER);
}

/* The detail of the focused result, on the Search tab's stack. */
static void open_result_detail(void)
{
	details_open_track(TAB_SEARCH, search.tracks, search.count,
	                   view_results_selected(), true);
	switch_tab(TAB_SEARCH);
}

/* A link of the Playing tab: the detail of the track playing, of its
 * artist or of its album, on that tab's stack. The track detail gets a
 * copy of the queue, which may change afterwards, so "Play" there plays
 * this queue again from this track; from a radio, a new radio from it. */
static void open_playing_detail(detail_kind kind)
{
	const sp_track *track = player_track();
	if (!track)
		return;
	switch (kind) {
	case DETAIL_ARTIST:
		details_open_artist(TAB_PLAYER, track->artist_uri);
		break;
	case DETAIL_ALBUM:
		details_open_album(TAB_PLAYER, track->album_uri);
		break;
	default:
		details_open_track(TAB_PLAYER, playback_queue, playback_queue_count,
		                   player.index, queue_source == QUEUE_RADIO);
		break;
	}
	switch_tab(TAB_PLAYER);
}

/* `t` kept on the SD card (or a failed download of it tried again). When
 * it is the track that plays, the player holds its key and its file: they
 * go with it, so the download asks the access point for nothing. Returns
 * what to tell the user. */
static str_id download_track(const sp_track *t)
{
	if (!t)
		return STR_DL_NOT_READY;
	const sp_track *playing = player_track();
	const track_stream *ts = &dl.stream;
	bool keyed = playing && strcmp(playing->uri, t->uri) == 0 &&
	             player.phase == PLAYER_PHASE_PLAYING && !dl_abandoned &&
	             strcmp(ts->meta.uri, t->uri) == 0;
	return keyed ? downloads_add_track_keyed(t, ts->key, ts->file_id,
	                                         ts->format)
	             : downloads_add_track(t);
}

static bool good_news(str_id said)
{
	return said == STR_DL_ADDED || said == STR_DL_ALREADY ||
	       said == STR_DL_REMOVED;
}

/* X on the Playing tab: the track that plays, kept on the SD card. */
static void download_playing(void)
{
	const sp_track *t = player_track();
	if (!t)
		return;
	str_id said = download_track(t);
	view_player_notice(said, good_news(said));
}

/* The track detail's download button, as it is now: download (or retry),
 * or remove. Queued and downloading tracks have it disabled. */
static void download_from_detail(const sp_track *t)
{
	if (!t)
		return;
	dl_state state = DL_QUEUED;
	bool alone = false;
	bool known = downloads_track_state(t->uri, &state, NULL, &alone);
	str_id said;
	switch (dl_track_button(known, state, alone)) {
	case DL_BUTTON_DOWNLOAD:
	case DL_BUTTON_RETRY:
		said = download_track(t);
		break;
	case DL_BUTTON_REMOVE:
		downloads_remove(t->uri);
		said = STR_DL_REMOVED;
		break;
	default:
		return;
	}
	view_detail_notice(said, good_news(said));
}

/* Whether an album or a playlist is on the card whole. */
static bool downloaded_whole(const char *uri)
{
	bool pending;
	int done, count;
	return downloads_collection_state(uri, &pending, &done, &count) &&
	       dl_collection_button(pending, true, done, count) ==
	       DL_COLL_BUTTON_DONE;
}

/* An album's or playlist's button over its list. */
static void download_collection(const detail_doc *doc)
{
	str_id said = downloads_add_collection(
		doc->kind == DETAIL_ALBUM ? DL_COLL_ALBUM : DL_COLL_PLAYLIST,
		doc->uri, doc->title, doc->art);
	view_detail_notice(said, good_news(said));
}

/* A detail is on top of the active tab: its view has the input. */
static void detail_input(spotify_session *session, u32 down)
{
	detail_doc *doc = details_top(active_tab);
	detail_action action = view_detail_update(doc, down);
	/* A playlist near the end of what is loaded asks for its next page. */
	details_follow(active_tab);
	/* The track the action is about: the one shown, or the focused row,
	 * as the view has just written it back. Opening a level copies what
	 * it needs from this one, which stays on the stack under it. */
	const sp_track *track = doc->count > 0 ? &doc->tracks[doc->selected]
	                                       : NULL;
	switch (action) {
	case DETAIL_ACTION_PLAY:
		play_from_detail(session, active_tab);
		break;
	case DETAIL_ACTION_ARTIST:
		if (track)
			details_open_artist(active_tab, track->artist_uri);
		switch_tab(active_tab);
		break;
	case DETAIL_ACTION_ALBUM:
		if (track)
			details_open_album(active_tab, track->album_uri);
		switch_tab(active_tab);
		break;
	case DETAIL_ACTION_OPEN_TRACK:
		/* The list comes along, as from the results: "Play" there queues
		 * this album or artist. From the Downloads, only its tracks: the
		 * collections before them are no tracks to play. */
		if (doc->kind == DETAIL_DOWNLOADS)
			details_open_track(active_tab, doc->tracks + doc->coll_rows,
			                   doc->track_rows,
			                   doc->selected - doc->coll_rows, false);
		else
			details_open_track(active_tab, doc->tracks, doc->count,
			                   doc->selected, false);
		switch_tab(active_tab);
		break;
	case DETAIL_ACTION_OPEN_COLLECTION:
		if (track)
			details_open_offline_collection(active_tab, track->uri);
		switch_tab(active_tab);
		break;
	case DETAIL_ACTION_REMOVE_ALL: {
		char uri[SP_URI_MAX];
		snprintf(uri, sizeof(uri), "%s", doc->uri);
		/* The level shows nothing any more: back to where it came from. */
		details_back(active_tab);
		downloads_remove(uri);
		switch_tab(active_tab);
		if (details_active(active_tab))
			view_detail_notice(STR_DL_REMOVED, true);
		break;
	}
	case DETAIL_ACTION_RETRY:
		details_retry(active_tab);
		break;
	case DETAIL_ACTION_BACK:
		details_back(active_tab);
		/* The level below, or the tab's own view, starts afresh. */
		switch_tab(active_tab);
		break;
	case DETAIL_ACTION_LANGUAGE:
		switch_language();
		break;
	case DETAIL_ACTION_SHUFFLE:
		toggle_shuffle();
		break;
	case DETAIL_ACTION_DOWNLOAD:
		download_from_detail(track);
		break;
	case DETAIL_ACTION_DOWNLOAD_ALL:
		if (doc->kind != DETAIL_ALBUM && doc->kind != DETAIL_PLAYLIST)
			break;
		if (downloaded_whole(doc->uri)) {
			/* "Downloaded": the copy on the card, with no network. */
			details_open_offline_collection(active_tab, doc->uri);
			switch_tab(active_tab);
		} else {
			download_collection(doc);
		}
		break;
	default:
		break;
	}
}

/* --- Tabs -------------------------------------------------------------- */

/* Shows `tab`: its view starts with no press in progress. */
static void switch_tab(app_tab tab)
{
	active_tab = tab;
	tabbar_set_active(tab);
	/* A detail left open on the tab is still there (plan 3.1). */
	if (details_active(tab)) {
		view_detail_enter();
		return;
	}
	switch (tab) {
	case TAB_HOME:
		view_home_enter();
		break;
	case TAB_SEARCH:
		view_results_enter();
		break;
	case TAB_PLAYER:
		view_player_enter();
		break;
	default:
		if (manager_open)
			view_downloads_enter();
		else
			view_settings_enter();
		break;
	}
}

/* A row of the downloads manager opened in the Home tab: a downloaded
 * track's detail, or its album's or playlist's. */
static void open_from_manager(dlv_action action)
{
	if (action == DLV_ACTION_OPEN_TRACK) {
		static sp_track track;
		if (view_downloads_target_track(&track))
			details_open_track(TAB_HOME, &track, 1, 0, true);
	} else {
		char uri[SP_URI_MAX];
		dl_coll_kind kind;
		if (!view_downloads_target_collection(uri, sizeof(uri), &kind))
			return;
		/* The copy on the card: no network needed. */
		details_open_offline_collection(TAB_HOME, uri);
	}
	switch_tab(TAB_HOME);
}

/* Reconnecting from the tabs without leaving them (defined with the
 * session start-up). */
typedef enum {
	RECONNECT_NONE,      /* nothing new */
	RECONNECT_ONLINE,    /* connected: the session is up, music went on */
	RECONNECT_OFFLINE,   /* still no network */
	RECONNECT_RESTART    /* the account said no: the full start-up */
} reconnect_result;
static void reconnect_start(void);
static reconnect_result reconnect_poll(spotify_session *session);
static bool reconnect_running(void);
static void reconnect_stop(void);
static bool wifi_down(void);

/* Where the tabs go when they end. */
typedef enum {
	TABS_EXIT,       /* close the app */
	TABS_RECONNECT,  /* start the session again */
	TABS_PAIR        /* pair another account */
} tabs_exit;

/* A bubble on whichever view shows: what a reconnection came to. */
static void tabs_notice(str_id text, bool ok)
{
	if (details_active(active_tab)) {
		view_detail_notice(text, ok);
		return;
	}
	switch (active_tab) {
	case TAB_HOME:
		view_home_notice(text);
		break;
	case TAB_PLAYER:
		view_player_notice(text, ok);
		break;
	default:
		break;
	}
}

/* "Retry connection" offline: with no Wi-Fi it says so at once; with Wi-Fi
 * the start-up runs in the background while the downloads go on playing. */
static void try_reconnect(void)
{
	if (reconnect_running())
		return;
	if (!net_ready || wifi_down()) {
		tabs_notice(STR_OFFLINE_STILL, false);
		return;
	}
	reconnect_start();
	tabs_notice(STR_OFFLINE_CONNECTING, true);
}

/* Connected from the tabs: what an online start does, without stopping
 * what plays. */
static void go_online(spotify_session *session)
{
	net_online = true;
	tabbar_set_offline(false);
	view_home_set_offline(false);
	view_detail_set_offline(false);
	cfg_set_async("username", session->username);
	account_start(session);
	/* A feed that failed when the connection went is fetched again; one
	 * that came in before is kept. */
	home_feed_start(session->host, session->library_token, session->username);
	home_feed_retry(session->host, session->library_token, session->username);
	view_results_idle();
	downloads_start(session->host, session->library_token, session->username,
	                downloads_open_ap);
	tabs_notice(STR_ONLINE_AGAIN, true);
}

/* The connection went while the tabs ran online: they look and behave as
 * after an offline start, and what plays from the card goes on. */
static void go_offline(spotify_session *session)
{
	LightLock_Lock(&session_lock);
	session->online = false;
	LightLock_Unlock(&session_lock);
	net_online = false;
	tabbar_set_offline(true);
	view_home_set_offline(true);
	view_detail_set_offline(true);
	/* A search in flight shows the same advice when it ends. */
	if (!search_thread)
		view_results_error(STR_OFFLINE_SEARCH);
	/* The track it was downloading goes back to the queue, to wait. */
	downloads_stop();
	downloads_start(session->host, session->library_token, session->username,
	                NULL);
	tabs_notice(STR_OFFLINE_LOST, false);
}

/* Online, Wi-Fi is asked about once a second (ACU answers at once). */
#define WIFI_CHECK_FRAMES 60

static bool connection_lost(void)
{
	static int frames;
	if (!net_online || ++frames < WIFI_CHECK_FRAMES)
		return false;
	frames = 0;
	return wifi_down();
}

/* The app once the session is up: one loop for every tab, so the audio
 * keeps playing while the user moves between them. */
static tabs_exit run_tabs(spotify_session *session, bool still_offline)
{
	active_view = VIEW_TABS;
	manager_open = false;
	net_online = session->online;
	tabbar_set_offline(!net_online);
	view_home_set_offline(!net_online);
	view_detail_set_offline(!net_online);
	/* The service's text may be in any script from here on; the fonts
	 * have had the whole sign-in to load. */
	ui_font_wait();
	account_start(session);
	tabbar_reset(TAB_HOME);
	/* In the background: the home tab shows its skeleton meanwhile.
	 * Offline it shows the downloads, and searching says it cannot. */
	if (net_online) {
		home_feed_start(session->host, session->library_token,
		                session->username);
		view_results_idle();
	} else {
		view_results_error(STR_OFFLINE_SEARCH);
	}
	downloads_session = session;
	/* Offline the engine gets no host: it keeps the index for playing and
	 * downloads nothing. */
	downloads_start(session->host, session->library_token, session->username,
	                net_online ? downloads_open_ap : NULL);
	switch_tab(TAB_HOME);
	if (still_offline)
		view_home_notice(STR_OFFLINE_STILL);
	for (;;) {
		hidScanInput();
		u32 down = keys_down();
		u32 held = hidKeysHeld();
		u32 up = hidKeysUp();
		if (down & KEY_START)
			return TABS_EXIT;

		/* The audio first: the DSP's cushion is ~370 ms. */
		player_pump();
		/* Radios and playlist pages come from the network. */
		if (net_online)
			queue_feed_tick(session);
		/* Below the player: quiet while it prepares a track or downloads
		 * the one that plays. */
		downloads_poll(dl_player_busy(player_preparing(), dl.thread != NULL,
		                              dl.done));
		account_poll();
		if (connection_lost())
			go_offline(session);
		switch (reconnect_poll(session)) {
		case RECONNECT_ONLINE:
			go_online(session);
			break;
		case RECONNECT_OFFLINE:
			tabs_notice(STR_OFFLINE_STILL, false);
			break;
		case RECONNECT_RESTART:
			return TABS_RECONNECT;
		default:
			break;
		}
		search_poll();
		details_poll(active_tab, session->host, session->library_token,
		             session->username);
		home_feed_poll();
		/* Not stopped on leaving the tab: it only fetches the rows on
		 * screen and the next ones anyway. */
		view_results_allow_thumbs(!player_preparing());

		int next = tabbar_update(down, held, up);
		if (next >= 0) {
			/* The button that switched tabs is spent. */
			down &= ~(u32)(KEY_L | KEY_R | KEY_ZL | KEY_ZR);
			if (next != (int)active_tab)
				switch_tab((app_tab)next);
		}

		if (details_active(active_tab)) {
			detail_input(session, down);
		} else switch (active_tab) {
		case TAB_HOME:
			switch (view_home_update(down)) {
			case HOME_ACTION_SEARCH:
				/* Offline the Search tab says why it cannot. */
				if (net_online)
					keyboard_search(session, home_query);
				else
					switch_tab(TAB_SEARCH);
				break;
			case HOME_ACTION_RECONNECT:
				try_reconnect();
				break;
			case HOME_ACTION_OPEN:
				/* On Home's own stack: L/R and back find it as left. The
				 * Downloads tile opens the card's, with no network. */
				if (strcmp(view_home_selected_uri(), DL_HOME_URI) == 0)
					details_open_downloads(TAB_HOME);
				else
					details_open_playlist(TAB_HOME,
					                      view_home_selected_uri());
				switch_tab(TAB_HOME);
				break;
			case HOME_ACTION_RETRY:
				home_feed_retry(session->host, session->library_token,
				                session->username);
				break;
			case HOME_ACTION_LANGUAGE:
				/* The view shows the new language by itself. */
				switch_language();
				break;
			default:
				break;
			}
			break;
		case TAB_SEARCH: {
			/* Feedback only while searching: cancelling is decided
			 * here. */
			results_action action = view_results_update(down);
			if (search_thread && !search.cancel_requested &&
			    (down & KEY_B)) {
				search.cancel_requested = true;
				view_results_cancelling();
			}
			switch (action) {
			case RESULTS_ACTION_OPEN:
				play_from_results(session, view_results_selected());
				break;
			case RESULTS_ACTION_DETAIL:
				open_result_detail();
				break;
			case RESULTS_ACTION_BACK:
				switch_tab(TAB_HOME);
				break;
			case RESULTS_ACTION_CLOSE:
				/* Back to before the results: the button and the
				 * recent searches. */
				view_results_idle();
				break;
			case RESULTS_ACTION_RECENT:
				snprintf(home_query, sizeof(home_query), "%s",
				         view_results_recent_query());
				if (home_query[0] && net_online)
					search_start(session, home_query);
				break;
			case RESULTS_ACTION_NEW_SEARCH:
				if (net_online)
					keyboard_search(session, home_query);
				break;
			case RESULTS_ACTION_RETRY:
				/* Offline, Retry is retrying the connection. */
				if (!net_online) {
					try_reconnect();
					break;
				}
				search_start(session, home_query);
				break;
			case RESULTS_ACTION_LANGUAGE:
				switch_language();
				break;
			default:
				break;
			}
			break;
		}
		case TAB_PLAYER:
			switch (view_player_update(down)) {
			case PLAYER_ACTION_TOGGLE:
				player_toggle();
				break;
			case PLAYER_ACTION_PREV:
				player_skip(-1);
				break;
			case PLAYER_ACTION_NEXT:
				player_skip(1);
				break;
			case PLAYER_ACTION_VOLUME_UP:
				set_playback_volume(playback_volume + 0.1f);
				break;
			case PLAYER_ACTION_VOLUME_DOWN:
				set_playback_volume(playback_volume - 0.1f);
				break;
			case PLAYER_ACTION_SEEK:
				player_seek(view_player_value());
				break;
			case PLAYER_ACTION_VOLUME_SET:
				set_playback_volume(view_player_value() / 1000.0f);
				break;
			case PLAYER_ACTION_BACK:
				switch_tab(TAB_SEARCH);
				break;
			case PLAYER_ACTION_LANGUAGE:
				switch_language();
				break;
			case PLAYER_ACTION_OPEN_TRACK:
				open_playing_detail(DETAIL_TRACK);
				break;
			case PLAYER_ACTION_OPEN_ARTIST:
				open_playing_detail(DETAIL_ARTIST);
				break;
			case PLAYER_ACTION_OPEN_ALBUM:
				open_playing_detail(DETAIL_ALBUM);
				break;
			case PLAYER_ACTION_DOWNLOAD:
				download_playing();
				break;
			default:
				break;
			}
			break;
		default:
			if (manager_open) {
				dlv_action action = view_downloads_update(down);
				switch (action) {
				case DLV_ACTION_BACK:
					manager_open = false;
					view_settings_enter();
					view_settings_focus(SETTINGS_ITEM_DOWNLOADS);
					break;
				case DLV_ACTION_LANGUAGE:
					switch_language();
					break;
				case DLV_ACTION_OPEN_TRACK:
				case DLV_ACTION_OPEN_COLLECTION:
					open_from_manager(action);
					break;
				default:
					break;
				}
				break;
			}
			switch (view_settings_update(down)) {
			case SETTINGS_ACTION_LANGUAGE:
				/* The language button shows the new one by itself. */
				switch_language();
				break;
			case SETTINGS_ACTION_DOWNLOADS:
				manager_open = true;
				view_downloads_enter();
				break;
			case SETTINGS_ACTION_RECONNECT:
				/* Offline it connects without leaving the tabs. */
				if (!net_online) {
					try_reconnect();
					break;
				}
				return TABS_RECONNECT;
			case SETTINGS_ACTION_PAIR:
				return TABS_PAIR;
			case SETTINGS_ACTION_BACK:
				switch_tab(TAB_HOME);
				break;
			default:
				break;
			}
			break;
		}

		frame();
		if (!aptMainLoop())
			return TABS_EXIT;
	}
}

/* --- Account ------------------------------------------------------------- */

/* The account signed in, for the Settings top screen (top_about.h): the
 * username from the session; the name and the picture from spclient's
 * profile view; the plan and the country from the Web API's /v1/me, which
 * is rate-limited hard and often says no. Fetched once per session in the
 * background and kept in the config ("profile", "profile_art") so it shows
 * at once on the next start, and stays when a request fails. */
#define PROFILE_SEP '\x1f'

typedef struct {
	char host[128];
	char library_token[1024];
	char username[128];
	char oauth_token[1280];    /* empty: no /v1/me */
	sp_profile profile;
	int result;
	volatile bool done;
} profile_job;
static profile_job profile;
static Thread profile_thread;

static void profile_worker(void *arg)
{
	profile_job *job = arg;
	char error[96];
	sp_profile view, me;
	bool got_view = sp_user_profile_view(job->host, job->library_token,
	                                     job->username, &view, error,
	                                     sizeof(error)) == 0;
	bool got_me = job->oauth_token[0] &&
	              sp_user_profile(job->oauth_token, &me, error,
	                              sizeof(error), NULL) == 0;
	/* The profile view first for what both have. */
	sp_profile *out = &job->profile;
	if (got_me)
		*out = me;
	if (got_view) {
		if (view.display_name[0])
			memcpy(out->display_name, view.display_name,
			       sizeof(out->display_name));
		if (view.art[0])
			memcpy(out->art, view.art, sizeof(out->art));
	}
	job->result = got_view || got_me ? 0 : -1;
	secure_clear(job->library_token, sizeof(job->library_token));
	secure_clear(job->oauth_token, sizeof(job->oauth_token));
	__sync_synchronize();
	job->done = true;
}

/* "name<SEP>product<SEP>country" back into the account. */
static void account_load_cached(void)
{
	char value[192];
	if (!cfg_get("profile", value, sizeof(value)))
		return;
	char *fields[3] = { value, NULL, NULL };
	for (int i = 1; i < 3; i++) {
		char *sep = fields[i - 1] ? strchr(fields[i - 1], PROFILE_SEP)
		                          : NULL;
		if (sep) {
			*sep = '\0';
			fields[i] = sep + 1;
		}
	}
	/* Written by account_poll from fields of these sizes. */
	snprintf(account.display_name, sizeof(account.display_name), "%.*s",
	         (int)sizeof(account.display_name) - 1, fields[0]);
	snprintf(account.product, sizeof(account.product), "%s",
	         fields[1] ? fields[1] : "");
	snprintf(account.country, sizeof(account.country), "%s",
	         fields[2] ? fields[2] : "");
}

static void account_set_art(const char *url)
{
	if (strcmp(url, account_art_url) == 0)
		return;
	snprintf(account_art_url, sizeof(account_art_url), "%s", url);
	artwork_request(&account_art, account_art_url);
}

/* With the tabs: what the config kept, then /v1/me in the background when
 * the session's token is still good. */
static void account_start(const spotify_session *session)
{
	unsigned serial = account.serial;
	memset(&account, 0, sizeof(account));
	account.serial = serial + 1;
	snprintf(account.username, sizeof(account.username), "%s",
	         session->username);
	account_load_cached();
	char art[80];
	account_set_art(cfg_load_profile_art(art, sizeof(art)) ? art : "");
	top_set_account(&account, &account_art);

	if (profile_thread || !session->username[0] || !session->online)
		return;
	memset(&profile, 0, sizeof(profile));
	snprintf(profile.host, sizeof(profile.host), "%s", session->host);
	snprintf(profile.library_token, sizeof(profile.library_token), "%s",
	         session->library_token);
	snprintf(profile.username, sizeof(profile.username), "%s",
	         session->username);
	/* /v1/me only with a token that is still good: renewing it is the
	 * player's business. */
	if (session->oauth_token[0] &&
	    session->oauth_expires_at > time(NULL) + 60)
		snprintf(profile.oauth_token, sizeof(profile.oauth_token), "%s",
		         session->oauth_token);
	profile_thread = threadCreate(profile_worker, &profile, 16 * 1024,
	                              background_priority(), -2, false);
	if (!profile_thread)
		secure_clear(&profile, sizeof(profile));
}

static void account_join(void)
{
	threadJoin(profile_thread, U64_MAX);
	threadFree(profile_thread);
	profile_thread = NULL;
}

/* Once per frame: takes a finished profile in, and keeps it. */
static void account_poll(void)
{
	if (!profile_thread || !profile.done)
		return;
	__sync_synchronize();
	account_join();
	if (profile.result != 0)
		return;   /* the cached answer stays */
	/* Only what came: a field the requests did not bring (/v1/me said
	 * no) keeps the value kept from before. */
	const sp_profile *p = &profile.profile;
	if (p->display_name[0])
		snprintf(account.display_name, sizeof(account.display_name), "%s",
		         p->display_name);
	if (p->product[0])
		snprintf(account.product, sizeof(account.product), "%s",
		         p->product);
	if (p->country[0])
		snprintf(account.country, sizeof(account.country), "%s",
		         p->country);
	account.serial++;
	char value[192];
	snprintf(value, sizeof(value), "%s%c%s%c%s", account.display_name,
	         PROFILE_SEP, account.product, PROFILE_SEP, account.country);
	cfg_set_async("profile", value);
	if (p->art[0]) {
		cfg_set_async("profile_art", p->art);
		account_set_art(p->art);
	}
}

/* Leaving the tabs: the next session may be another account. */
static void account_stop(void)
{
	if (profile_thread)
		account_join();
	secure_clear(&profile, sizeof(profile));
	unsigned serial = account.serial;
	secure_clear(&account, sizeof(account));
	account.serial = serial + 1;
	account_set_art("");
}

/* Leaving the tabs: no audio, search, details or thumbnails survive the
 * session. */
static void stop_tabs(void)
{
	/* Every worker's network wait fails within TLS_ABORT_SLICE_MS, so the
	 * joins below take a moment, not a whole connection's timeout. */
	reconnect_stop();
	tls_set_abort(true);
	player_stop();
	search_stop();
	details_stop();
	queue_feed_stop();
	home_feed_stop();
	downloads_stop();
	downloads_session = NULL;
	account_stop();
	/* Waits: after this the session, TLS and sockets may go away, and a
	 * thumbnail request still in flight would use them freed. */
	thumbs_wait();
	tls_set_abort(false);
	active_view = VIEW_LOGIN;
}

/* --- Session start-up ---------------------------------------------------- */

/* The start-up runs in a worker so the main thread keeps drawing and reading
 * the buttons while it blocks in TLS handshakes for seconds at a time. The
 * worker never touches the screens: it publishes how far it got, and the
 * main thread draws that. One at a time; global, never on a stack, because
 * the worker holds its address. */
typedef struct {
	bool force_pair;                /* in: skip the stored credential */
	bool no_pair;                   /* in: a dead credential fails instead
	                                 * of pairing (no screen to pair on) */
	spotify_session *session;       /* out, filled on success */
	volatile str_id stage;          /* what it is doing now */
	volatile bool pairing;          /* a code is on screen, being polled */
	volatile bool paired;           /* pairing ran: one more step */
	volatile bool reason_expired;   /* pairing because the credential died */
	volatile bool cancel;           /* main thread: give up */
	volatile bool done;
	bool ok;
	startup_error error;
	/* What the pairing screen shows, copied under `lock`. Never the
	 * device code, which is secret. */
	LightLock lock;
	char user_code[16];
	char verification_uri[128];
	char qr_url[192];
	bool offline;                   /* the last poll did not reach Spotify */
	Thread thread;
} session_job;

/* A consistent copy of the pairing data, for drawing. */
typedef struct {
	bool reason_expired;
	bool offline;
	char user_code[16];
	char verification_uri[128];
	char qr_url[192];
} pair_view;

static session_job login;
/* The worker's large locals, off its stack and cleared after each use. */
static spotify_tokens login_tokens;
static pair_request login_request;
static ap_info login_ap;
static login5_token login_library;
static ap_candidate login_candidates[AP_MAX_CANDIDATES];
static char login_stored[512];
/* As PREPARE_STACK, which already logs in to an access point over TLS. */
#define SESSION_STACK (64 * 1024)
#define SESSION_SLICE_MS 100

static void session_stage(str_id stage)
{
	__sync_synchronize();
	login.stage = stage;
}

static void pair_publish(const pair_request *req, bool offline)
{
	LightLock_Lock(&login.lock);
	snprintf(login.user_code, sizeof(login.user_code), "%s", req->user_code);
	snprintf(login.verification_uri, sizeof(login.verification_uri), "%s",
	         req->verification_uri);
	login_qr_url(req->verification_uri, req->verification_uri_complete,
	             req->user_code, login.qr_url, sizeof(login.qr_url));
	login.offline = offline;
	LightLock_Unlock(&login.lock);
}

static void pair_snapshot(pair_view *out)
{
	LightLock_Lock(&login.lock);
	out->reason_expired = login.reason_expired;
	out->offline = login.offline;
	memcpy(out->user_code, login.user_code, sizeof(out->user_code));
	memcpy(out->verification_uri, login.verification_uri,
	       sizeof(out->verification_uri));
	memcpy(out->qr_url, login.qr_url, sizeof(out->qr_url));
	LightLock_Unlock(&login.lock);
}

/* Waits `seconds` in slices, so a cancel is seen within one. True when it
 * was cancelled. */
static bool session_wait(int seconds)
{
	for (int ms = 0; ms < seconds * 1000 && !login.cancel;
	     ms += SESSION_SLICE_MS)
		svcSleepThread((s64)SESSION_SLICE_MS * 1000000LL);
	return login.cancel;
}

static void pair_cancelled(startup_error *error)
{
	error->category = STR_CAT_PAIR_CANCELLED;
	snprintf(error->detail, sizeof(error->detail), "%s",
	         tr(STR_PAIR_CREDENTIALS_UNCHANGED));
}

/* Worker thread. */
static bool pair_account(spotify_tokens *tokens, startup_error *error)
{
	pair_request *req = &login_request;
	char detail[192] = {0};
	session_stage(STR_PAIR_REQUESTING);
	if (pair_begin(req, detail, sizeof(detail)) != 0) {
		error->category = STR_CAT_PAIRING;
		snprintf(error->detail, sizeof(error->detail), "%s", detail);
		secure_clear(req, sizeof(*req));
		return false;
	}

	int interval = req->interval_s > 0 ? req->interval_s : 5;
	time_t deadline = time(NULL) + req->expires_in_s;
	pair_publish(req, false);
	__sync_synchronize();
	login.pairing = true;
	bool ok = false;
	for (;;) {
		if (session_wait(interval)) {
			pair_cancelled(error);
			break;
		}
		if (time(NULL) >= deadline) {
			error->category = STR_CAT_PAIR_EXPIRED;
			snprintf(error->detail, sizeof(error->detail), "%s",
			         tr(STR_PAIR_RETRY_NEW_CODE));
			break;
		}

		detail[0] = '\0';
		pair_status result = pair_poll(req, tokens, detail, sizeof(detail));
		/* A cancel wins even over an approval that raced it: nothing is
		 * saved, and the stored credential stays as it was. */
		if (login.cancel) {
			secure_clear(tokens, sizeof(*tokens));
			pair_cancelled(error);
			break;
		}
		/* Still pending: with a detail, the poll never reached Spotify
		 * (pair_poll keeps the code good and says why). */
		if (result == PAIR_PENDING) {
			pair_publish(req, detail[0] != '\0');
			continue;
		}
		if (result == PAIR_SLOW_DOWN) {
			interval += 5;
			pair_publish(req, false);
			continue;
		}
		login.pairing = false;
		if (result == PAIR_OK) {
			login.paired = true;
			if (!tokens->refresh_token[0] ||
			    !cfg_save_refresh_token(tokens->refresh_token, tokens->scope)) {
				error->category = STR_CAT_SAVE_CREDENTIALS;
				snprintf(error->detail, sizeof(error->detail), "%s",
				         tr(STR_CHECK_SD_SPACE));
				secure_clear(tokens, sizeof(*tokens));
				break;
			}
			ok = true;
			break;
		}

		error->category = result == PAIR_DENIED
		                  ? STR_CAT_PAIR_DENIED
		                  : result == PAIR_EXPIRED
		                    ? STR_CAT_PAIR_EXPIRED
		                    : STR_CAT_PAIR_FAILED;
		snprintf(error->detail, sizeof(error->detail), "%s",
		         detail[0] ? detail : tr(STR_NO_VALID_RESPONSE));
		secure_clear(tokens, sizeof(*tokens));
		break;
	}
	login.pairing = false;
	secure_clear(req, sizeof(*req));
	return ok;
}

/* Worker thread. */
static bool resolve_and_login(spotify_tokens *tokens, startup_error *error,
                              spotify_session *session)
{
	char detail[192] = {0};
	char device_id[64];
	ap_device_id(device_id, sizeof(device_id));

	ap_candidate *candidates = login_candidates;
	session_stage(STR_AP_SEARCHING);
	int count = ap_resolve_list(candidates, AP_MAX_CANDIDATES,
	                            detail, sizeof(detail));
	if (count < 1) {
		error->category = STR_CAT_AP_RESOLVE;
		snprintf(error->detail, sizeof(error->detail),
		         tr(STR_CHECK_NETWORK_FMT), detail);
		secure_clear(tokens, sizeof(*tokens));
		return false;
	}

	ap_info *info = &login_ap;
	memset(info, 0, sizeof(*info));
	int tried = 0;
	session_stage(STR_AP_AUTHENTICATING);
	ap_session *ap = ap_login_any(candidates, count, tokens->access_token,
	                              device_id, info, &tried,
	                              detail, sizeof(detail));
	if (ap) {
		snprintf(session->oauth_token, sizeof(session->oauth_token), "%s",
		         tokens->access_token);
		snprintf(session->refresh_token, sizeof(session->refresh_token), "%s",
		         tokens->refresh_token);
		session->oauth_expires_at = tokens->expires_at;
		memcpy(session->ap_candidates, candidates,
		       (size_t)count * sizeof(candidates[0]));
		session->ap_candidate_count = count;
	}
	secure_clear(tokens, sizeof(*tokens));
	if (!ap) {
		error->category = info->login_error
		                  ? STR_CAT_AP_REJECTED
		                  : STR_CAT_AP_CONNECT;
		if (info->login_error)
			snprintf(error->detail, sizeof(error->detail),
			         tr(STR_ERROR_CODE_FMT),
			         ap_login_error_text(info->login_error),
			         info->login_error);
		else
			snprintf(error->detail, sizeof(error->detail), "%s",
			         detail[0] ? detail : tr(STR_NO_AP_RESPONDED));
		secure_clear(info, sizeof(*info));
		return false;
	}

	login5_token *library = &login_library;
	session_stage(STR_LIBRARY_OPENING);
	int rc = login5_from_credentials(info->username, info->reusable,
	                                 info->reusable_len, device_id,
	                                 library, detail, sizeof(detail));
	error->transport = rc == LOGIN5_ERR_TRANSPORT;
	snprintf(session->username, sizeof(session->username), "%.*s",
	         (int)sizeof(session->username) - 1, info->username);
	ap_close(ap);
	secure_clear(info, sizeof(*info));
	secure_clear(device_id, sizeof(device_id));
	if (rc != 0) {
		error->category = strstr(detail, "login5 refused:")
		                  ? STR_CAT_LOGIN_REFUSED
		                  : STR_CAT_LOGIN_FAILED;
		snprintf(error->detail, sizeof(error->detail), "%s", detail);
		secure_clear(library, sizeof(*library));
		return false;
	}

	detail[0] = '\0';
	session_stage(STR_SERVER_RESOLVING);
	rc = sp_resolve(session->host, sizeof(session->host),
	                 detail, sizeof(detail));
	if (rc != 0 || !session->host[0]) {
		error->category = STR_CAT_SERVER_RESOLVE;
		snprintf(error->detail, sizeof(error->detail), "%s",
		         detail[0] ? detail : tr(STR_INVALID_SERVER_RESPONSE));
		secure_clear(library, sizeof(*library));
		return false;
	}
	/* Both are 1024 bytes; the bound only tells GCC so, now that the token
	 * is a global it cannot see the end of. */
	snprintf(session->library_token, sizeof(session->library_token), "%.*s",
	         (int)sizeof(session->library_token) - 1, library->access_token);
	secure_clear(library, sizeof(*library));
	return true;
}

/* Worker thread. */
static bool start_session(bool force_pair, startup_error *error,
                          spotify_session *session)
{
	char *stored = login_stored;
	size_t stored_size = sizeof(login_stored);
	spotify_tokens *tokens = &login_tokens;
	memset(stored, 0, stored_size);
	memset(tokens, 0, sizeof(*tokens));

	if (force_pair || !cfg_load_refresh_token(stored, stored_size)) {
		secure_clear(stored, stored_size);
		if (!pair_account(tokens, error))
			return false;
		return resolve_and_login(tokens, error, session);
	}

	char detail[192] = {0};
	session_stage(STR_CREDENTIAL_RENEWING);
	int renewed = token_refresh(stored, tokens, detail, sizeof(detail));
	error->transport = renewed == TOKEN_ERR_TRANSPORT;
	if (renewed != 0) {
		secure_clear(stored, stored_size);
		secure_clear(tokens, sizeof(*tokens));
		if (strstr(detail, "invalid_grant") && !login.no_pair) {
			/* Straight to pairing; the pairing screen says why. */
			login.reason_expired = true;
			if (!pair_account(tokens, error))
				return false;
			return resolve_and_login(tokens, error, session);
		}
		error->category = STR_CAT_OAUTH_RENEW;
		snprintf(error->detail, sizeof(error->detail), "%s",
		         detail[0] ? detail : tr(STR_NO_VALID_RESPONSE));
		return false;
	}

	if (tokens->refresh_token[0] &&
	    strcmp(tokens->refresh_token, stored) != 0 &&
	    !cfg_save_refresh_token(tokens->refresh_token, tokens->scope)) {
		error->category = STR_CAT_SAVE_RENEWED;
		snprintf(error->detail, sizeof(error->detail), "%s",
		         tr(STR_CHECK_SD_SPACE));
		secure_clear(stored, stored_size);
		secure_clear(tokens, sizeof(*tokens));
		return false;
	}
	if (!tokens->refresh_token[0])
		snprintf(tokens->refresh_token, sizeof(tokens->refresh_token), "%s",
		         stored);
	secure_clear(stored, stored_size);
	return resolve_and_login(tokens, error, session);
}

static void session_worker(void *arg)
{
	(void)arg;
	login.ok = start_session(login.force_pair, &login.error, login.session);
	__sync_synchronize();
	login.done = true;
}

/* Cuts the worker's network waits short and joins it: the same pattern as
 * stop_tabs(). DNS is not cut (tls3ds.h), so this may take a moment. */
static void session_cancel(void)
{
	login.cancel = true;
	if (!login.thread)
		return;
	tls_set_abort(true);
	threadJoin(login.thread, U64_MAX);
	threadFree(login.thread);
	login.thread = NULL;
	tls_set_abort(false);
}

/* --- Reconnecting from the tabs ------------------------------------------ */

/* Where a background start-up puts the session; copied into the tabs'
 * once it is safe, then wiped. */
static spotify_session reconnect_session;

static bool reconnect_running(void)
{
	return login.thread != NULL;
}

static void reconnect_start(void)
{
	secure_clear(&login, sizeof(login));
	LightLock_Init(&login.lock);
	login.no_pair = true;
	login.session = &reconnect_session;
	secure_clear(&reconnect_session, sizeof(reconnect_session));
	__sync_synchronize();
	login.thread = threadCreate(session_worker, NULL, SESSION_STACK,
	                            background_priority(), -2, false);
}

/* Once per frame with the tabs. A session that came up waits for the
 * player to be between preparations (its worker reads the session), then
 * replaces the offline one under the session lock. */
static reconnect_result reconnect_poll(spotify_session *session)
{
	if (!login.thread || !login.done)
		return RECONNECT_NONE;
	if (login.ok && (prep.thread || player_preparing()))
		return RECONNECT_NONE;
	__sync_synchronize();
	threadJoin(login.thread, U64_MAX);
	threadFree(login.thread);
	login.thread = NULL;
	reconnect_result result;
	if (login.ok) {
		LightLock_Lock(&session_lock);
		*session = reconnect_session;
		session->online = true;
		LightLock_Unlock(&session_lock);
		result = RECONNECT_ONLINE;
	} else {
		result = login_error_allows_offline(login.error.category,
		                                    login.error.transport)
		         ? RECONNECT_OFFLINE : RECONNECT_RESTART;
	}
	secure_clear(&reconnect_session, sizeof(reconnect_session));
	secure_clear(&login, sizeof(login));
	return result;
}

/* Leaving the tabs: a background start-up is cut short and joined. */
static void reconnect_stop(void)
{
	if (!login.thread)
		return;
	session_cancel();
	secure_clear(&reconnect_session, sizeof(reconnect_session));
	secure_clear(&login, sizeof(login));
}

typedef enum {
	LOGIN_READY,    /* *session is up */
	LOGIN_FAILED,   /* *error says why */
	LOGIN_QUIT,     /* the app is closing */
	LOGIN_OFFLINE   /* no connection: the tabs run on the downloads */
} login_result;

/* Hands the worker's progress to the login view. Cheap enough for every
 * frame: the view changes nothing when the content is the same. */
static void show_login(void)
{
	if (!login.pairing) {
		view_login_connecting(login.stage, login.paired);
		return;
	}
	pair_view pair;
	pair_snapshot(&pair);
	login_pair_info info = {
		.reason_expired = pair.reason_expired,
		.offline = pair.offline,
		.user_code = pair.user_code,
		.verification_uri = pair.verification_uri,
		.qr_url = pair.qr_url,
	};
	view_login_pairing(&info);
	secure_clear(&pair, sizeof(pair));
}

/* Runs the session start-up in the worker and keeps drawing meanwhile. B
 * cancels a pairing without touching the stored credential; SELECT
 * switches the language. */
static login_result run_login(bool force_pair, spotify_session *session,
                              startup_error *error)
{
	secure_clear(&login, sizeof(login));
	LightLock_Init(&login.lock);
	login.force_pair = force_pair;
	login.session = session;
	login.stage = STR_LOGIN_CONNECTING;
	active_view = VIEW_LOGIN;
	show_login();
	frame();

	__sync_synchronize();
	login.thread = threadCreate(session_worker, NULL, SESSION_STACK,
	                            background_priority(), -2, false);
	/* Without a thread, the start-up still works, only without a live
	 * screen: as it always did. */
	if (!login.thread)
		session_worker(NULL);

	while (!login.done) {
		if (!aptMainLoop()) {
			session_cancel();
			secure_clear(&login, sizeof(login));
			return LOGIN_QUIT;
		}
		hidScanInput();
		show_login();
		login_action action = view_login_update(hidKeysDown());
		if (action == LOGIN_ACTION_LANGUAGE)
			switch_language();
		else if (action == LOGIN_ACTION_CANCEL && login.pairing)
			session_cancel();
		frame();
	}

	__sync_synchronize();
	if (login.thread) {
		threadJoin(login.thread, U64_MAX);
		threadFree(login.thread);
		login.thread = NULL;
	}
	login_result result = login.ok ? LOGIN_READY : LOGIN_FAILED;
	if (!login.ok)
		*error = login.error;
	secure_clear(&login, sizeof(login));
	return result;
}

/* --- Without a connection ---------------------------------------------- */

/* A credential is saved: the app may start on its downloads. */
static bool credential_stored(void)
{
	char stored[512];
	bool ok = cfg_load_refresh_token(stored, sizeof(stored));
	secure_clear(stored, sizeof(stored));
	return ok;
}

/* No Wi-Fi right now, asked of ACU without waiting on anything: a start-up
 * then goes straight to the downloads instead of to its timeouts. */
static bool wifi_down(void)
{
	if (R_FAILED(acInit()))
		return false;   /* unknown: try the network as always */
	u32 wifi = 0, status = 0;
	bool wifi_ok = R_SUCCEEDED(ACU_GetWifiStatus(&wifi));
	bool status_ok = R_SUCCEEDED(ACU_GetStatus(&status));
	acExit();
	return login_wifi_down(wifi_ok, wifi, status_ok, status);
}

/* A session for the downloads alone: no host and no token; the username
 * the last session saved, which the downloads index is checked against. */
static void offline_session(spotify_session *session)
{
	secure_clear(session, sizeof(*session));
	session->online = false;
	if (!cfg_get("username", session->username, sizeof(session->username)))
		session->username[0] = '\0';
}

/* The start-up error until the user picks: Retry (A) with the stored
 * credential, Pair (Y) again, or Exit (START). A fatal error (no network
 * to retry with) only exits. Closing the app counts as Exit. */
static login_action run_error(const startup_error *error, bool fatal)
{
	active_view = VIEW_LOGIN;
	view_login_error(error->category, error->detail, fatal);
	while (aptMainLoop()) {
		hidScanInput();
		login_action action = view_login_update(hidKeysDown());
		if (action == LOGIN_ACTION_LANGUAGE)
			switch_language();
		else if (action == LOGIN_ACTION_RETRY ||
		         action == LOGIN_ACTION_PAIR ||
		         action == LOGIN_ACTION_EXIT)
			return action;
		frame();
	}
	return LOGIN_ACTION_EXIT;
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	gfxInitDefault();
	/* Without romfs the logo falls back to the app name as text. */
	bool romfs_ready = R_SUCCEEDED(romfsInit());
	/* Without the renderer there is no way to show anything at all. */
	if (!ui_init()) {
		if (romfs_ready)
			romfsExit();
		gfxExit();
		return 1;
	}
	ptmu_ready = R_SUCCEEDED(ptmuInit());
	lcd_ready = R_SUCCEEDED(gspLcdInit());
	if (lcd_ready)
		gspLcdExit();
	ndm_ready = R_SUCCEEDED(ndmuInit());
	/* Now, with the lid surely open; a failure is retried, and reported,
	 * on the first play. */
	audio_open();
	/* Without icons, thumbnails or the logo the views still work, just
	 * without them. */
	icons_init();
	/* Before any worker that fills it: the thumbnails, the covers. */
	art_cache_init();
	thumbs_init();
	if (romfs_ready)
		logo_load();
	/* After the logo: the loader mounts the fonts' archive beside romfs.
	 * Korean and Chinese, in the background while the session starts. */
	ui_font_start();
	if (!view_home_init() || !view_results_init() ||
	    !view_player_init() || !view_settings_init() || !view_login_init() ||
	    !view_detail_init() || !view_downloads_init() || !top_init()) {
		top_exit();
		view_detail_exit();
		view_login_exit();
		view_settings_exit();
		view_downloads_exit();
		view_player_exit();
		view_results_exit();
		view_home_exit();
		logo_free();
		thumbs_exit();
		icons_exit();
		ui_font_exit();
		ui_exit();
		if (romfs_ready)
			romfsExit();
		gfxExit();
		return 1;
	}
	cfg_set_path(NULL);
	language_init();
	shuffle_init();
	recent_init();
	volume_init();
	hidSetRepeatParameters(NAV_REPEAT_DELAY, NAV_REPEAT_INTERVAL);

	u32 *soc_buffer = memalign(0x1000, SOC_BUFFER);
	bool soc_ready = false;
	bool ps_ready = false;
	bool tls_ready = false;
	startup_error error = {0};
	spotify_session session = {0};

	/* Sockets and TLS: without them only the downloads can play. */
	net_ready = true;
	if (!soc_buffer || R_FAILED(socInit(soc_buffer, SOC_BUFFER))) {
		error.category = STR_CAT_NO_NETWORK;
		snprintf(error.detail, sizeof(error.detail), "%s",
		         tr(STR_CHECK_WIFI_CONNECTED));
		net_ready = false;
	} else {
		soc_ready = true;
	}
	if (R_FAILED(psInit())) {
		error.category = STR_CAT_PS_INIT;
		snprintf(error.detail, sizeof(error.detail), "%s",
		         tr(STR_PS_ENTROPY));
		goto failed;
	}
	ps_ready = true;
	LightLock_Init(&session_lock);
	/* The index loads while the session starts. */
	downloads_init();

	if (net_ready) {
		int rc = tls_global_init();
		if (rc != 0) {
			char reason[128];
			tls_strerror(rc, reason, sizeof(reason));
			error.category = STR_CAT_TLS_INIT;
			snprintf(error.detail, sizeof(error.detail), "%s", reason);
			net_ready = false;
		} else {
			tls_ready = true;
		}
	}
	/* Nothing to start with: no network, and no account to play the
	 * downloads of. */
	if (!net_ready && !credential_stored())
		goto failed;

	bool force_pair = false;
	/* The last tabs asked to connect again: offline once more, they say
	 * so. */
	bool reconnecting = false;
	for (;;) {
		login_result login_state;
		if (!force_pair && credential_stored() &&
		    (!net_ready || wifi_down())) {
			/* No Wi-Fi: straight to the downloads, no timeouts. */
			login_state = LOGIN_OFFLINE;
		} else {
			login_state = run_login(force_pair, &session, &error);
			if (login_state == LOGIN_FAILED && !force_pair &&
			    credential_stored() &&
			    login_error_allows_offline(error.category, error.transport))
				login_state = LOGIN_OFFLINE;
		}
		if (login_state == LOGIN_QUIT)
			goto done;
		if (login_state == LOGIN_READY || login_state == LOGIN_OFFLINE) {
			if (login_state == LOGIN_OFFLINE) {
				offline_session(&session);
			} else {
				session.online = true;
				/* For the next start without a connection. */
				cfg_set_async("username", session.username);
			}
			tabs_exit result = run_tabs(&session,
			                            login_state == LOGIN_OFFLINE &&
			                            reconnecting);
			/* Reconnecting or pairing starts a new session from
			 * scratch: nothing of this one survives. */
			stop_tabs();
			secure_clear(&session, sizeof(session));
			if (result == TABS_EXIT)
				goto done;
			force_pair = result == TABS_PAIR;
			reconnecting = result == TABS_RECONNECT;
		} else {
			reconnecting = false;
			secure_clear(&session, sizeof(session));
			login_action choice = run_error(&error, false);
			if (choice == LOGIN_ACTION_EXIT)
				goto done;
			force_pair = choice == LOGIN_ACTION_PAIR;
		}
	}

failed:
	run_error(&error, true);

done:
	volume_save();
	/* The preferences still queued (cfg_set_async) reach the card. */
	cfg_flush();
	stop_tabs();
	/* The downloads index's last changes, before PS (its keys) goes. */
	if (ps_ready)
		downloads_exit();
	artwork_close(&cover_art);
	artwork_close(&detail_art);
	artwork_close(&account_art);
	artwork_forget_kept();
	secure_clear(&session, sizeof(session));
	secure_clear(&search, sizeof(search));
	secure_clear(playback_queue, sizeof(playback_queue));
	if (tls_ready)
		tls_global_exit();
	if (ps_ready)
		psExit();
	if (soc_ready)
		socExit();
	free(soc_buffer);
	top_exit();
	view_detail_exit();
	view_login_exit();
	view_settings_exit();
	view_downloads_exit();
	view_player_exit();
	view_results_exit();
	view_home_exit();
	logo_free();
	thumbs_exit();
	/* After the thumbnails' worker and the covers' (artwork_close). */
	art_cache_exit();
	icons_exit();
	if (screens_dark)
		set_backlights(true);
	if (ndsp_up)
		ndspExit();
	wifi_hold(false);
	if (ndm_ready)
		ndmuExit();
	aptSetSleepAllowed(true);
	if (ptmu_ready)
		ptmuExit();
	ui_font_exit();
	ui_exit();
	if (romfs_ready)
		romfsExit();
	gfxExit();
	return 0;
}
