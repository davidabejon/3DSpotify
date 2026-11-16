#include "view_detail.h"

#include "detail_logic.h"
#include "draw.h"
#include "icons.h"
#include "lang.h"
#include "list_logic.h"
#include "press_logic.h"
#include "theme.h"
#include "track_list.h"
#include "ui_font.h"

#include <citro2d.h>

#include <stdio.h>

#define TEXT_GLYPHS    1024
#define SCRATCH_GLYPHS UI_FIELD_SIZE

/* Touch control id of the error state's retry button; the track buttons
 * are their detail_button. */
#define CONTROL_RETRY 100

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

/* What the bubble (5.9) says: the language after SELECT, or the order
 * after Y. A new one replaces the last. */
typedef enum { BUBBLE_LANGUAGE, BUBBLE_SHUFFLE } bubble_kind;

/* Parsed text, for `parsed_doc` as it was at `parsed_serial` and
 * `parsed_state`, in `parsed_lang`. */
static C2D_TextBuf text_buf, scratch_buf;
static C2D_Text button_text[DETAIL_BUTTON_COUNT];
static C2D_Text letter_text[GLYPH_COUNT];
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
} detail_look;
static detail_look drawn;
static bool drawn_valid;

/* --- Lifetime ---------------------------------------------------------- */

bool view_detail_init(void)
{
	text_buf = C2D_TextBufNew(TEXT_GLYPHS);
	scratch_buf = C2D_TextBufNew(SCRATCH_GLYPHS);
	if (!text_buf || !scratch_buf ||
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
	text_buf = scratch_buf = NULL;
	track_list_exit(&list);
}

void view_detail_enter(void)
{
	track_list_reset_input(&list);
	for (int i = 0; i < DETAIL_BUTTON_COUNT; i++)
		press_reset(&press_button[i]);
	press_reset(&press_retry);
	for (int i = 0; i < GLYPH_COUNT; i++)
		press_reset(&press_glyph[i]);
	touch.target = -1;
	bubble_frames = -1;
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
	parse(&hint_back, tr(STR_HINT_BACK));
	parse(&hint_cancel, tr(STR_HINT_CANCEL));
	parse(&hint_retry, tr(STR_HINT_RETRY));
	parse(&hint_shuffle_on, tr(STR_HINT_SHUFFLE_ON));
	parse(&hint_shuffle_off, tr(STR_HINT_SHUFFLE_OFF));
	parse(&bubble_text, tr(STR_LANG_NAME));
	parse(&shuffle_on_text, tr(STR_SHUFFLE_ON));
	parse(&shuffle_off_text, tr(STR_SHUFFLE_OFF));
	parse(&loading_text, tr(STR_DETAIL_LOADING));
	parse(&empty_text, tr(STR_DETAIL_EMPTY));
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
		enabled[DETAIL_BUTTON_ARTIST] = t->artist_uri[0] != '\0';
		enabled[DETAIL_BUTTON_ALBUM] = t->album_uri[0] != '\0';
		parse(&button_text[DETAIL_BUTTON_PLAY], tr(STR_DETAIL_PLAY));
		parse_button(DETAIL_BUTTON_ARTIST, STR_DETAIL_ARTIST_FMT,
		             t->artist[0] ? t->artist : tr(STR_UNKNOWN_ARTIST));
		parse_button(DETAIL_BUTTON_ALBUM, STR_DETAIL_ALBUM_FMT,
		             t->album[0] ? t->album : tr(STR_UNKNOWN_ALBUM));
	}
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
			track_list_set(&list, doc->tracks, doc->count, doc->selected);
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
		if (mode == MODE_LIST) {
			/* Where the list was left, if this level was shown before. */
			list.style = doc->kind == DETAIL_ALBUM ? TRACK_LIST_NUMBERS
			                                      : TRACK_LIST_THUMBS;
			track_list_set(&list, doc->tracks, doc->count, doc->selected);
			track_list_scroll_to(&list, doc->first);
		} else {
			track_list_set(&list, NULL, 0, 0);
		}
	}
	parse_text(doc);
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
	    !track_list_same_look(&a->list, &b->list))
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
	for (int i = 0; i < GLYPH_COUNT; i++)
		press_tick(&press_glyph[i]);
	if (bubble_frames >= 0 && ++bubble_frames >= BUBBLE_SHOW)
		bubble_frames = -1;
	if (mode == MODE_LOADING || loading_more)
		loader_frames++;
}

/* --- Input ------------------------------------------------------------- */

static detail_action button_action(detail_button button)
{
	switch (button) {
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
		}
	}
	if (up & KEY_B)
		press_end(&press_glyph[GLYPH_B]);
	if (up & KEY_X)
		press_end(&press_glyph[GLYPH_X]);
	if (up & KEY_Y)
		press_end(&press_glyph[GLYPH_Y]);
}

/* The buttons of the track and error states. Returns true while the
 * stylus owns the input. */
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
	}
	int control;
	touch_event event = touch_track_update(&touch, (down & KEY_TOUCH) != 0,
	                                       (held & KEY_TOUCH) != 0,
	                                       (up & KEY_TOUCH) != 0, hit,
	                                       &control);
	press_state *press = control == CONTROL_RETRY ? &press_retry
	                     : control >= 0 ? &press_button[control] : NULL;
	switch (event) {
	case TOUCH_PRESS:
		if (control != CONTROL_RETRY)
			focus = (detail_button)control;
		press_begin(press);
		break;
	case TOUCH_CANCEL:
		press_cancel(press);
		break;
	case TOUCH_ACTIVATE:
		press_end(press);
		queue(control == CONTROL_RETRY ? DETAIL_ACTION_RETRY
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
		if (event == TRACK_LIST_ACTIVATE)
			queue(DETAIL_ACTION_PLAY, &list.press);  /* the focused row */
		if (event != TRACK_LIST_IDLE)
			return DETAIL_ACTION_NONE;
	} else if (mode == MODE_TRACK || mode == MODE_FAILED) {
		if (handle_button_touch(down, held, up) ||
		    pending != DETAIL_ACTION_NONE)
			return DETAIL_ACTION_NONE;
	}

	if (mode == MODE_LIST && track_list_realign(&list, down, KEY_A | KEY_X)) {
		/* 6.6: with the focus dragged out of view, the first press only
		 * brings the list back to it; A and X show their glyph pressed
		 * but act on nothing they cannot see. */
		if (down & KEY_A)
			press_begin(&press_glyph[GLYPH_A]);
		if (down & KEY_X)
			press_begin(&press_glyph[GLYPH_X]);
		return DETAIL_ACTION_NONE;
	}
	if (mode == MODE_LIST)
		track_list_navigate(&list, down);
	/* 6.2: the focus moves, and shows, in the same frame. */
	if (mode == MODE_TRACK && (down & (KEY_UP | KEY_DOWN)))
		focus = detail_focus_move(focus, (down & KEY_UP) ? -1 : 1, enabled);

	if ((down & KEY_A) && mode == MODE_TRACK) {
		press_key(GLYPH_A, button_action(focus), &press_button[focus]);
	} else if ((down & KEY_A) && mode == MODE_LIST) {
		/* The album or artist from this row (plan 8.1). */
		press_key(GLYPH_A, DETAIL_ACTION_PLAY,
		          track_list_focused_press(&list));
	} else if ((down & KEY_A) && mode == MODE_FAILED) {
		press_key(GLYPH_A, DETAIL_ACTION_RETRY, &press_retry);
	} else if ((down & KEY_X) && mode == MODE_LIST) {
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

static void draw_track(const detail_look *look)
{
	C2D_Image play = icon_image(ICON_PLAY);
	for (int i = 0; i < DETAIL_BUTTON_COUNT; i++) {
		ui_control_look button = {
			.pressed = look->button[i],
			.focused = look->focus == (detail_button)i,
			.disabled = !enabled[i],
		};
		ui_button(detail_button_rect(i),
		          i == DETAIL_BUTTON_PLAY ? UI_BUTTON_PRIMARY
		                                  : UI_BUTTON_SECONDARY,
		          button, &button_text[i],
		          i == DETAIL_BUTTON_PLAY ? &play : NULL);
	}
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
		hints[n++] = hint(GLYPH_A, &hint_play, look);
		hints[n++] = hint(GLYPH_X, &hint_detail, look);
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
	track_list_ensure_parsed(&list);
	detail_look look = current_look();

	switch (look.mode) {
	case MODE_TRACK:
		draw_track(&look);
		break;
	case MODE_LOADING:
		/* Skeleton rows: the layout the tracks will fill. */
		track_list_draw_skeleton();
		break;
	case MODE_LIST:
		track_list_draw(&list, &look.list);
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
		const C2D_Text *text = !order ? &bubble_text
		                       : look.shuffle ? &shuffle_on_text
		                                      : &shuffle_off_text;
		C2D_Image icon = icon_image(order ? ICON_SHUFFLE : ICON_LANGUAGE);
		ui_bubble(list_bubble_slot_rect(ui_bubble_content_width(text), slot),
		          &icon, text);
	}

	drawn = look;
	drawn_valid = true;
}
