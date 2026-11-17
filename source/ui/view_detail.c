#include "view_detail.h"

#include "detail_logic.h"
#include "downloads.h"
#include "draw.h"
#include "icons.h"
#include "lang.h"
#include "list_logic.h"
#include "press_logic.h"
#include "theme.h"
#include "track_list.h"
#include "ui.h"
#include "ui_font.h"

#include <citro2d.h>

#include <stdio.h>

#define TEXT_GLYPHS    1024
#define SCRATCH_GLYPHS UI_FIELD_SIZE

/* Touch control ids of the error state's retry button and of the button
 * over a list; the track buttons are their detail_button. */
#define CONTROL_RETRY  100
#define CONTROL_HEADER 101

/* The button over an album or playlist on the card: it removes it. Past
 * the dl_coll_button values. */
#define DL_COLL_BUTTON_REMOVE 100

/* A download's share is read again once a second (STYLES.md 9). */
#define DL_SAMPLE_FRAMES 60
/* "Press A again to remove" shows, and waits for that press, this long. */
#define CONFIRM_FRAMES   (2 * BUBBLE_SHOW)

typedef enum {
	MODE_NONE,       /* no document: nothing but B */
	MODE_TRACK,      /* the three buttons of 4.1 */
	MODE_LOADING,    /* an album or artist on its way */
	MODE_FAILED,     /* ...that did not come */
	MODE_LIST,       /* its tracks */
	MODE_EMPTY       /* ...when it has none */
} detail_mode;

/* The button bar (STYLES.md 5.6). */
enum { GLYPH_A, GLYPH_B, GLYPH_X, GLYPH_Y, GLYPH_COUNT };
static const char *const glyph_letters[GLYPH_COUNT] = { "A", "B", "X", "Y" };
static const u32 glyph_colors[GLYPH_COUNT] = {
	COL_BTN_A, COL_BTN_B, COL_BTN_X, COL_BTN_Y
};

/* What the bubble (5.9) says: the language after SELECT, the order after
 * Y, or what a download button did (view_detail_notice). A new one
 * replaces the last. */
typedef enum { BUBBLE_LANGUAGE, BUBBLE_SHUFFLE, BUBBLE_NOTICE } bubble_kind;

/* The download button's state: a dl_button (the track) or a
 * dl_coll_button (the button over an album or playlist), with its share,
 * or how many of how many. `doc_kind` is whose button it is: the same
 * `kind` says "Download" on a track and "Download playlist" on a list. */
typedef struct {
	int doc_kind, kind, a, b;
} dl_label;

/* Parsed text, for `parsed_doc` as it was at `parsed_serial` and
 * `parsed_state`, in `parsed_lang`. */
static C2D_TextBuf text_buf, scratch_buf;
static C2D_Text button_text[DETAIL_BUTTON_COUNT];
static C2D_Text letter_text[GLYPH_COUNT];
static C2D_Text hint_open;
static C2D_Text hint_choose, hint_play, hint_detail, hint_back, hint_cancel,
                hint_retry, hint_shuffle_on, hint_shuffle_off;
static C2D_Text bubble_text, shuffle_on_text, shuffle_off_text, loading_text,
                empty_text, retry_text;
static C2D_Text notice_title_text, notice_body_text;
static lang_id parsed_lang = LANG_COUNT;
static const detail_doc *parsed_doc;
static unsigned parsed_serial;
static detail_state parsed_state;
/* A playlist grows page by page: the count the list was last given, and
 * whether the next page is on its way (the waiting bubble shows). */
static int parsed_count;
static bool loading_more;

/* The download of what is shown, and its text, in a buffer of its own:
 * the share changes while the rest stays. */
static C2D_TextBuf dl_buf;
static C2D_Text dl_text, notice_text;
static dl_label dl_now = { -1, -1, 0, 0 }, dl_parsed = { -1, -1, 0, 0 };
static unsigned dl_seen = ~0u;
static int dl_frames;
static lang_id dl_lang = LANG_COUNT;
static str_id notice_id = STR_COUNT, notice_parsed = STR_COUNT;
static bool notice_ok;
/* An album's or playlist's list has its download button over it; the
 * D-pad reaches it up from the first row (detail_header_focus). */
static bool header, header_enabled, header_focus;
/* What the level on screen is (details.h). */
static detail_kind shown_kind = DETAIL_TRACK;
static press_state press_header;

static detail_mode mode = MODE_NONE;
/* Which track buttons the track allows (6.1). */
static bool enabled[DETAIL_BUTTON_COUNT];
/* An album's or artist's tracks. Its style follows the kind: numbers for
 * an album, whose rows share one cover; thumbnails for an artist. */
static track_list list;

/* Interaction (STYLES.md 6). `touch` is the buttons'; the rows are the
 * list's. */
static detail_button focus = DETAIL_BUTTON_PLAY;
static press_state press_button[DETAIL_BUTTON_COUNT];
static press_state press_retry;
static press_state press_glyph[GLYPH_COUNT];
static touch_track touch = { -1 };
static int bubble_frames = -1;
static bubble_kind bubble = BUBBLE_LANGUAGE;
static int loader_frames;
/* Whether a list's queue plays shuffled: the app's setting, shown on Y. */
static bool shuffle;
static detail_action pending = DETAIL_ACTION_NONE;
static const press_state *pending_press;

typedef struct {
	detail_mode mode;
	detail_button focus;
	bool button[DETAIL_BUTTON_COUNT];
	bool retry;
	bool glyph[GLYPH_COUNT];
	bool bubble;
	bubble_kind bubble_kind;
	bool shuffle;
	bool more;
	int loader_step;
	track_list_look list;
	dl_label dl;
	str_id notice;
	bool header, header_focus, header_pressed, header_enabled;
} detail_look;
static detail_look drawn;
static bool drawn_valid;

/* --- Lifetime ---------------------------------------------------------- */

bool view_detail_init(void)
{
	text_buf = C2D_TextBufNew(TEXT_GLYPHS);
	scratch_buf = C2D_TextBufNew(SCRATCH_GLYPHS);
	dl_buf = C2D_TextBufNew(2 * UI_FIELD_SIZE);
	if (!text_buf || !scratch_buf || !dl_buf ||
	    !track_list_init(&list, TRACK_LIST_NUMBERS)) {
		view_detail_exit();
		return false;
	}
	return true;
}

void view_detail_exit(void)
{
	if (text_buf)
		C2D_TextBufDelete(text_buf);
	if (scratch_buf)
		C2D_TextBufDelete(scratch_buf);
	if (dl_buf)
		C2D_TextBufDelete(dl_buf);
	text_buf = scratch_buf = dl_buf = NULL;
	track_list_exit(&list);
}

void view_detail_enter(void)
{
	track_list_reset_input(&list);
	for (int i = 0; i < DETAIL_BUTTON_COUNT; i++)
		press_reset(&press_button[i]);
	press_reset(&press_retry);
	press_reset(&press_header);
	for (int i = 0; i < GLYPH_COUNT; i++)
		press_reset(&press_glyph[i]);
	touch.target = -1;
	bubble_frames = -1;
	header_focus = false;
	dl_seen = ~0u;
	loader_frames = 0;
	pending = DETAIL_ACTION_NONE;
	pending_press = NULL;
	drawn_valid = false;
}

/* --- Text -------------------------------------------------------------- */

static void parse(C2D_Text *text, const char *string)
{
	ui_parse(text, text_buf, string);
	C2D_TextOptimize(text);
}

/* A secondary button's label: `fmt` with `name`, cut with "..." to fit
 * (3.3); the whole name is on the top screen. */
static void parse_button(detail_button button, str_id fmt, const char *name)
{
	char label[192];
	snprintf(label, sizeof(label), tr(fmt), name);
	int width = detail_button_rect(button).w - 2 * SP_4;
	ui_parse_fitted(&button_text[button], text_buf, scratch_buf, label,
	                TXT_CUERPO, width);
}

static detail_mode mode_of(const detail_doc *doc)
{
	if (!doc)
		return MODE_NONE;
	if (doc->kind == DETAIL_TRACK)
		return doc->count > 0 ? MODE_TRACK : MODE_NONE;
	switch (doc->state) {
	case DETAIL_LOADING:
		return MODE_LOADING;
	case DETAIL_FAILED:
		return MODE_FAILED;
	default:
		return doc->count > 0 ? MODE_LIST : MODE_EMPTY;
	}
}

static bool same_level(const detail_doc *doc)
{
	return doc && doc == parsed_doc && doc->serial == parsed_serial;
}

static bool doc_changed(const detail_doc *doc)
{
	return !same_level(doc) || doc->state != parsed_state;
}

static void parse_text(const detail_doc *doc)
{
	C2D_TextBufClear(text_buf);
	for (int i = 0; i < GLYPH_COUNT; i++)
		parse(&letter_text[i], glyph_letters[i]);
	parse(&hint_choose, tr(STR_HINT_CHOOSE));
	parse(&hint_play, tr(STR_HINT_PLAY));
	parse(&hint_detail, tr(STR_HINT_DETAIL));
	parse(&hint_open, tr(STR_HINT_OPEN));
	parse(&hint_back, tr(STR_HINT_BACK));
	parse(&hint_cancel, tr(STR_HINT_CANCEL));
	parse(&hint_retry, tr(STR_HINT_RETRY));
	parse(&hint_shuffle_on, tr(STR_HINT_SHUFFLE_ON));
	parse(&hint_shuffle_off, tr(STR_HINT_SHUFFLE_OFF));
	parse(&bubble_text, tr(STR_LANG_NAME));
	parse(&shuffle_on_text, tr(STR_SHUFFLE_ON));
	parse(&shuffle_off_text, tr(STR_SHUFFLE_OFF));
	parse(&loading_text, tr(STR_DETAIL_LOADING));
	parse(&empty_text, tr(doc && doc->kind == DETAIL_DOWNLOADS
	                      ? STR_DLH_EMPTY : STR_DETAIL_EMPTY));
	parse(&retry_text, tr(STR_RESULTS_RETRY));
	ui_rect notice = list_notice_rect();
	ui_parse_fitted(&notice_title_text, text_buf, scratch_buf,
	                tr(STR_DETAIL_FAILED_TITLE), TXT_CABEZA,
	                notice.w - 2 * SP_3 - ICON_S - SP_1);
	parse(&notice_body_text,
	      mode == MODE_FAILED && doc->error[0] ? doc->error
	                                           : tr(STR_NO_VALID_RESPONSE));

	for (int i = 0; i < DETAIL_BUTTON_COUNT; i++)
		enabled[i] = false;
	if (mode == MODE_TRACK) {
		const sp_track *t = &doc->tracks[doc->selected];
		enabled[DETAIL_BUTTON_PLAY] = true;
		enabled[DETAIL_BUTTON_DOWNLOAD] = dl_button_enabled(dl_now.kind);
		enabled[DETAIL_BUTTON_ARTIST] = t->artist_uri[0] != '\0';
		enabled[DETAIL_BUTTON_ALBUM] = t->album_uri[0] != '\0';
		parse(&button_text[DETAIL_BUTTON_PLAY], tr(STR_DETAIL_PLAY));
		parse_button(DETAIL_BUTTON_ARTIST, STR_DETAIL_ARTIST_FMT,
		             t->artist[0] ? t->artist : tr(STR_UNKNOWN_ARTIST));
		parse_button(DETAIL_BUTTON_ALBUM, STR_DETAIL_ALBUM_FMT,
		             t->album[0] ? t->album : tr(STR_UNKNOWN_ALBUM));
	}
}

/* --- Downloads --------------------------------------------------------- */

static bool same_label(dl_label a, dl_label b)
{
	return a.doc_kind == b.doc_kind && a.kind == b.kind && a.a == b.a &&
	       a.b == b.b;
}

/* What the download button says now. */
static void label_string(const detail_doc *doc, char *out, size_t len)
{
	str_id id = STR_DL_DOWNLOAD;
	if (mode == MODE_TRACK) {
		switch ((dl_button)dl_now.kind) {
		case DL_BUTTON_QUEUED:
			id = STR_DL_QUEUED;
			break;
		case DL_BUTTON_PROGRESS:
			snprintf(out, len, tr(STR_DL_PROGRESS_FMT), dl_now.a);
			return;
		case DL_BUTTON_RETRY:
			id = STR_DL_RETRY;
			break;
		case DL_BUTTON_REMOVE:
			id = STR_DL_REMOVE;
			break;
		case DL_BUTTON_HELD:
			id = STR_DL_DONE;
			break;
		default:
			break;
		}
	} else if (dl_now.kind == DL_COLL_BUTTON_REMOVE) {
		id = STR_DL_REMOVE;
	} else {
		switch ((dl_coll_button)dl_now.kind) {
		case DL_COLL_BUTTON_WAITING:
			id = STR_DL_COLL_WAITING;
			break;
		case DL_COLL_BUTTON_PROGRESS:
			snprintf(out, len, tr(STR_DL_COLL_PROGRESS_FMT), dl_now.a,
			         dl_now.b);
			return;
		case DL_COLL_BUTTON_DONE:
			id = STR_DL_DONE;
			break;
		default:
			id = doc && doc->kind == DETAIL_ALBUM ? STR_DL_ALL_ALBUM
			                                      : STR_DL_ALL_PLAYLIST;
			break;
		}
	}
	snprintf(out, len, "%s", tr(id));
}

/* The button's label and the notice, when either changed. */
static void parse_download(const detail_doc *doc)
{
	if (same_label(dl_parsed, dl_now) && dl_lang == lang_get() &&
	    notice_parsed == notice_id)
		return;
	C2D_TextBufClear(dl_buf);
	char label[96];
	label_string(doc, label, sizeof(label));
	ui_parse_fitted(&dl_text, dl_buf, scratch_buf, label, TXT_CUERPO,
	                detail_button_rect(DETAIL_BUTTON_DOWNLOAD).w - 2 * SP_4 -
	                ICON_S - SP_1);
	C2D_TextOptimize(&dl_text);
	if (notice_id != STR_COUNT) {
		ui_parse(&notice_text, dl_buf, tr(notice_id));
		C2D_TextOptimize(&notice_text);
	}
	dl_parsed = dl_now;
	dl_lang = lang_get();
	notice_parsed = notice_id;
}

/* Reads the download of what is shown again when the downloads changed,
 * once a second while a track's is under way (its share), or when
 * `force`d by a new document. */
static void follow_download(const detail_doc *doc, bool force)
{
	if (!doc || (mode != MODE_TRACK && !header))
		return;
	unsigned now = downloads_generation();
	bool sample = mode == MODE_TRACK && dl_now.kind == DL_BUTTON_PROGRESS &&
	              ++dl_frames >= DL_SAMPLE_FRAMES;
	if (!force && now == dl_seen && !sample)
		return;
	dl_frames = 0;
	dl_seen = now;
	dl_label next = { (int)doc->kind, 0, 0, 0 };
	if (mode == MODE_TRACK) {
		const sp_track *t = &doc->tracks[doc->selected];
		dl_state state = DL_QUEUED;
		bool alone = false;
		int pct = 0;
		bool known = downloads_track_state(t->uri, &state, &pct, &alone);
		next.kind = dl_track_button(known, state, alone);
		if (next.kind == DL_BUTTON_PROGRESS)
			next.a = pct;
		enabled[DETAIL_BUTTON_DOWNLOAD] = dl_button_enabled(next.kind);
		/* Queued by its own press: a disabled button holds no focus. */
		focus = detail_focus_move(focus, 0, enabled);
	} else if (doc->kind == DETAIL_OFFLINE_COLLECTION) {
		next.kind = DL_COLL_BUTTON_REMOVE;
		header_enabled = true;
	} else {
		bool pending;
		int done, count;
		bool known = downloads_collection_state(doc->uri, &pending, &done,
		                                        &count);
		next.kind = dl_collection_button(pending, known, done, count);
		if (next.kind == DL_COLL_BUTTON_PROGRESS) {
			next.a = done;
			next.b = count;
		}
		/* "Downloaded" opens it from the card. */
		header_enabled = next.kind == DL_COLL_BUTTON_DOWNLOAD ||
		                 next.kind == DL_COLL_BUTTON_DONE;
		header_focus = detail_header_focus(header_focus, list.selected, 0,
		                                   header_enabled);
	}
	dl_now = next;
}

static bool has_header(const detail_doc *doc)
{
	return doc && (doc->kind == DETAIL_ALBUM || doc->kind == DETAIL_PLAYLIST ||
	               doc->kind == DETAIL_OFFLINE_COLLECTION);
}

/* An album's or playlist's list goes under its download button (one on
 * the card, under the button that removes it); an artist's (popular
 * tracks are no collection to keep) and the Downloads have the screen. */
static void place_list(const detail_doc *doc)
{
	header = mode == MODE_LIST && has_header(doc);
	if (!header)
		header_focus = false;
	track_list_set_frame(&list, header ? list_frame_under_button()
	                                   : list_frame_full());
}

static list_frame skeleton_frame(const detail_doc *doc)
{
	return has_header(doc) ? list_frame_under_button() : list_frame_full();
}

static bool remove_armed(void)
{
	return bubble_frames >= 0 && bubble == BUBBLE_NOTICE &&
	       notice_id == STR_DL_REMOVE_CONFIRM;
}

/* A or a tap on the track's download button. Removing asks first: the
 * first press says to press again, and a second one while that shows
 * removes. */
static detail_action download_action(void)
{
	if (dl_now.kind != DL_BUTTON_REMOVE)
		return DETAIL_ACTION_DOWNLOAD;
	if (remove_armed()) {
		bubble_frames = -1;
		return DETAIL_ACTION_DOWNLOAD;
	}
	view_detail_notice(STR_DL_REMOVE_CONFIRM, true);
	return DETAIL_ACTION_NONE;
}

/* A or a tap on the button over the list: download it all, open it from
 * the card, or (on the card) remove it after a second press. */
static detail_action header_action(void)
{
	if (shown_kind != DETAIL_OFFLINE_COLLECTION)
		return DETAIL_ACTION_DOWNLOAD_ALL;
	if (remove_armed()) {
		bubble_frames = -1;
		return DETAIL_ACTION_REMOVE_ALL;
	}
	view_detail_notice(STR_DL_REMOVE_CONFIRM, true);
	return DETAIL_ACTION_NONE;
}

/* The focused row of the Downloads is an album or a playlist. */
static bool focused_collection(void)
{
	return list.rows && list.count > 0 &&
	       list.rows[list.selected].kind != TRACK_ROW_TRACK;
}

/* A, or a tap, on the focused row: play from it, or open the collection;
 * nothing on a track that is not on the card. */
static detail_action row_action(void)
{
	if (list.count <= 0 || !track_row_on(list.rows, list.selected))
		return DETAIL_ACTION_NONE;
	return focused_collection() ? DETAIL_ACTION_OPEN_COLLECTION
	                            : DETAIL_ACTION_PLAY;
}

/* Follows `doc`: another level starts afresh (focus on Play, no press
 * left over); the same level that finished loading keeps what was going
 * on; either way the text is parsed again, as on a language switch. */
static void sync(const detail_doc *doc)
{
	loading_more = doc && doc->loading_more && mode == MODE_LIST;
	/* A page arrived: the same list, longer, focus and scroll kept. A first
	 * page of nothing but episodes left it empty until now. */
	if (!doc_changed(doc) && doc->count != parsed_count) {
		if (mode == MODE_LIST) {
			track_list_extend(&list, doc->count);
		} else if (mode == MODE_EMPTY && doc->count > 0) {
			mode = MODE_LIST;
			list.style = TRACK_LIST_THUMBS;
			place_list(doc);
			track_list_set(&list, doc->tracks, doc->count, doc->selected);
			track_list_set_rows(&list, doc->rows);
			follow_download(doc, true);
		}
		parsed_count = doc->count;
	}
	bool changed = doc_changed(doc);
	if (!changed && parsed_lang == lang_get())
		return;
	if (changed) {
		if (!same_level(doc)) {
			view_detail_enter();
			focus = DETAIL_BUTTON_PLAY;
		}
		mode = mode_of(doc);
		shown_kind = doc ? doc->kind : DETAIL_TRACK;
		place_list(doc);
		if (mode == MODE_LIST) {
			/* Where the list was left, if this level was shown before. */
			bool numbers = doc->kind == DETAIL_ALBUM ||
			               (doc->kind == DETAIL_OFFLINE_COLLECTION &&
			                doc->offline_album);
			list.style = numbers ? TRACK_LIST_NUMBERS : TRACK_LIST_THUMBS;
			track_list_set(&list, doc->tracks, doc->count, doc->selected);
			track_list_set_rows(&list, doc->rows);
			track_list_scroll_to(&list, doc->first);
		} else {
			track_list_set(&list, NULL, 0, 0);
		}
	}
	parse_text(doc);
	/* parse_text set the buttons afresh: the download's follows. */
	follow_download(doc, true);
	parsed_doc = doc;
	parsed_serial = doc ? doc->serial : 0;
	parsed_state = doc ? doc->state : DETAIL_LOADING;
	parsed_count = doc ? doc->count : 0;
	parsed_lang = lang_get();
}

/* --- State ------------------------------------------------------------- */

static detail_look current_look(void)
{
	detail_look look = {
		.mode = mode,
		.focus = focus,
		.retry = press_down(&press_retry),
		.bubble = bubble_frames >= 0,
		.bubble_kind = bubble,
		.shuffle = shuffle,
		.more = loading_more,
		.loader_step = mode == MODE_LOADING || loading_more
		               ? loader_frames / LOADER_STEP : 0,
		.list = track_list_get_look(&list),
		.dl = dl_now,
		.notice = notice_id,
		.header = header,
		.header_focus = header_focus,
		.header_pressed = press_down(&press_header),
		.header_enabled = header_enabled,
	};
	for (int i = 0; i < DETAIL_BUTTON_COUNT; i++)
		look.button[i] = press_down(&press_button[i]);
	for (int i = 0; i < GLYPH_COUNT; i++)
		look.glyph[i] = press_down(&press_glyph[i]);
	return look;
}

static bool same_look(const detail_look *a, const detail_look *b)
{
	if (a->mode != b->mode || a->focus != b->focus || a->retry != b->retry ||
	    a->bubble != b->bubble || a->bubble_kind != b->bubble_kind ||
	    a->shuffle != b->shuffle || a->more != b->more ||
	    a->loader_step != b->loader_step ||
	    !track_list_same_look(&a->list, &b->list) ||
	    !same_label(a->dl, b->dl) || a->notice != b->notice ||
	    a->header != b->header || a->header_focus != b->header_focus ||
	    a->header_pressed != b->header_pressed ||
	    a->header_enabled != b->header_enabled)
		return false;
	for (int i = 0; i < DETAIL_BUTTON_COUNT; i++)
		if (a->button[i] != b->button[i])
			return false;
	for (int i = 0; i < GLYPH_COUNT; i++)
		if (a->glyph[i] != b->glyph[i])
			return false;
	return true;
}

static void tick(void)
{
	track_list_tick(&list);
	for (int i = 0; i < DETAIL_BUTTON_COUNT; i++)
		press_tick(&press_button[i]);
	press_tick(&press_retry);
	press_tick(&press_header);
	for (int i = 0; i < GLYPH_COUNT; i++)
		press_tick(&press_glyph[i]);
	/* "Press A again" waits for that press longer than a bubble shows. */
	int shown = remove_armed() ? CONFIRM_FRAMES : BUBBLE_SHOW;
	if (bubble_frames >= 0 && ++bubble_frames >= shown)
		bubble_frames = -1;
	if (mode == MODE_LOADING || loading_more)
		loader_frames++;
}

/* --- Input ------------------------------------------------------------- */

static detail_action button_action(detail_button button)
{
	switch (button) {
	case DETAIL_BUTTON_DOWNLOAD:
		return download_action();
	case DETAIL_BUTTON_ARTIST:
		return DETAIL_ACTION_ARTIST;
	case DETAIL_BUTTON_ALBUM:
		return DETAIL_ACTION_ALBUM;
	default:
		return DETAIL_ACTION_PLAY;
	}
}

static ui_rect retry_rect(void)
{
	return list_action_rect(ui_text_width(&retry_text, TXT_CUERPO), false);
}

static void queue(detail_action action, const press_state *press)
{
	pending = action;
	pending_press = press;
}

/* The glyph plus what the button does here. */
static void press_key(int glyph, detail_action action, press_state *control)
{
	press_begin(&press_glyph[glyph]);
	if (control)
		press_begin(control);
	queue(action, control ? control : &press_glyph[glyph]);
}

static void release_keys(u32 up)
{
	if (up & KEY_A) {
		press_end(&press_glyph[GLYPH_A]);
		track_list_release(&list);
		if (touch.target < 0) {
			for (int i = 0; i < DETAIL_BUTTON_COUNT; i++)
				press_end(&press_button[i]);
			press_end(&press_retry);
			press_end(&press_header);
		}
	}
	if (up & KEY_B)
		press_end(&press_glyph[GLYPH_B]);
	if (up & KEY_X)
		press_end(&press_glyph[GLYPH_X]);
	if (up & KEY_Y)
		press_end(&press_glyph[GLYPH_Y]);
}

/* The buttons of the track and error states, and the one over a list.
 * Returns true while the stylus owns the input. */
static bool handle_button_touch(u32 down, u32 held, u32 up)
{
	int hit = -1;
	if (held & KEY_TOUCH) {
		touchPosition pos;
		hidTouchRead(&pos);
		if (mode == MODE_TRACK)
			hit = detail_hit_test(pos.px, pos.py, enabled);
		else if (mode == MODE_FAILED &&
		         ui_rect_contains(retry_rect(), pos.px, pos.py))
			hit = CONTROL_RETRY;
		else if (header && header_enabled &&
		         ui_rect_contains(list_header_button_rect(), pos.px, pos.py))
			hit = CONTROL_HEADER;
	}
	int control;
	touch_event event = touch_track_update(&touch, (down & KEY_TOUCH) != 0,
	                                       (held & KEY_TOUCH) != 0,
	                                       (up & KEY_TOUCH) != 0, hit,
	                                       &control);
	press_state *press = control == CONTROL_RETRY ? &press_retry
	                     : control == CONTROL_HEADER ? &press_header
	                     : control >= 0 ? &press_button[control] : NULL;
	switch (event) {
	case TOUCH_PRESS:
		if (control == CONTROL_HEADER)
			header_focus = true;
		else if (control != CONTROL_RETRY)
			focus = (detail_button)control;
		press_begin(press);
		break;
	case TOUCH_CANCEL:
		press_cancel(press);
		break;
	case TOUCH_ACTIVATE:
		press_end(press);
		queue(control == CONTROL_RETRY ? DETAIL_ACTION_RETRY
		      : control == CONTROL_HEADER ? header_action()
		      : button_action((detail_button)control),
		      press);
		break;
	case TOUCH_NONE:
		break;
	}
	return touch.target >= 0;
}

static detail_action update(u32 down)
{
	u32 held = hidKeysHeld();
	u32 up = hidKeysUp();
	tick();
	release_keys(up);
	if (mode == MODE_LIST)
		track_list_thumbs_frame(&list);

	if (pending != DETAIL_ACTION_NONE) {
		if (!press_shown(pending_press))
			return DETAIL_ACTION_NONE;
		detail_action action = pending;
		pending = DETAIL_ACTION_NONE;
		pending_press = NULL;
		/* The caller switches the order; the bubble shows the new one. */
		if (action == DETAIL_ACTION_SHUFFLE) {
			bubble = BUBBLE_SHUFFLE;
			bubble_frames = 0;
		}
		return action;
	}

	/* The stylus first: it owns the input while it is down. */
	if (mode == MODE_LIST) {
		track_list_touch event = track_list_update_touch(&list, down, held,
		                                                 up);
		/* A row touched while the button over the list has the focus
		 * takes the focus back first, without playing. */
		if (event == TRACK_LIST_ACTIVATE && !header_focus)
			queue(row_action(), &list.press);  /* the focused row */
		if (event != TRACK_LIST_IDLE) {
			header_focus = false;
			return DETAIL_ACTION_NONE;
		}
		if (header && (handle_button_touch(down, held, up) ||
		               pending != DETAIL_ACTION_NONE))
			return DETAIL_ACTION_NONE;
	} else if (mode == MODE_TRACK || mode == MODE_FAILED) {
		if (handle_button_touch(down, held, up) ||
		    pending != DETAIL_ACTION_NONE)
			return DETAIL_ACTION_NONE;
	}

	if (mode == MODE_LIST && !header_focus &&
	    track_list_realign(&list, down, KEY_A | KEY_X)) {
		/* 6.6: with the focus dragged out of view, the first press only
		 * brings the list back to it; A and X show their glyph pressed
		 * but act on nothing they cannot see. */
		if (down & KEY_A)
			press_begin(&press_glyph[GLYPH_A]);
		if (down & KEY_X)
			press_begin(&press_glyph[GLYPH_X]);
		return DETAIL_ACTION_NONE;
	}
	if (mode == MODE_LIST) {
		/* Up from the first row to the button over the list, and down
		 * back; the list itself moves only while it has the focus. */
		u32 nav = down;
		if (down & (KEY_UP | KEY_DOWN)) {
			bool next = detail_header_focus(header_focus, list.selected,
			                                (down & KEY_UP) ? -1 : 1,
			                                header && header_enabled);
			if (next != header_focus)
				nav &= ~(u32)(KEY_UP | KEY_DOWN);
			header_focus = next;
		}
		if (!header_focus)
			track_list_navigate(&list, nav);
	}
	/* 6.2: the focus moves, and shows, in the same frame. */
	if (mode == MODE_TRACK && (down & (KEY_UP | KEY_DOWN)))
		focus = detail_focus_move(focus, (down & KEY_UP) ? -1 : 1, enabled);

	if ((down & KEY_A) && mode == MODE_TRACK) {
		press_key(GLYPH_A, button_action(focus), &press_button[focus]);
	} else if ((down & KEY_A) && mode == MODE_LIST && header_focus) {
		press_key(GLYPH_A, header_action(), &press_header);
	} else if ((down & KEY_A) && mode == MODE_LIST) {
		/* The album or artist from this row (plan 8.1). */
		press_key(GLYPH_A, row_action(), track_list_focused_press(&list));
	} else if ((down & KEY_A) && mode == MODE_FAILED) {
		press_key(GLYPH_A, DETAIL_ACTION_RETRY, &press_retry);
	} else if ((down & KEY_X) && mode == MODE_LIST && !header_focus &&
	           !focused_collection() && list.count > 0) {
		press_key(GLYPH_X, DETAIL_ACTION_OPEN_TRACK, NULL);
	} else if ((down & KEY_Y) && mode == MODE_LIST) {
		press_key(GLYPH_Y, DETAIL_ACTION_SHUFFLE, NULL);
	} else if (down & KEY_B) {
		/* While loading this cancels: going back forgets the load. */
		press_key(GLYPH_B, DETAIL_ACTION_BACK, NULL);
	} else if (down & KEY_SELECT) {
		/* No language control on screen: a bubble with the result (6.5). */
		bubble = BUBBLE_LANGUAGE;
		bubble_frames = 0;
		return DETAIL_ACTION_LANGUAGE;
	}
	return DETAIL_ACTION_NONE;
}

detail_action view_detail_update(detail_doc *doc, u32 down)
{
	sync(doc);
	follow_download(doc, false);
	parse_download(doc);
	detail_action action = update(down);
	/* The level keeps where its list was, for "Play", for the track detail
	 * opened from it, and for coming back to it. */
	if (mode == MODE_LIST) {
		doc->selected = list.selected;
		doc->first = list.first;
	}
	return action;
}

void view_detail_set_shuffle(bool on)
{
	shuffle = on;
}

void view_detail_notice(str_id text, bool ok)
{
	notice_id = text;
	notice_ok = ok;
	bubble = BUBBLE_NOTICE;
	bubble_frames = 0;
}

bool view_detail_dirty(const detail_doc *doc)
{
	if (doc_changed(doc) || parsed_lang != lang_get() || !drawn_valid ||
	    track_list_text_stale(&list) || doc->count != parsed_count ||
	    (doc->loading_more && mode == MODE_LIST) != loading_more)
		return true;
	detail_look look = current_look();
	return !same_look(&look, &drawn);
}

/* --- Drawing ----------------------------------------------------------- */

/* The download button's icon: what pressing it would do, or where the
 * download is. */
static icon_id download_icon(const detail_look *look)
{
	if (look->mode == MODE_TRACK) {
		switch ((dl_button)look->dl.kind) {
		case DL_BUTTON_QUEUED:
			return ICON_QUEUED;
		case DL_BUTTON_REMOVE:
		case DL_BUTTON_HELD:
			return ICON_DOWNLOADED;
		default:
			return ICON_DOWNLOAD;
		}
	}
	if (look->dl.kind == DL_COLL_BUTTON_REMOVE)
		return ICON_DOWNLOADED;
	switch ((dl_coll_button)look->dl.kind) {
	case DL_COLL_BUTTON_WAITING:
		return ICON_QUEUED;
	case DL_COLL_BUTTON_DONE:
		return ICON_DOWNLOADED;
	default:
		return ICON_DOWNLOAD;
	}
}

static void draw_track(const detail_look *look)
{
	C2D_Image play = icon_image(ICON_PLAY);
	C2D_Image download = icon_image(download_icon(look));
	for (int i = 0; i < DETAIL_BUTTON_COUNT; i++) {
		ui_control_look button = {
			.pressed = look->button[i],
			.focused = look->focus == (detail_button)i,
			.disabled = !enabled[i],
		};
		bool dl = i == DETAIL_BUTTON_DOWNLOAD;
		ui_button(detail_button_rect(i),
		          i == DETAIL_BUTTON_PLAY ? UI_BUTTON_PRIMARY
		                                  : UI_BUTTON_SECONDARY,
		          button, dl ? &dl_text : &button_text[i],
		          i == DETAIL_BUTTON_PLAY ? &play : dl ? &download : NULL);
	}
}

/* The button over an album's or playlist's list: its download. */
static void draw_header(const detail_look *look)
{
	C2D_Image icon = icon_image(download_icon(look));
	ui_control_look button = {
		.pressed = look->header_pressed,
		.focused = look->header_focus,
		.disabled = !look->header_enabled,
	};
	ui_button(list_header_button_rect(), UI_BUTTON_SECONDARY, button,
	          &dl_text, &icon);
}

static ui_hint hint(int glyph, const C2D_Text *label, const detail_look *look)
{
	return (ui_hint){ &letter_text[glyph], label, glyph_colors[glyph],
	                  look->glyph[glyph] };
}

static void draw_bar(const detail_look *look)
{
	ui_hint hints[GLYPH_COUNT];
	int n = 0;
	switch (look->mode) {
	case MODE_TRACK:
		hints[n++] = hint(GLYPH_A, &hint_choose, look);
		break;
	case MODE_LIST:
		/* On the button over the list, A downloads, and X has no row. */
		if (look->header_focus) {
			hints[n++] = hint(GLYPH_A, &hint_choose, look);
		} else {
			/* A collection of the Downloads opens, and has no detail of
			 * a track to show. */
			bool coll = list.rows && list.count > 0 &&
			            list.rows[look->list.selected].kind !=
			            TRACK_ROW_TRACK;
			hints[n++] = hint(GLYPH_A, coll ? &hint_open : &hint_play, look);
			if (!coll)
				hints[n++] = hint(GLYPH_X, &hint_detail, look);
		}
		hints[n++] = hint(GLYPH_Y, look->shuffle ? &hint_shuffle_on
		                                         : &hint_shuffle_off, look);
		break;
	case MODE_FAILED:
		hints[n++] = hint(GLYPH_A, &hint_retry, look);
		break;
	default:
		break;
	}
	hints[n++] = hint(GLYPH_B, look->mode == MODE_LOADING ? &hint_cancel
	                                                      : &hint_back,
	                  look);
	ui_button_bar(detail_hints_rect(), hints, n);
}

void view_detail_draw_bottom(const detail_doc *doc)
{
	sync(doc);
	parse_download(doc);
	track_list_ensure_parsed(&list);
	detail_look look = current_look();

	switch (look.mode) {
	case MODE_TRACK:
		draw_track(&look);
		break;
	case MODE_LOADING:
		/* Skeleton rows: the layout the tracks will fill. */
		track_list_draw_skeleton(skeleton_frame(doc));
		break;
	case MODE_LIST:
		if (look.header) {
			/* No row shows the focus while the button has it. Rows
			 * dragged up slide under the button, as under the bars. */
			track_list_look rows = look.list;
			if (look.header_focus)
				rows.selected = -1;
			track_list_draw(&list, &rows);
			ui_background((ui_rect){ 0, TAB_H, SCREEN_BOTTOM_W,
			                         list.frame.top - TAB_H });
			draw_header(&look);
		} else {
			track_list_draw(&list, &look.list);
		}
		break;
	case MODE_EMPTY: {
		/* A single message: centred (3.3). */
		ui_rect notice = list_notice_rect();
		ui_text_centered(&empty_text, SCREEN_BOTTOM_W / 2,
		                 notice.y + notice.h / 2, TXT_CUERPO, COL_TEXTO);
		break;
	}
	case MODE_FAILED: {
		C2D_Image icon = icon_image(ICON_ERROR);
		ui_notice(list_notice_rect(), true, &icon, &notice_title_text,
		          &notice_body_text);
		ui_control_look button = { .pressed = look.retry, .focused = true };
		ui_button(retry_rect(), UI_BUTTON_PRIMARY, button, &retry_text, NULL);
		break;
	}
	case MODE_NONE:
		break;
	}

	/* The bar after the rows: nudged and dragged rows slide under it, and
	 * under the tab bar, drawn by the caller. */
	draw_bar(&look);

	/* While loading, the waiting bubble (5.9 with the indicator of 5.7)
	 * over the skeleton rows; the language bubble stacks above it. */
	int slot = 0;
	if (look.mode == MODE_LOADING || look.more) {
		ui_bubble_loading(list_bubble_rect(
			ui_bubble_loading_content_width(&loading_text)),
			look.loader_step, &loading_text);
		slot = 1;
	}
	if (look.bubble) {
		bool order = look.bubble_kind == BUBBLE_SHUFFLE;
		bool notice = look.bubble_kind == BUBBLE_NOTICE;
		const C2D_Text *text = notice ? &notice_text
		                       : !order ? &bubble_text
		                       : look.shuffle ? &shuffle_on_text
		                                      : &shuffle_off_text;
		C2D_Image icon = icon_image(notice ? (notice_ok ? ICON_NOTE
		                                                : ICON_ERROR)
		                            : order ? ICON_SHUFFLE : ICON_LANGUAGE);
		ui_bubble(list_bubble_slot_rect(ui_bubble_content_width(text), slot),
		          &icon, text);
	}

	drawn = look;
	drawn_valid = true;
}
