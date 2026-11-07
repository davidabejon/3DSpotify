#include "view_player.h"

#include "draw.h"
#include "icons.h"
#include "player_logic.h"
#include "press_logic.h"
#include "theme.h"

#include <citro2d.h>

#include <stdio.h>
#include <string.h>

#define STATIC_GLYPHS 768
#define TRACK_GLYPHS 320
#define TIME_GLYPHS 24
#define SCRATCH_GLYPHS UI_FIELD_SIZE

/* The button bar (STYLES.md 5.6). */
enum { GLYPH_A, GLYPH_B, GLYPH_COUNT };
static const char *const glyph_letters[GLYPH_COUNT] = { "A", "B" };
static const u32 glyph_colors[GLYPH_COUNT] = { COL_BTN_A, COL_BTN_B };

static C2D_TextBuf static_buf, track_buf, time_buf, scratch_buf;

static C2D_Text idle_text, stage_text, finished_text, volume_label;
static C2D_Text failed_title, failed_body;
static C2D_Text letter_text[GLYPH_COUNT];
static C2D_Text hint_pause, hint_play, hint_retry, hint_back;
static C2D_Text title_text, artist_text, queue_text;
static C2D_Text elapsed_text, duration_text;
static int volume_label_w;

/* What was parsed, to parse again only on a change. */
static lang_id parsed_lang = LANG_COUNT;
static str_id parsed_stage = STR_COUNT;
static char parsed_error[256];
static unsigned parsed_serial = ~0u;
static int parsed_index = -1, parsed_count = -1;
static unsigned parsed_elapsed = ~0u, parsed_duration = ~0u;

static char info_error[256];
static player_info info = { .stage = STR_PLAYER_ART_LOADING,
                            .error = info_error };
/* 9: the download share follows the clock, not every frame. */
static player_download_sample download;

static press_state press_control[PLAYER_CONTROL_COUNT];
static press_state press_glyph[GLYPH_COUNT];
static touch_track touch = { -1 };
static player_action pending = PLAYER_ACTION_NONE;
static const press_state *pending_press;
static int loader_frames;

typedef struct {
	player_phase phase;
	unsigned serial;
	int index, count;
	str_id stage;
	bool paused;
	unsigned elapsed, duration, download, volume;
	bool control[PLAYER_CONTROL_COUNT];
	bool glyph[GLYPH_COUNT];
	int loader_step;
} player_look;
static player_look drawn;
static bool drawn_valid;

/* --- Lifetime --------------------------------------------------------- */

bool view_player_init(void)
{
	static_buf = C2D_TextBufNew(STATIC_GLYPHS);
	track_buf = C2D_TextBufNew(TRACK_GLYPHS);
	time_buf = C2D_TextBufNew(TIME_GLYPHS);
	scratch_buf = C2D_TextBufNew(SCRATCH_GLYPHS);
	if (!static_buf || !track_buf || !time_buf || !scratch_buf) {
		view_player_exit();
		return false;
	}
	return true;
}

void view_player_exit(void)
{
	C2D_TextBuf *bufs[] = { &static_buf, &track_buf, &time_buf,
	                        &scratch_buf };
	for (size_t i = 0; i < sizeof(bufs) / sizeof(bufs[0]); i++) {
		if (*bufs[i])
			C2D_TextBufDelete(*bufs[i]);
		*bufs[i] = NULL;
	}
}

void view_player_enter(void)
{
	for (int i = 0; i < PLAYER_CONTROL_COUNT; i++)
		press_reset(&press_control[i]);
	for (int i = 0; i < GLYPH_COUNT; i++)
		press_reset(&press_glyph[i]);
	touch.target = -1;
	pending = PLAYER_ACTION_NONE;
	pending_press = NULL;
	drawn_valid = false;
}

void view_player_set(const player_info *next)
{
	bool tick = next->elapsed_s != info.elapsed_s ||
	            next->phase != info.phase || next->serial != info.serial;
	info = *next;
	snprintf(info_error, sizeof(info_error), "%s",
	         next->error ? next->error : "");
	info.error = info_error;
	player_sample_download(&download, tick, info.download_pct);
}

/* --- Text ------------------------------------------------------------- */

static void parse(C2D_Text *text, C2D_TextBuf buf, const char *string)
{
	C2D_TextParse(text, buf, string);
	C2D_TextOptimize(text);
}

static void parse_fitted(C2D_Text *text, C2D_TextBuf buf, const char *string,
                         float scale, int width)
{
	ui_parse_fitted(text, buf, scratch_buf, string, scale, width);
}

static int content_width(void)
{
	return SCREEN_BOTTOM_W - 2 * SP_2;
}

static void parse_static(void)
{
	C2D_TextBufClear(static_buf);
	parse(&idle_text, static_buf, tr(STR_PLAYER_IDLE));
	parse(&stage_text, static_buf, tr(info.stage));
	parse(&finished_text, static_buf, tr(STR_PLAYER_FINISHED_TITLE));
	parse(&volume_label, static_buf, tr(STR_PLAYER_VOLUME));
	volume_label_w = ui_px_ceil(ui_text_width(&volume_label, TXT_META));
	ui_rect notice = player_notice_rect();
	parse_fitted(&failed_title, static_buf, tr(STR_PLAYER_FAILED_TITLE),
	             TXT_CABEZA, notice.w - 2 * SP_3 - ICON_S - SP_1);
	parse(&failed_body, static_buf,
	      info.error[0] ? info.error : tr(STR_PLAYER_INTERRUPTED));
	for (int i = 0; i < GLYPH_COUNT; i++)
		parse(&letter_text[i], static_buf, glyph_letters[i]);
	parse(&hint_pause, static_buf, tr(STR_HINT_PAUSE));
	parse(&hint_play, static_buf, tr(STR_HINT_PLAY));
	parse(&hint_retry, static_buf, tr(STR_HINT_RETRY));
	parse(&hint_back, static_buf, tr(STR_HINT_BACK));
}

static void parse_track(void)
{
	C2D_TextBufClear(track_buf);
	if (!info.track)
		return;
	char queue[24];
	snprintf(queue, sizeof(queue), tr(STR_PLAYER_POSITION_FMT),
	         info.index + 1, info.count);
	parse(&queue_text, track_buf, queue);
	int queue_w = ui_px_ceil(ui_text_width(&queue_text, TXT_META));
	const char *name = info.track->name[0] ? info.track->name
	                                       : tr(STR_UNTITLED);
	const char *artist = info.track->artist[0] ? info.track->artist
	                                           : tr(STR_UNKNOWN_ARTIST);
	parse_fitted(&title_text, track_buf, name, TXT_CABEZA,
	             content_width() - queue_w - SP_2);
	parse_fitted(&artist_text, track_buf, artist, TXT_CUERPO,
	             content_width());
}

static void parse_times(void)
{
	C2D_TextBufClear(time_buf);
	char text[16];
	player_format_time(info.elapsed_s, text, sizeof(text));
	parse(&elapsed_text, time_buf, text);
	player_format_time(info.duration_s, text, sizeof(text));
	parse(&duration_text, time_buf, text);
}

static void ensure_parsed(void)
{
	bool lang_changed = parsed_lang != lang_get();
	if (lang_changed || parsed_stage != info.stage ||
	    strcmp(parsed_error, info.error) != 0) {
		parse_static();
		parsed_stage = info.stage;
		snprintf(parsed_error, sizeof(parsed_error), "%s", info.error);
	}
	if (lang_changed || parsed_serial != info.serial ||
	    parsed_index != info.index || parsed_count != info.count) {
		parse_track();
		parsed_serial = info.serial;
		parsed_index = info.index;
		parsed_count = info.count;
	}
	if (parsed_elapsed != info.elapsed_s ||
	    parsed_duration != info.duration_s) {
		parse_times();
		parsed_elapsed = info.elapsed_s;
		parsed_duration = info.duration_s;
	}
	parsed_lang = lang_get();
}

/* --- State ------------------------------------------------------------ */

static bool has_controls(void)
{
	return info.phase == PLAYER_PHASE_PLAYING ||
	       info.phase == PLAYER_PHASE_FINISHED;
}

static bool control_enabled(int control)
{
	switch (control) {
	case PLAYER_CONTROL_PREV:
		return has_controls() && info.index > 0;
	case PLAYER_CONTROL_NEXT:
		return has_controls() && info.index + 1 < info.count;
	default:
		return has_controls();
	}
}

static player_look current_look(void)
{
	player_look look = {
		.phase = info.phase,
		.serial = info.serial,
		.index = info.index,
		.count = info.count,
		.stage = info.stage,
		.paused = info.paused,
		.elapsed = info.elapsed_s,
		.duration = info.duration_s,
		.download = download.shown,
		.volume = info.volume_pct,
		.loader_step = info.phase == PLAYER_PHASE_LOADING
		               ? loader_frames / LOADER_STEP : 0,
	};
	for (int i = 0; i < PLAYER_CONTROL_COUNT; i++)
		look.control[i] = press_down(&press_control[i]);
	for (int i = 0; i < GLYPH_COUNT; i++)
		look.glyph[i] = press_down(&press_glyph[i]);
	return look;
}

static bool same_look(const player_look *a, const player_look *b)
{
	if (a->phase != b->phase || a->serial != b->serial ||
	    a->index != b->index || a->count != b->count ||
	    a->stage != b->stage || a->paused != b->paused ||
	    a->elapsed != b->elapsed || a->duration != b->duration ||
	    a->download != b->download || a->volume != b->volume ||
	    a->loader_step != b->loader_step)
		return false;
	for (int i = 0; i < PLAYER_CONTROL_COUNT; i++)
		if (a->control[i] != b->control[i])
			return false;
	for (int i = 0; i < GLYPH_COUNT; i++)
		if (a->glyph[i] != b->glyph[i])
			return false;
	return true;
}

static void tick(void)
{
	for (int i = 0; i < PLAYER_CONTROL_COUNT; i++)
		press_tick(&press_control[i]);
	for (int i = 0; i < GLYPH_COUNT; i++)
		press_tick(&press_glyph[i]);
	if (info.phase == PLAYER_PHASE_LOADING)
		loader_frames++;
}

/* --- Input ------------------------------------------------------------ */

static player_action control_action(int control)
{
	switch (control) {
	case PLAYER_CONTROL_PREV:
		return PLAYER_ACTION_PREV;
	case PLAYER_CONTROL_NEXT:
		return PLAYER_ACTION_NEXT;
	default:
		return PLAYER_ACTION_TOGGLE;
	}
}

/* Pausing answers in the same frame (6.8); anything that loads a track
 * takes the screens over, so it waits for the press to be seen. */
static bool loads_track(player_action action)
{
	return action != PLAYER_ACTION_TOGGLE ||
	       info.phase != PLAYER_PHASE_PLAYING;
}

static void queue(player_action action, const press_state *press)
{
	pending = action;
	pending_press = press;
}

static void release_keys(u32 up)
{
	if (touch.target < 0) {
		if (up & KEY_A)
			press_end(&press_control[PLAYER_CONTROL_PLAY]);
		if (up & KEY_LEFT)
			press_end(&press_control[PLAYER_CONTROL_PREV]);
		if (up & KEY_RIGHT)
			press_end(&press_control[PLAYER_CONTROL_NEXT]);
	}
	if (up & KEY_A)
		press_end(&press_glyph[GLYPH_A]);
	if (up & KEY_B)
		press_end(&press_glyph[GLYPH_B]);
}

/* Returns the action to run now, if any; sets `*owned` while the stylus
 * owns the input. */
static player_action handle_touch(u32 down, u32 held, u32 up, bool *owned)
{
	int hit = -1;
	if (held & KEY_TOUCH) {
		touchPosition pos;
		hidTouchRead(&pos);
		hit = player_hit_test(pos.px, pos.py);
		if (hit >= 0 && !control_enabled(hit))
			hit = -1;
	}
	int control;
	touch_event event = touch_track_update(&touch, (down & KEY_TOUCH) != 0,
	                                       (held & KEY_TOUCH) != 0,
	                                       (up & KEY_TOUCH) != 0, hit,
	                                       &control);
	player_action now = PLAYER_ACTION_NONE;
	switch (event) {
	case TOUCH_PRESS:
		press_begin(&press_control[control]);
		break;
	case TOUCH_CANCEL:
		press_cancel(&press_control[control]);
		break;
	case TOUCH_ACTIVATE: {
		press_end(&press_control[control]);
		player_action action = control_action(control);
		if (loads_track(action))
			queue(action, &press_control[control]);
		else
			now = action;
		break;
	}
	case TOUCH_NONE:
		break;
	}
	*owned = touch.target >= 0;
	return now;
}

/* A physical button on a control (6.5): the control shows the press. */
static player_action press_key(int control, int glyph)
{
	press_begin(&press_control[control]);
	if (glyph >= 0)
		press_begin(&press_glyph[glyph]);
	player_action action = control_action(control);
	if (!loads_track(action))
		return action;
	queue(action, &press_control[control]);
	return PLAYER_ACTION_NONE;
}

player_action view_player_update(u32 down)
{
	u32 held = hidKeysHeld();
	u32 up = hidKeysUp();
	ensure_parsed();
	tick();
	release_keys(up);

	if (pending != PLAYER_ACTION_NONE) {
		if (!press_shown(pending_press))
			return PLAYER_ACTION_NONE;
		player_action action = pending;
		pending = PLAYER_ACTION_NONE;
		pending_press = NULL;
		return action;
	}

	bool owned;
	player_action now = handle_touch(down, held, up, &owned);
	if (now != PLAYER_ACTION_NONE || owned || pending != PLAYER_ACTION_NONE)
		return now;

	if (down & KEY_A) {
		if (info.phase == PLAYER_PHASE_FAILED) {
			/* No control on screen: the glyph shows the press. */
			press_begin(&press_glyph[GLYPH_A]);
			queue(PLAYER_ACTION_TOGGLE, &press_glyph[GLYPH_A]);
			return PLAYER_ACTION_NONE;
		}
		if (control_enabled(PLAYER_CONTROL_PLAY))
			return press_key(PLAYER_CONTROL_PLAY, GLYPH_A);
	}
	if ((down & KEY_LEFT) && control_enabled(PLAYER_CONTROL_PREV))
		return press_key(PLAYER_CONTROL_PREV, -1);
	if ((down & KEY_RIGHT) && control_enabled(PLAYER_CONTROL_NEXT))
		return press_key(PLAYER_CONTROL_NEXT, -1);
	/* 6.7: the bar follows in the same frame. */
	if (down & KEY_UP)
		return PLAYER_ACTION_VOLUME_UP;
	if (down & KEY_DOWN)
		return PLAYER_ACTION_VOLUME_DOWN;
	if (down & KEY_B) {
		press_begin(&press_glyph[GLYPH_B]);
		queue(PLAYER_ACTION_BACK, &press_glyph[GLYPH_B]);
		return PLAYER_ACTION_NONE;
	}
	if (down & KEY_SELECT)
		return PLAYER_ACTION_LANGUAGE;
	return PLAYER_ACTION_NONE;
}

bool view_player_dirty(void)
{
	if (!drawn_valid || parsed_lang != lang_get())
		return true;
	player_look look = current_look();
	return !same_look(&look, &drawn);
}

/* --- Drawing ---------------------------------------------------------- */

static void draw_track(void)
{
	int title_cy = PLAYER_TEXT_TOP + ui_line_height(TXT_CABEZA) / 2;
	int artist_cy = PLAYER_TEXT_TOP + ui_line_height(TXT_CABEZA) + SP_1 +
	                ui_line_height(TXT_CUERPO) / 2;
	ui_text(&title_text, SP_2, title_cy, TXT_CABEZA, COL_TEXTO);
	int queue_w = ui_px_ceil(ui_text_width(&queue_text, TXT_META));
	ui_text(&queue_text, SCREEN_BOTTOM_W - SP_2 - queue_w, title_cy,
	        TXT_META, COL_TEXTO_2);
	ui_text(&artist_text, SP_2, artist_cy, TXT_CUERPO, COL_TEXTO_2);
}

static void draw_controls(const player_look *look)
{
	static const icon_id skip_icons[PLAYER_CONTROL_COUNT] = {
		ICON_PREV, ICON_PLAY, ICON_NEXT
	};
	for (int i = 0; i < PLAYER_CONTROL_COUNT; i++) {
		ui_control_look button = {
			.pressed = look->control[i],
			.disabled = !control_enabled(i),
		};
		icon_id id = skip_icons[i];
		if (i == PLAYER_CONTROL_PLAY &&
		    look->phase == PLAYER_PHASE_PLAYING && !look->paused)
			id = ICON_PAUSE;
		C2D_Image icon = icon_image(id);
		ui_icon_button(player_control_rect(i),
		               i == PLAYER_CONTROL_PLAY ? UI_BUTTON_PRIMARY
		                                        : UI_BUTTON_SECONDARY,
		               button, &icon);
	}
}

static void draw_bars(const player_look *look)
{
	int cy = player_times_cy();
	if (look->phase == PLAYER_PHASE_FINISHED) {
		ui_text(&finished_text, SP_2, cy, TXT_META, COL_TEXTO_2);
	} else {
		ui_text(&elapsed_text, SP_2, cy, TXT_META, COL_TEXTO_2);
		int w = ui_px_ceil(ui_text_width(&duration_text, TXT_META));
		ui_text(&duration_text, SCREEN_BOTTOM_W - SP_2 - w, cy, TXT_META,
		        COL_TEXTO_2);
	}
	int progress = player_progress_permille(
		look->phase == PLAYER_PHASE_FINISHED, look->elapsed, look->duration);
	ui_progress(player_progress_rect(),
	            player_permille(look->download, 100), progress, true);

	ui_rect volume = player_volume_rect(volume_label_w);
	ui_text(&volume_label, SP_2, volume.y + volume.h / 2, TXT_META,
	        COL_TEXTO_2);
	ui_progress(volume, -1, player_permille(look->volume, 100), false);
}

static void draw_hints(const player_look *look)
{
	ui_hint hints[GLYPH_COUNT];
	int n = 0;
	const C2D_Text *a_label = NULL;
	switch (look->phase) {
	case PLAYER_PHASE_PLAYING:
		a_label = look->paused ? &hint_play : &hint_pause;
		break;
	case PLAYER_PHASE_FINISHED:
		a_label = &hint_play;
		break;
	case PLAYER_PHASE_FAILED:
		a_label = &hint_retry;
		break;
	default:
		break;
	}
	if (a_label)
		hints[n++] = (ui_hint){ &letter_text[GLYPH_A], a_label,
		                        glyph_colors[GLYPH_A],
		                        look->glyph[GLYPH_A] };
	hints[n++] = (ui_hint){ &letter_text[GLYPH_B], &hint_back,
	                        glyph_colors[GLYPH_B], look->glyph[GLYPH_B] };
	ui_button_bar(player_hints_rect(), hints, n);
}

void view_player_draw_bottom(void)
{
	ensure_parsed();
	player_look look = current_look();
	int middle = (TAB_H + SCREEN_H - BAR_H) / 2;

	switch (look.phase) {
	case PLAYER_PHASE_IDLE:
		/* A single message: centred (3.3). */
		ui_text_centered(&idle_text, SCREEN_BOTTOM_W / 2, middle,
		                 TXT_CUERPO, COL_TEXTO);
		break;
	case PLAYER_PHASE_FAILED: {
		C2D_Image icon = icon_image(ICON_ERROR);
		ui_notice(player_notice_rect(), true, &icon, &failed_title,
		          &failed_body);
		break;
	}
	case PLAYER_PHASE_LOADING: {
		/* Waiting, with no audio yet: the indicator is allowed (5.7). */
		draw_track();
		int cy = PLAYER_CONTROLS_TOP + BTN_PLAY / 2;
		ui_loader(SCREEN_BOTTOM_W / 2, cy, look.loader_step);
		ui_text_centered(&stage_text, SCREEN_BOTTOM_W / 2,
		                 cy + LOADER_BAR_H + SP_4, TXT_CUERPO, COL_TEXTO);
		break;
	}
	default:
		draw_track();
		draw_controls(&look);
		draw_bars(&look);
		break;
	}
	draw_hints(&look);

	drawn = look;
	drawn_valid = true;
}
