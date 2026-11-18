#include "top_detail.h"

#include "artist_logic.h"
#include "text_wrap.h"
#include "downloads_view_logic.h"
#include "draw.h"
#include "icons.h"
#include "lang.h"
#include "list_logic.h"
#include "marquee_logic.h"
#include "now_layout.h"
#include "theme.h"
#include "top_now.h"
#include "ui_font.h"

#include <3ds.h>
#include <citro2d.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most lines of a biography the top screen holds at once. */
#define BIO_PAGE_VISIBLE_MAX 16
#define TEXT_GLYPHS    ((1 + NOW_LINES + ARTIST_BIO_LINES) * UI_FIELD_SIZE)

static C2D_TextBuf text_buf;
/* The title, then up to NOW_LINES lines under it. */
static C2D_Text texts[1 + NOW_LINES];
static int text_w[1 + NOW_LINES];
static int line_count;
/* An artist's biography, wrapped to the column when parsed (text_wrap.h),
 * and the buffer its lines are measured in. */
static C2D_Text bio_texts[ARTIST_BIO_LINES];
static int bio_count;
static C2D_TextBuf measure_buf;
/* The whole biography (top_detail_bio_*). */
#define BIO_PAGE_LINES_MAX 400
#define BIO_PAGE_SCROLL_W  SP_1

static char (*page_lines)[TEXT_WRAP_LINE_MAX];
static int page_total;
/* What was wrapped: this document, as it was, with this biography. */
static const detail_doc *page_doc;
static unsigned page_serial;
static const char *page_src;
static C2D_TextBuf page_buf;
static C2D_Text page_texts[BIO_PAGE_VISIBLE_MAX];
static C2D_Text page_note;
static int page_first = -1, page_shown;
static lang_id page_lang = LANG_COUNT;
static bool drawn_page;
static int drawn_bio_scroll;
/* When the texts were parsed, the start of every marquee; and how far
 * each one had rolled when drawn. */
static u64 parsed_ms;
static int drawn_rolls[1 + NOW_LINES];

/* What was parsed: the document as it was, in this language. */
static const detail_doc *parsed_doc;
static unsigned parsed_serial;
static detail_state parsed_state;
/* A playlist's count grows as its pages arrive. */
static int parsed_count;
/* An artist's listeners may come after the rest. */
static long parsed_listeners;
static lang_id parsed_lang = LANG_COUNT;
static bool drawn_valid;

bool top_detail_init(void)
{
	text_buf = C2D_TextBufNew(TEXT_GLYPHS);
	measure_buf = C2D_TextBufNew(TEXT_WRAP_LINE_MAX);
	page_buf = C2D_TextBufNew((BIO_PAGE_VISIBLE_MAX + 1) * TEXT_WRAP_LINE_MAX);
	if (!text_buf || !measure_buf || !page_buf) {
		top_detail_exit();
		return false;
	}
	return true;
}

void top_detail_exit(void)
{
	if (text_buf)
		C2D_TextBufDelete(text_buf);
	text_buf = NULL;
	if (measure_buf)
		C2D_TextBufDelete(measure_buf);
	measure_buf = NULL;
	if (page_buf)
		C2D_TextBufDelete(page_buf);
	page_buf = NULL;
	free(page_lines);
	page_lines = NULL;
	page_doc = NULL;
	page_src = NULL;
	parsed_doc = NULL;
	parsed_lang = LANG_COUNT;
	drawn_valid = false;
}

void top_detail_invalidate(void)
{
	drawn_valid = false;
}

/* --- Text -------------------------------------------------------------- */

/* The whole string: one too wide for the column rolls (marquee_logic.h)
 * instead of being cut. */
static void set_text(C2D_Text *text, const char *string, float scale)
{
	ui_parse(text, text_buf, string);
	C2D_TextOptimize(text);
	text_w[text - texts] = ui_px_ceil(ui_text_width(text, scale));
}

/* Adds a line under the title; empty strings take no line. */
static void add_line(const char *string)
{
	if (!string || !string[0] || line_count >= NOW_LINES)
		return;
	set_text(&texts[1 + line_count++], string, TXT_CUERPO);
}

/* "3:42 - track 2 - disc 2": the disc only when there is more than one. */
static void track_meta(const sp_track *t, char *out, size_t len)
{
	list_format_duration(t->duration_ms, out, len);
	char part[32];
	if (t->track_number > 0) {
		snprintf(part, sizeof(part), tr(STR_DETAIL_TRACK_NUMBER_FMT),
		         t->track_number);
		snprintf(out + strlen(out), len - strlen(out), " - %s", part);
	}
	if (t->disc_number > 1) {
		snprintf(part, sizeof(part), tr(STR_DETAIL_DISC_FMT),
		         t->disc_number);
		snprintf(out + strlen(out), len - strlen(out), " - %s", part);
	}
}

static const char *album_type(int type)
{
	switch (type) {
	case SP_ALBUM_ALBUM:       return tr(STR_ALBUM_TYPE_ALBUM);
	case SP_ALBUM_SINGLE:      return tr(STR_ALBUM_TYPE_SINGLE);
	case SP_ALBUM_COMPILATION: return tr(STR_ALBUM_TYPE_COMPILATION);
	case SP_ALBUM_EP:          return tr(STR_ALBUM_TYPE_EP);
	default:                   return "";
	}
}

static void parse_track(const sp_track *t)
{
	set_text(&texts[0], t->name[0] ? t->name : tr(STR_UNTITLED), TXT_TITULO);
	add_line(t->artist[0] ? t->artist : tr(STR_UNKNOWN_ARTIST));
	add_line(t->album[0] ? t->album : tr(STR_UNKNOWN_ALBUM));
	char line[64];
	track_meta(t, line, sizeof(line));
	add_line(line);
	if (t->year > 0) {
		snprintf(line, sizeof(line), "%d", t->year);
		add_line(line);
	}
}

static void parse_album(const detail_doc *d)
{
	set_text(&texts[0], d->title[0] ? d->title : tr(STR_UNTITLED),
	         TXT_TITULO);
	if (d->preview) {
		/* A search result: what it is and by whom, as on its row. Nothing
		 * else is known without opening it. */
		char what[192];
		snprintf(what, sizeof(what), tr(STR_RESULT_ALBUM_FMT),
		         d->subtitle[0] ? d->subtitle : tr(STR_UNKNOWN_ARTIST));
		add_line(what);
		return;
	}
	add_line(d->subtitle);
	/* "Album - 2004", or whichever half is known. */
	char line[64];
	const char *type = album_type(d->album_type);
	if (type[0] && d->year > 0)
		snprintf(line, sizeof(line), "%s - %d", type, d->year);
	else if (d->year > 0)
		snprintf(line, sizeof(line), "%d", d->year);
	else
		snprintf(line, sizeof(line), "%s", type);
	add_line(line);
	/* Past DETAIL_MAX_TRACKS, how many of how many the list shows. */
	if (d->total_tracks > d->count)
		snprintf(line, sizeof(line), tr(STR_ALBUM_TRACKS_CUT_FMT), d->count,
		         d->total_tracks);
	else if (d->count == 1)
		snprintf(line, sizeof(line), "%s", tr(STR_ALBUM_TRACK_ONE));
	else
		snprintf(line, sizeof(line), tr(STR_ALBUM_TRACKS_FMT), d->count);
	add_line(line);
	add_line(d->label);
}

/* The name, what it is, and how many of its tracks the list shows. An
 * open playlist leaves its owner out: the service only names it by an
 * opaque user id (probe_meta step 15), nothing a person would recognise.
 * A search result's preview has the owner's name (pathfinder gives it):
 * "Playlist - <owner>". */
static void parse_playlist(const detail_doc *d)
{
	bool liked = strcmp(d->uri, SP_LIKED_URI) == 0;
	set_text(&texts[0], liked ? tr(STR_LIKED_SONGS)
	                    : d->title[0] ? d->title : tr(STR_UNTITLED),
	         TXT_TITULO);
	char line[192];
	if (d->preview && d->subtitle[0]) {
		snprintf(line, sizeof(line), tr(STR_RESULT_PLAYLIST_FMT),
		         d->subtitle);
		add_line(line);
	} else {
		add_line(tr(STR_PLAYLIST_TYPE));
	}
	/* The home tab's preview may not know the length (Liked Songs): no
	 * line then rather than "0 tracks". */
	if (d->total_tracks > d->count)
		snprintf(line, sizeof(line), tr(STR_ALBUM_TRACKS_CUT_FMT), d->count,
		         d->total_tracks);
	else if (d->count == 1)
		snprintf(line, sizeof(line), "%s", tr(STR_ALBUM_TRACK_ONE));
	else
		snprintf(line, sizeof(line), tr(STR_ALBUM_TRACKS_FMT), d->count);
	if (d->count > 0 || d->total_tracks > 0)
		add_line(line);
	if (d->resume[0]) {
		snprintf(line, sizeof(line), tr(STR_RESUME_FMT), d->resume);
		add_line(line);
	}
}

/* The Home tab's Downloads: how many songs and collections. */
static void parse_downloads(const detail_doc *d)
{
	set_text(&texts[0], tr(STR_SETTINGS_DOWNLOADS), TXT_TITULO);
	char line[128];
	snprintf(line, sizeof(line), tr(STR_DLH_SUMMARY_FMT), d->track_rows,
	         d->coll_rows);
	add_line(line);
}

/* An album or playlist on the card: what it is and by whom, and how many
 * songs take how much. */
static void parse_offline_collection(const detail_doc *d)
{
	set_text(&texts[0], d->title[0] ? d->title : tr(STR_UNTITLED),
	         TXT_TITULO);
	char line[192];
	const char *type = d->offline_album ? tr(STR_ALBUM_TYPE_ALBUM)
	                                    : tr(STR_PLAYLIST_TYPE);
	if (d->subtitle[0])
		snprintf(line, sizeof(line), "%s - %s", type, d->subtitle);
	else
		snprintf(line, sizeof(line), "%s", type);
	add_line(line);
	char size[32];
	dlv_format_size(d->bytes, lang_get() == LANG_ES ? ',' : '.', size,
	                sizeof(size));
	snprintf(line, sizeof(line), tr(STR_DLH_COLL_SIZE_FMT), d->total_tracks,
	         size);
	add_line(line);
}

static void parse_bio(const char *bio);

static void parse(const detail_doc *d)
{
	C2D_TextBufClear(text_buf);
	line_count = 0;
	bio_count = 0;
	if (d->kind == DETAIL_TRACK) {
		parse_track(&d->tracks[d->selected]);
	} else if (d->state == DETAIL_LOADING) {
		/* Nothing known yet but what it is about to be. */
		set_text(&texts[0], tr(STR_DETAIL_LOADING), TXT_TITULO);
	} else if (d->state == DETAIL_FAILED) {
		set_text(&texts[0], tr(STR_DETAIL_FAILED_TITLE), TXT_TITULO);
	} else if (d->kind == DETAIL_ALBUM) {
		parse_album(d);
	} else if (d->kind == DETAIL_PLAYLIST) {
		parse_playlist(d);
	} else if (d->kind == DETAIL_DOWNLOADS) {
		parse_downloads(d);
	} else if (d->kind == DETAIL_OFFLINE_COLLECTION) {
		parse_offline_collection(d);
	} else {
		set_text(&texts[0], d->title[0] ? d->title : tr(STR_UNKNOWN_ARTIST),
		    TXT_TITULO);
		/* Its monthly listeners when known; else, and in a search
		 * result's preview, what it is. "Popular tracks" heads the list
		 * below now. */
		if (!d->preview && d->listeners >= 0) {
			char count[24], line[64];
			artist_format_count(d->listeners,
			                    lang_get() == LANG_ES ? '.' : ',', count,
			                    sizeof(count));
			if (d->listeners == 1)
				snprintf(line, sizeof(line), "%s",
				         tr(STR_ARTIST_LISTENER_ONE));
			else
				snprintf(line, sizeof(line), tr(STR_ARTIST_LISTENERS_FMT),
				         count);
			add_line(line);
		} else if (d->preview || !d->bio) {
			/* Neither: the title is not left alone. */
			add_line(tr(STR_RESULT_ARTIST));
		}
		if (!d->preview && d->bio)
			parse_bio(d->bio);
	}
}

static bool changed(const detail_doc *d)
{
	return d != parsed_doc || d->serial != parsed_serial ||
	       d->state != parsed_state || d->count != parsed_count ||
	       d->listeners != parsed_listeners ||
	       parsed_lang != lang_get();
}

static float measure_meta(const char *string, void *ctx)
{
	(void)ctx;
	return ui_measure(measure_buf, string, TXT_META);
}

/* The start of a biography, word-wrapped to the column in up to
 * ARTIST_BIO_LINES lines, the last ending in "..." when there is more
 * (STYLES.md 7.12): a paragraph that rolled could not be read. */
static void parse_bio(const char *bio)
{
	static char wrapped[ARTIST_BIO_LINES][TEXT_WRAP_LINE_MAX];
	bio_count = text_wrap(bio, (float)now_text_w(), ARTIST_BIO_LINES,
	                      measure_meta, NULL, wrapped);
	for (int i = 0; i < bio_count; i++) {
		ui_parse(&bio_texts[i], text_buf, wrapped[i]);
		C2D_TextOptimize(&bio_texts[i]);
	}
}

/* --- State and drawing ------------------------------------------------- */

/* What shows where the cover goes until it comes, or when there is none:
 * the note, as everywhere; the Downloads, which has no cover, its arrow, as
 * on its tile; an artist the user, as in the list (5.3). */
static int placeholder(const detail_doc *d)
{
	return d->kind == DETAIL_DOWNLOADS ? ICON_DOWNLOAD
	     : d->kind == DETAIL_ARTIST ? ICON_USER : ICON_NOTE;
}

/* How far every text has rolled by now: -1 for one that fits. */
static void current_rolls(int *rolls)
{
	unsigned ms = (unsigned)(osGetTime() - parsed_ms);
	for (int i = 0; i < 1 + line_count; i++)
		rolls[i] = marquee_rolls(text_w[i], now_text_w())
		               ? marquee_offset(ms, text_w[i], now_text_w())
		               : -1;
}

/* --- The whole biography ---------------------------------------------- */

/* An artist's biography in full, on its own (its button, STYLES.md 7.12):
 * t_cuerpo, word-wrapped once to the page, a page of lines parsed at a
 * time as it scrolls, never the whole of it. */

/* Spotify serves biographies in English only, whatever the language
 * asked (probe_artist step 2): in any other, a line over the text says so. */
static bool bio_page_note(void)
{
	return lang_get() != LANG_EN;
}

static ui_rect bio_page_rect(void)
{
	int top = SP_3 + (bio_page_note() ? ui_line_height(TXT_META) + SP_2 : 0);
	return (ui_rect){ SP_3, top,
	                  SCREEN_TOP_W - 2 * SP_3 - SP_2 - BIO_PAGE_SCROLL_W,
	                  SCREEN_H - SP_3 - top };
}

static int bio_page_lines(void)
{
	int n = bio_page_rect().h / ui_line_height(TXT_CUERPO);
	return n > BIO_PAGE_VISIBLE_MAX ? BIO_PAGE_VISIBLE_MAX : n;
}

static bool showing_page(const detail_doc *d)
{
	return d && d->kind == DETAIL_ARTIST && d->state == DETAIL_READY &&
	       !d->preview && d->bio && d->bio_open;
}

static float measure_body(const char *string, void *ctx)
{
	(void)ctx;
	return ui_measure(measure_buf, string, TXT_CUERPO);
}

/* Wraps `d`'s biography for the page, once per document and text. */
static bool wrap_page(const detail_doc *d)
{
	if (d == page_doc && d->serial == page_serial && d->bio == page_src)
		return page_lines != NULL;
	if (!page_lines)
		page_lines = malloc(BIO_PAGE_LINES_MAX * sizeof(page_lines[0]));
	if (!page_lines)
		return false;
	page_total = text_wrap(d->bio, (float)bio_page_rect().w,
	                       BIO_PAGE_LINES_MAX, measure_body, NULL,
	                       page_lines);
	page_doc = d;
	page_serial = d->serial;
	page_src = d->bio;
	page_first = -1;
	return true;
}

int top_detail_bio_max_scroll(const detail_doc *d)
{
	if (!d || !d->bio || !wrap_page(d))
		return 0;
	int max = page_total - bio_page_lines();
	return max > 0 ? max : 0;
}

int top_detail_bio_page_lines(void)
{
	return bio_page_lines();
}

static void draw_bio_page(const detail_doc *d)
{
	if (!wrap_page(d))
		return;
	int visible = bio_page_lines();
	int max = page_total - visible > 0 ? page_total - visible : 0;
	int first = d->bio_scroll < 0 ? 0 : d->bio_scroll > max ? max
	                                                      : d->bio_scroll;
	if (first != page_first || page_lang != lang_get()) {
		C2D_TextBufClear(page_buf);
		ui_parse(&page_note, page_buf, tr(STR_BIO_ENGLISH_ONLY));
		C2D_TextOptimize(&page_note);
		page_lang = lang_get();
		page_shown = 0;
		for (int i = 0; i < visible && first + i < page_total; i++) {
			ui_parse(&page_texts[i], page_buf, page_lines[first + i]);
			C2D_TextOptimize(&page_texts[i]);
			page_shown++;
		}
		page_first = first;
	}
	ui_rect r = bio_page_rect();
	int line_h = ui_line_height(TXT_CUERPO);
	if (bio_page_note())
		ui_text(&page_note, r.x, SP_3 + ui_line_height(TXT_META) / 2,
		        TXT_META, COL_TEXTO_2);
	for (int i = 0; i < page_shown; i++)
		ui_text(&page_texts[i], r.x, r.y + i * line_h + line_h / 2,
		        TXT_CUERPO, COL_TEXTO);
	/* 5.11: only when it does not fit. */
	if (page_total > visible) {
		ui_rect track = { SCREEN_TOP_W - SP_3 - BIO_PAGE_SCROLL_W, r.y,
		                  BIO_PAGE_SCROLL_W, r.h };
		int thumb_h = r.h * visible / page_total;
		if (thumb_h < SP_3)
			thumb_h = SP_3;
		int thumb_y = r.y + (r.h - thumb_h) * first / max;
		ui_fill(track, COL_CAPA);
		ui_fill((ui_rect){ track.x, thumb_y, track.w, thumb_h },
		        COL_CLOROFILA);
	}
}

bool top_detail_dirty(const detail_doc *doc, const artwork_state *art)
{
	if (!drawn_valid || changed(doc))
		return true;
	bool page = showing_page(doc);
	if (page != drawn_page || (page && doc->bio_scroll != drawn_bio_scroll))
		return true;
	if (page)
		return false;
	if (artwork_dirty(art))
		return true;
	int rolls[1 + NOW_LINES];
	current_rolls(rolls);
	return memcmp(rolls, drawn_rolls, (1 + line_count) * sizeof(int)) != 0;
}

void top_detail_draw(const detail_doc *doc, artwork_state *art, int art_y)
{
	if (changed(doc)) {
		parse(doc);
		parsed_doc = doc;
		parsed_serial = doc->serial;
		parsed_state = doc->state;
		parsed_count = doc->count;
		parsed_listeners = doc->listeners;
		parsed_lang = lang_get();
		parsed_ms = osGetTime();
	}
	drawn_page = showing_page(doc);
	drawn_bio_scroll = doc->bio_scroll;
	if (drawn_page) {
		draw_bio_page(doc);
		drawn_valid = true;
		return;
	}
	const C2D_Text *lines[NOW_LINES];
	for (int i = 0; i < line_count; i++)
		lines[i] = &texts[1 + i];
	current_rolls(drawn_rolls);
	const C2D_Text *bio[ARTIST_BIO_LINES];
	for (int i = 0; i < bio_count; i++)
		bio[i] = &bio_texts[i];
	top_now_draw_header(art, true, art_y, &texts[0], lines, line_count,
	                    drawn_rolls, placeholder(doc), bio, bio_count);
	drawn_valid = true;
}
