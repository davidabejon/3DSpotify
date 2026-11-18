#include "top_detail.h"

#include "artist_logic.h"
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
#include <string.h>

#define TEXT_GLYPHS    ((1 + NOW_LINES) * UI_FIELD_SIZE)

static C2D_TextBuf text_buf;
/* The title, then up to NOW_LINES lines under it. */
static C2D_Text texts[1 + NOW_LINES];
static int text_w[1 + NOW_LINES];
static int line_count;
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
static lang_id parsed_lang = LANG_COUNT;
static bool drawn_valid;

bool top_detail_init(void)
{
	text_buf = C2D_TextBufNew(TEXT_GLYPHS);
	if (!text_buf) {
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

static void parse(const detail_doc *d)
{
	C2D_TextBufClear(text_buf);
	line_count = 0;
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
		} else {
			add_line(tr(STR_RESULT_ARTIST));
		}
	}
}

static bool changed(const detail_doc *d)
{
	return d != parsed_doc || d->serial != parsed_serial ||
	       d->state != parsed_state || d->count != parsed_count ||
	       parsed_lang != lang_get();
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

bool top_detail_dirty(const detail_doc *doc, const artwork_state *art)
{
	if (!drawn_valid || changed(doc) || artwork_dirty(art))
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
		parsed_lang = lang_get();
		parsed_ms = osGetTime();
	}
	const C2D_Text *lines[NOW_LINES];
	for (int i = 0; i < line_count; i++)
		lines[i] = &texts[1 + i];
	current_rolls(drawn_rolls);
	top_now_draw_header(art, true, art_y, &texts[0], lines, line_count,
	                    drawn_rolls, placeholder(doc));
	drawn_valid = true;
}
