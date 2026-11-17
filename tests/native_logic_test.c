#include "about_layout.h"
#include "art_cache_logic.h"
#include "art_url.h"
#include "detail_logic.h"
#include "detail_nav.h"
#include "downloads_logic.h"
#include "downloads_view_logic.h"
#include "offline_logic.h"
#include "grid_logic.h"
#include "home_feed_logic.h"
#include "home_logic.h"
#include "http_logic.h"
#include "image_util.h"
#include "lang.h"
#include "layout.h"
#include "list_logic.h"
#include "marquee_logic.h"
#include "login_logic.h"
#include "meta_parse.h"
#include "now_layout.h"
#include "player_logic.h"
#include "qrcodegen.h"
#include "queue_logic.h"
#include "recent_logic.h"
#include "power_logic.h"
#include "press_logic.h"
#include "search_path.h"
#include "spinner_logic.h"
#include "stream_chunks.h"
#include "spotify_id.h"
#include "settings_logic.h"
#include "tabs_logic.h"
#include "text_compose.h"
#include "text_fit.h"
#include "thumb_queue.h"
#include "theme.h"
#include "top_logic.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_search_path_encoding(void)
{
	char path[448];
	assert(sp_search_path("Beyonce Halo", path, sizeof(path)));
	assert(strcmp(path,
	              "/context-resolve/v1/spotify:search:Beyonce+Halo") == 0);
	assert(sp_search_path("rock + roll/100%", path, sizeof(path)));
	assert(strcmp(path,
	              "/context-resolve/v1/spotify:search:rock+%2B+roll%2F100%25")
	       == 0);
	assert(sp_search_path("canci\xc3\xb3n", path, sizeof(path)));
	assert(strcmp(path,
	              "/context-resolve/v1/spotify:search:canci%C3%B3n") == 0);
	assert(!sp_search_path("", path, sizeof(path)));
	assert(!sp_search_path("test", path, 4));
}

static void test_spotify_id(void)
{
	/* Real pairs, from probe_meta's log of Mr. Brightside. */
	static const struct { const char *id, *hex; } VEC[] = {
		{ "3n3Ppam7vgaVa1iaRUc9Lp", "6ecac90c03f74dcba9e21b408e14719f" },
		{ "4OHNH3sDzIxnmUADXzv2kT", "9e4c9dd5f8d641e0aaff6b2a560b45af" },
		{ "0C0XlULifJtAgn6ZNCW2eu", "142571008feb40c881095ffe591b793a" },
		{ "0000000000000000000000", "00000000000000000000000000000000" },
		{ "7N42dgm5tFLK9N8MT7fHC7", "ffffffffffffffffffffffffffffffff" },
	};
	for (size_t i = 0; i < sizeof(VEC) / sizeof(VEC[0]); i++) {
		uint8_t gid[16];
		char hex[33], id[SP_ID_LEN + 1];
		assert(sp_gid_from_id(VEC[i].id, gid));
		for (int j = 0; j < 16; j++)
			snprintf(hex + j * 2, 3, "%02x", gid[j]);
		assert(strcmp(hex, VEC[i].hex) == 0);
		sp_id_from_gid(gid, id);
		assert(strcmp(id, VEC[i].id) == 0);
	}

	/* Every byte pattern survives the round trip. */
	uint8_t gid[16], back[16];
	char id[SP_ID_LEN + 1];
	for (int seed = 0; seed < 256; seed++) {
		for (int j = 0; j < 16; j++)
			gid[j] = (uint8_t)(seed * 31 + j * 97 + (seed ^ j));
		sp_id_from_gid(gid, id);
		assert(strlen(id) == SP_ID_LEN);
		assert(sp_gid_from_id(id, back));
		assert(memcmp(gid, back, 16) == 0);
	}

	/* Wrong length, a character outside base62, more than 128 bits. */
	assert(!sp_gid_from_id(NULL, back));
	assert(!sp_gid_from_id("", back));
	assert(!sp_gid_from_id("3n3Ppam7vgaVa1iaRUc9L", back));
	assert(!sp_gid_from_id("3n3Ppam7vgaVa1iaRUc9Lpp", back));
	assert(!sp_gid_from_id("3n3Ppam7vgaVa1iaRUc9L-", back));
	assert(!sp_gid_from_id("7N42dgm5tFLK9N8MT7fHC8", back));   /* 2^128 */
	assert(!sp_gid_from_id("zzzzzzzzzzzzzzzzzzzzzz", back));
	for (int j = 0; j < 16; j++)
		assert(back[j] == 0);

	char uri[SP_URI_MAX];
	assert(sp_gid_from_id("4OHNH3sDzIxnmUADXzv2kT", gid));
	assert(sp_uri_from_gid(SP_KIND_ALBUM, gid, uri, sizeof(uri)));
	assert(strcmp(uri, "spotify:album:4OHNH3sDzIxnmUADXzv2kT") == 0);
	assert(sp_uri_from_gid(SP_KIND_TRACK, gid, uri, sizeof(uri)));
	assert(strcmp(uri, "spotify:track:4OHNH3sDzIxnmUADXzv2kT") == 0);
	/* The longest kind fits SP_URI_MAX with room to spare... */
	assert(sp_uri_from_gid(SP_KIND_ARTIST, gid, uri, sizeof(uri)));
	assert(strlen(uri) == 37);
	/* ...and a short buffer is refused, not truncated. */
	assert(!sp_uri_from_gid(SP_KIND_ARTIST, gid, uri, 37));
	assert(uri[0] == '\0');
	assert(!sp_uri_from_gid((sp_kind)7, gid, uri, sizeof(uri)));
}

/* sint32 fields go on the wire zigzag-encoded. */
static uint64_t zz(int64_t v)
{
	return ((uint64_t)v << 1) ^ (uint64_t)(v >> 63);
}

/* Distinct for every seed below 256. */
static void fake_gid(uint8_t gid[16], int seed)
{
	for (int j = 0; j < 16; j++)
		gid[j] = (uint8_t)(seed + j * 7);
}

static void add_gid(pb *b, uint32_t field, int seed)
{
	uint8_t gid[16];
	fake_gid(gid, seed);
	pb_bytes(b, field, gid, 16);
}

static void expect_uri(const char *uri, sp_kind kind, int seed)
{
	uint8_t gid[16];
	char want[SP_URI_MAX];
	fake_gid(gid, seed);
	assert(sp_uri_from_gid(kind, gid, want, sizeof(want)));
	assert(strcmp(uri, want) == 0);
}

/* An Image { file_id, size }, appended to `msg` under `field`. */
static void add_image(pb *msg, uint32_t field, uint8_t fill, int size)
{
	uint8_t store[48], fid[20];
	pb img;
	memset(fid, fill, sizeof(fid));
	pb_init(&img, store, sizeof(store));
	pb_bytes(&img, 1, fid, sizeof(fid));
	pb_varint(&img, 2, (uint64_t)size);
	pb_sub(msg, field, &img);
}

/* A message holding only a gid: how Disc and TopTracks list their tracks. */
static void add_gid_only(pb *msg, uint32_t field, int seed)
{
	uint8_t store[24];
	pb tr;
	pb_init(&tr, store, sizeof(store));
	add_gid(&tr, 1, seed);
	pb_sub(msg, field, &tr);
}

static void test_meta_parse_track(void)
{
	static uint8_t date_s[16], group_s[128], album_s[512], art1_s[64],
	               art2_s[64], file_s[2][48], track_s[1024];
	pb date, group, album, art1, art2, file[2], track;

	pb_init(&date, date_s, sizeof(date_s));
	pb_varint(&date, 1, zz(2004));
	pb_varint(&date, 2, zz(6));
	pb_init(&group, group_s, sizeof(group_s));
	add_image(&group, 1, 0xab, 1);
	add_image(&group, 1, 0xcd, 2);
	pb_init(&album, album_s, sizeof(album_s));
	add_gid(&album, 1, 2);
	pb_str(&album, 2, "Hot Fuss");
	pb_varint(&album, 4, 1);
	pb_str(&album, 5, "EMI");
	pb_sub(&album, 6, &date);
	pb_sub(&album, 17, &group);
	pb_init(&art1, art1_s, sizeof(art1_s));
	add_gid(&art1, 1, 3);
	pb_str(&art1, 2, "The Killers");
	pb_init(&art2, art2_s, sizeof(art2_s));
	add_gid(&art2, 1, 4);
	pb_str(&art2, 2, "Someone Else");

	pb_init(&track, track_s, sizeof(track_s));
	add_gid(&track, 1, 1);
	pb_str(&track, 2, "Mr. Brightside");
	pb_sub(&track, 3, &album);
	pb_sub(&track, 4, &art1);
	pb_sub(&track, 4, &art2);
	pb_varint(&track, 5, zz(2));
	pb_varint(&track, 6, zz(1));
	pb_varint(&track, 7, zz(222200));
	for (int i = 0; i < 2; i++) {
		uint8_t fid[20];
		memset(fid, 0x10 + i, sizeof(fid));
		pb_init(&file[i], file_s[i], sizeof(file_s[i]));
		pb_bytes(&file[i], 1, fid, sizeof(fid));
		pb_varint(&file[i], 2,
		          (uint64_t)(i == 0 ? SP_FMT_OGG_320 : SP_FMT_OGG_160));
		pb_sub(&track, 12, &file[i]);
	}
	assert(pb_ok(&track));

	static sp_track t;
	memset(&t, 0, sizeof(t));
	pbv tv = { track.p, track.len };
	sp_parse_track(&tv, &t);
	assert(t.has_gid && t.gid[0] == 1 && t.gid[15] == 1 + 15 * 7);
	assert(strcmp(t.name, "Mr. Brightside") == 0);
	assert(strcmp(t.album, "Hot Fuss") == 0);
	assert(strcmp(t.artist, "The Killers") == 0);
	expect_uri(t.album_uri, SP_KIND_ALBUM, 2);
	expect_uri(t.artist_uri, SP_KIND_ARTIST, 3);
	/* Read as plain varints these would be 4, 2 and 444400. */
	assert(t.track_number == 2 && t.disc_number == 1);
	assert(t.duration_ms == 222200);
	assert(t.year == 2004);
	assert(!t.is_explicit);              /* absent on the wire when false */
	assert(t.uri[0] == '\0');            /* left to the caller */
	assert(strcmp(t.art_small, "https://i.scdn.co/image/"
	              "abababababababababababababababababababab") == 0);
	assert(strcmp(t.art_large, "https://i.scdn.co/image/"
	              "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd") == 0);
	assert(t.file_count == 2);
	assert(t.files[0].format == SP_FMT_OGG_320 && t.files[0].id[0] == 0x10);
	assert(t.files[1].format == SP_FMT_OGG_160 && t.files[1].id[0] == 0x11);

	/* Through the extended-metadata envelope, as the service sends it. */
	static uint8_t any_s[1152], ed_s[1216], arr_s[1280], body_s[1344];
	pb any, ed, arr, body;
	pb_init(&any, any_s, sizeof(any_s));
	pb_str(&any, 1, "type.googleapis.com/spotify.metadata.Track");
	pb_sub(&any, 2, &track);
	pb_init(&ed, ed_s, sizeof(ed_s));
	pb_str(&ed, 2, "spotify:track:x");
	pb_sub(&ed, 3, &any);
	pb_init(&arr, arr_s, sizeof(arr_s));
	pb_varint(&arr, 1, EXT_KIND_TRACK_V4);
	pb_sub(&arr, 3, &ed);
	pb_init(&body, body_s, sizeof(body_s));
	pb_sub(&body, 2, &arr);
	assert(pb_ok(&body));
	pbv entity;
	assert(sp_parse_entity(body.p, body.len, &entity));
	assert(entity.len == track.len &&
	       memcmp(entity.p, track.p, track.len) == 0);
	assert(!sp_parse_entity(track.p, 0, &entity));

	/* Explicit, and a track with no album or artist at all. */
	pb_init(&track, track_s, sizeof(track_s));
	add_gid(&track, 1, 1);
	pb_str(&track, 2, "Bare");
	pb_varint(&track, 9, 1);
	memset(&t, 0, sizeof(t));
	tv.p = track.p;
	tv.len = track.len;
	sp_parse_track(&tv, &t);
	assert(t.is_explicit);
	assert(!t.album_uri[0] && !t.artist_uri[0] && !t.art_small[0]);
	assert(t.year == 0 && t.track_number == 0 && t.file_count == 0);
	assert(t.audio_gid[0] == 1 && t.audio_gid[15] == 1 + 15 * 7);

	/* No files of its own: they come from the first alternative that has
	 * any, and the key must then name that alternative. */
	static uint8_t alt_s[2][256];
	pb alt[2];
	pb_init(&alt[0], alt_s[0], sizeof(alt_s[0]));
	add_gid(&alt[0], 1, 5);                   /* an alternative with none */
	pb_init(&alt[1], alt_s[1], sizeof(alt_s[1]));
	add_gid(&alt[1], 1, 6);
	pb_sub(&alt[1], 12, &file[0]);
	pb_sub(&alt[1], 12, &file[1]);
	pb_init(&track, track_s, sizeof(track_s));
	add_gid(&track, 1, 1);
	pb_str(&track, 2, "Only alternatives");
	pb_sub(&track, 13, &alt[0]);
	pb_sub(&track, 13, &alt[1]);
	assert(pb_ok(&track));
	memset(&t, 0, sizeof(t));
	tv.p = track.p;
	tv.len = track.len;
	sp_parse_track(&tv, &t);
	assert(t.gid[0] == 1);                    /* still the track asked for */
	assert(t.audio_gid[0] == 6);
	assert(t.file_count == 2);
	assert(t.files[1].format == SP_FMT_OGG_160 && t.files[1].id[0] == 0x11);

	/* Files of its own win over an alternative's. */
	pb_sub(&track, 12, &file[0]);
	memset(&t, 0, sizeof(t));
	tv.p = track.p;
	tv.len = track.len;
	sp_parse_track(&tv, &t);
	assert(t.file_count == 1 && t.audio_gid[0] == 1);
}

static void test_meta_parse_album(void)
{
	static uint8_t date_s[16], art_s[64], disc_s[2][1024], group_s[64],
	               album_s[4096];
	pb date, art, disc[2], group, album;

	pb_init(&date, date_s, sizeof(date_s));
	pb_varint(&date, 1, zz(2004));
	pb_init(&art, art_s, sizeof(art_s));
	add_gid(&art, 1, 3);
	pb_str(&art, 2, "The Killers");
	pb_init(&group, group_s, sizeof(group_s));
	add_image(&group, 1, 0x01, 0);   /* only DEFAULT: both sizes use it */

	/* Two discs, 20 + 10 tracks: more than SP_ALBUM_MAX_TRACKS. */
	int seed = 100;
	for (int d = 0; d < 2; d++) {
		pb_init(&disc[d], disc_s[d], sizeof(disc_s[d]));
		pb_varint(&disc[d], 1, zz(d + 1));
		for (int i = 0; i < (d == 0 ? 20 : 10); i++)
			add_gid_only(&disc[d], 3, seed++);
	}

	pb_init(&album, album_s, sizeof(album_s));
	add_gid(&album, 1, 2);
	pb_str(&album, 2, "Hot Fuss");
	pb_sub(&album, 3, &art);
	pb_varint(&album, 4, SP_ALBUM_EP);
	pb_str(&album, 5, "EMI");
	pb_sub(&album, 6, &date);
	pb_sub(&album, 11, &disc[0]);
	pb_sub(&album, 11, &disc[1]);
	pb_sub(&album, 17, &group);
	assert(pb_ok(&album));

	static sp_album a;
	pbv av = { album.p, album.len };
	assert(sp_parse_album(&av, &a));
	expect_uri(a.uri, SP_KIND_ALBUM, 2);
	assert(strcmp(a.name, "Hot Fuss") == 0);
	assert(strcmp(a.artist, "The Killers") == 0);
	expect_uri(a.artist_uri, SP_KIND_ARTIST, 3);
	assert(strcmp(a.label, "EMI") == 0);
	assert(a.year == 2004 && a.type == SP_ALBUM_EP);
	assert(a.art_small[0] && strcmp(a.art_small, a.art_large) == 0);
	assert(a.total_tracks == 30);
	assert(a.track_count == SP_ALBUM_MAX_TRACKS);
	/* Disc order, numbered straight through: the 21st is disc 2's first. */
	expect_uri(a.track_uris[0], SP_KIND_TRACK, 100);
	expect_uri(a.track_uris[20], SP_KIND_TRACK, 120);
	expect_uri(a.track_uris[23], SP_KIND_TRACK, 123);

	/* No gid: refused, and nothing left over from the last parse. */
	pb_init(&album, album_s, sizeof(album_s));
	pb_str(&album, 2, "Nameless");
	av.p = album.p;
	av.len = album.len;
	assert(!sp_parse_album(&av, &a));
	assert(a.name[0] == '\0' && a.track_count == 0);
}

/* One rootlist entry: an Item with `uri` and its MetaItem, built into the
 * caller's ListItems. `pic` adds a 20-byte picture id (all bytes `pic`). */
static void add_root_entry(pb *items, const char *uri, const char *name,
                           int pic, const char *sized_url, int length)
{
	uint8_t item_s[96], attrs_s[256], size_s[160], meta_s[320];
	pb item, attrs, size, meta;
	pb_init(&item, item_s, sizeof(item_s));
	pb_str(&item, 1, uri);
	pb_init(&attrs, attrs_s, sizeof(attrs_s));
	if (name)
		pb_str(&attrs, 1, name);
	if (pic) {
		uint8_t id[20];
		memset(id, pic, sizeof(id));
		pb_bytes(&attrs, 3, id, sizeof(id));
	}
	if (sized_url) {
		pb_init(&size, size_s, sizeof(size_s));
		pb_str(&size, 1, "default");
		pb_str(&size, 2, sized_url);
		pb_sub(&attrs, 13, &size);
	}
	pb_init(&meta, meta_s, sizeof(meta_s));
	pb_sub(&meta, 2, &attrs);
	if (length)
		pb_varint(&meta, 3, (uint64_t)length);
	pb_sub(items, 3, &item);
	pb_sub(items, 4, &meta);
}

static void test_meta_parse_rootlist(void)
{
	static uint8_t items_s[4096], body_s[4200];
	pb items, body;
	pb_init(&items, items_s, sizeof(items_s));
	add_root_entry(&items, "spotify:playlist:7ccWAm7GdeAUpA2Eu9YI5q", "relax",
	               0xAB, NULL, 32);
	/* A folder: its markers go, the playlist inside stays. */
	add_root_entry(&items, "spotify:start-group:abc:Folder", "Folder", 0,
	               NULL, 0);
	add_root_entry(&items, "spotify:playlist:4FScOx76dqrpgQbhk258xR",
	               "Alterna2026", 0, NULL, 132);
	add_root_entry(&items, "spotify:end-group:abc", NULL, 0, NULL, 0);
	/* A generated cover off the reachable CDN reads as none... */
	add_root_entry(&items, "spotify:playlist:37i9dQZF1EJDBB41SW1yDD",
	               "Blend", 0,
	               "https://blend-playlist-covers.spotifycdn.com/v2/x.jpg", 50);
	/* ...one on i.scdn.co is kept; no name stays empty. */
	add_root_entry(&items, "spotify:playlist:5pi5MhptTMnmV8L8Q6cJv1", NULL,
	               0, "https://i.scdn.co/image/ab67706c0000da84", 0);
	add_root_entry(&items, "spotify:album:2noRn2Aes5aoNVsU6iWThc", "Album", 0,
	               NULL, 0);
	pb_init(&body, body_s, sizeof(body_s));
	pb_varint(&body, 2, 7);
	pb_sub(&body, 5, &items);
	assert(pb_ok(&body));

	static sp_playlist_ref refs[8];
	int total = -1;
	int n = sp_parse_rootlist(body.p, body.len, refs, 8, &total);
	assert(total == 7);
	assert(n == 4);
	assert(strcmp(refs[0].uri, "spotify:playlist:7ccWAm7GdeAUpA2Eu9YI5q") == 0);
	assert(strcmp(refs[0].name, "relax") == 0 && refs[0].length == 32);
	assert(strcmp(refs[0].art, "https://i.scdn.co/image/"
	              "abababababababababababababababababababab") == 0);
	assert(strcmp(refs[1].name, "Alterna2026") == 0 && !refs[1].art[0]);
	assert(refs[1].length == 132);
	assert(strcmp(refs[2].name, "Blend") == 0 && !refs[2].art[0]);
	assert(!refs[3].name[0]);
	assert(strcmp(refs[3].art, "https://i.scdn.co/image/ab67706c0000da84") == 0);

	/* `max` bounds what is kept. */
	assert(sp_parse_rootlist(body.p, body.len, refs, 2, NULL) == 2);
	assert(strcmp(refs[1].name, "Alterna2026") == 0);
	/* An empty folder is a valid answer. */
	pb_init(&body, body_s, sizeof(body_s));
	pb_varint(&body, 2, 0);
	assert(sp_parse_rootlist(body.p, body.len, refs, 8, &total) == 0);
	assert(total == 0);

	/* A cut name stays whole UTF-8: "aé" cut to 2 bytes keeps only "a". */
	static uint8_t attrs_s[64];
	pb attrs;
	pb_init(&attrs, attrs_s, sizeof(attrs_s));
	pb_str(&attrs, 1, "a\xC3\xA9");
	pbv av = { attrs.p, attrs.len };
	char small[3];
	sp_parse_list_attributes(&av, small, sizeof(small), NULL, 0);
	assert(strcmp(small, "a") == 0);
}

static void test_meta_parse_rootlist_live(void)
{
	static uint8_t items_s[1024], body_s[1100], fa_s[64], attrs_s[128],
	               meta_s[160], item_s[64];
	pb items, body, fa, attrs, meta, item;
	pb_init(&items, items_s, sizeof(items_s));
	add_root_entry(&items, "spotify:playlist:7ccWAm7GdeAUpA2Eu9YI5q", "relax",
	               0, NULL, 32);
	/* The DJ, as the rootlist decorates it. */
	pb_init(&item, item_s, sizeof(item_s));
	pb_str(&item, 1, "spotify:playlist:37i9dQZF1EYkqdzj48dyYq");
	pb_init(&fa, fa_s, sizeof(fa_s));
	pb_str(&fa, 1, "lexicon_context_url");
	pb_str(&fa, 2, "hm://lexicon-session-provider/x");
	pb_init(&attrs, attrs_s, sizeof(attrs_s));
	pb_str(&attrs, 1, "DJ");
	pb_sub(&attrs, 12, &fa);
	pb_init(&meta, meta_s, sizeof(meta_s));
	pb_sub(&meta, 2, &attrs);
	pb_sub(&items, 3, &item);
	pb_sub(&items, 4, &meta);
	add_root_entry(&items, "spotify:playlist:4FScOx76dqrpgQbhk258xR",
	               "Alterna2026", 0, NULL, 132);
	pb_init(&body, body_s, sizeof(body_s));
	pb_varint(&body, 2, 3);
	pb_sub(&body, 5, &items);
	assert(pb_ok(&body));

	static sp_playlist_ref refs[4];
	int n = sp_parse_rootlist(body.p, body.len, refs, 4, NULL);
	assert(n == 2);
	assert(strcmp(refs[0].name, "relax") == 0);
	/* The entry after it still reads its own meta_item. */
	assert(strcmp(refs[1].name, "Alterna2026") == 0 && refs[1].length == 132);
}

static void test_list_window(void)
{
	/* A list that fits is its own window. */
	assert(list_window_base(0, 0, 24, 24) == 0);
	assert(list_window_base(5, 19, 24, 24) == 0);
	/* Rows 0-17 on screen leave the window at 0; 18 moves it, centred. */
	assert(list_window_base(0, 17, 200, 24) == 0);
	assert(list_window_base(0, 18, 200, 24) == 9);
	/* The rows on screen (5, the peek row included) are always inside. */
	int base = 0;
	for (int first = 0; first <= 195; first++) {
		base = list_window_base(base, first, 200, 24);
		assert(first >= base && first + 5 <= base + 24);
	}
	for (int first = 195; first >= 0; first--) {
		base = list_window_base(base, first, 200, 24);
		assert(first >= base && first + 5 <= base + 24);
	}
	assert(base == 0);
	/* Never past the end, and a window left past a shorter list comes
	 * back inside it. */
	assert(list_window_base(0, 195, 200, 24) == 176);
	assert(list_window_base(150, 10, 60, 24) == 1);
}

static void test_detail_paging(void)
{
	/* 50 loaded, 4 rows on screen, 8 ahead. */
	assert(!detail_wants_more(0, 0, 4, 50, 8));
	assert(!detail_wants_more(37, 37, 4, 50, 8));    /* last shown 40 */
	assert(detail_wants_more(38, 38, 4, 50, 8));     /* last shown 41 */
	/* The focus counts even when a drag left it below the rows shown. */
	assert(detail_wants_more(0, 45, 4, 50, 8));
	/* Nothing loaded yet (a first page of episodes only): ask. */
	assert(detail_wants_more(0, 0, 4, 0, 8));
}

static void test_thumb_reuse(void)
{
	const char *old_urls[4] = { "a", "b", "c", "d" };
	thumb_state old_states[4] = { THUMB_READY, THUMB_READY, THUMB_DECODED,
	                              THUMB_READY };
	int src[4];
	bool kept[4];
	/* The window moved by two: c and d come forward with their textures,
	 * except c, still waiting for its upload in its own staging buffer. */
	const char *moved[4] = { "c", "d", "e", "f" };
	thumb_reuse_plan(old_urls, old_states, 4, moved, 4, 4, src, kept);
	assert(src[1] == 3 && kept[1]);
	assert(!kept[0] && !kept[2] && !kept[3]);
	/* Every texture goes somewhere, once. */
	bool seen[4] = { false };
	for (int i = 0; i < 4; i++) {
		assert(src[i] >= 0 && src[i] < 4 && !seen[src[i]]);
		seen[src[i]] = true;
	}
	/* Unchanged positions keep their own, a decoded one included. */
	thumb_reuse_plan(old_urls, old_states, 4, old_urls, 4, 4, src, kept);
	for (int i = 0; i < 4; i++)
		assert(src[i] == i && kept[i]);
	/* A shorter list: the slots past it keep nothing. */
	const char *two[2] = { "d", "a" };
	thumb_reuse_plan(old_urls, old_states, 4, two, 2, 4, src, kept);
	assert(src[0] == 3 && kept[0] && src[1] == 0 && kept[1]);
	assert(!kept[2] && !kept[3]);
}

static void test_meta_parse_playlist(void)
{
	static uint8_t items_s[4096], attrs_s[128], body_s[4400];
	pb items, attrs, body;
	pb_init(&items, items_s, sizeof(items_s));
	char uri[SP_URI_MAX];
	for (int i = 0; i < 30; i++) {
		uint8_t item_s[96];
		pb item;
		pb_init(&item, item_s, sizeof(item_s));
		if (i == 1)
			pb_str(&item, 1, "spotify:episode:4rOoJ6Egrf8K2IrywzwOMk");
		else if (i == 2)
			pb_str(&item, 1, "spotify:local:Artist:Album:A+very+long+title:200");
		else {
			snprintf(uri, sizeof(uri), "spotify:track:%022d", i);
			pb_str(&item, 1, uri);
		}
		pb_sub(&items, 3, &item);
	}
	pb_init(&attrs, attrs_s, sizeof(attrs_s));
	pb_str(&attrs, 1, "TWICE Relaxing Songs");
	pb_init(&body, body_s, sizeof(body_s));
	pb_varint(&body, 2, 1275);
	pb_sub(&body, 3, &attrs);
	pb_sub(&body, 5, &items);
	assert(pb_ok(&body));

	static sp_playlist_info pl;
	const char *pl_uri = "spotify:playlist:5pi5MhptTMnmV8L8Q6cJv1";
	sp_parse_playlist(body.p, body.len, pl_uri, 99, &pl);
	assert(strcmp(pl.uri, pl_uri) == 0);
	assert(strcmp(pl.name, "TWICE Relaxing Songs") == 0 && !pl.art[0]);
	assert(pl.total_tracks == 1275);
	/* A page of 30 items: 28 tracks, an episode and a local file. */
	assert(pl.skipped == 2 && pl.items == 30 && !pl.live);
	assert(pl.track_count == 28);
	assert(strcmp(pl.track_uris[0], "spotify:track:0000000000000000000000") == 0);
	assert(strcmp(pl.track_uris[1], "spotify:track:0000000000000000000003") == 0);
	for (int i = 0; i < pl.track_count; i++)
		assert(sp_is_track_uri(pl.track_uris[i]));

	/* A full page stops reading: `items` says where the next one starts,
	 * past the two skipped items it went through. */
	sp_parse_playlist(body.p, body.len, pl_uri, 5, &pl);
	assert(pl.track_count == 5 && pl.items == 7 && pl.skipped == 2);
	assert(strcmp(pl.track_uris[4], "spotify:track:0000000000000000000006") == 0);

	/* A header alone: nothing read, the total still known. */
	sp_parse_playlist(body.p, body.len, pl_uri, 0, &pl);
	assert(pl.track_count == 0 && pl.items == 0 && pl.total_tracks == 1275);

	/* The DJ: lexicon format attributes and no items. */
	static uint8_t fa_s[64], dj_attrs_s[160], dj_s[200];
	pb fa, dj_attrs, dj;
	pb_init(&fa, fa_s, sizeof(fa_s));
	pb_str(&fa, 1, "lexicon_set_type");
	pb_str(&fa, 2, "your_dj");
	pb_init(&dj_attrs, dj_attrs_s, sizeof(dj_attrs_s));
	pb_str(&dj_attrs, 1, "DJ");
	pb_sub(&dj_attrs, 12, &fa);
	pb_init(&dj, dj_s, sizeof(dj_s));
	pb_sub(&dj, 3, &dj_attrs);
	assert(pb_ok(&dj));
	sp_parse_playlist(dj.p, dj.len, "spotify:playlist:37i9dQZF1EYkqdzj48dyYq",
	                  SP_PLAYLIST_PAGE, &pl);
	assert(pl.live && pl.track_count == 0 && strcmp(pl.name, "DJ") == 0);
	pbv djv = { dj_attrs.p, dj_attrs.len };
	assert(sp_list_is_live(&djv));
	/* Other format attributes (a blend's) are not a session. */
	pb_init(&fa, fa_s, sizeof(fa_s));
	pb_str(&fa, 1, "madeFor.username");
	pb_str(&fa, 2, "someone");
	pb_init(&dj_attrs, dj_attrs_s, sizeof(dj_attrs_s));
	pb_sub(&dj_attrs, 12, &fa);
	djv.p = dj_attrs.p;
	djv.len = dj_attrs.len;
	assert(!sp_list_is_live(&djv));

	assert(sp_is_track_uri("spotify:track:3n3Ppam7vgaVa1iaRUc9Lp"));
	assert(!sp_is_track_uri("spotify:track:"));
	assert(!sp_is_track_uri("spotify:episode:4rOoJ6Egrf8K2IrywzwOMk"));
	assert(!sp_is_track_uri(NULL));
}

static void test_meta_parse_recent(void)
{
	/* Trimmed from the real answer (probe_meta step 15.2). */
	static const char JSON[] =
	    "{\"playContexts\":["
	    "{\"uri\":\"spotify:playlist:37i9dQZF1EYkqdzj48dyYq\","
	    "\"lastPlayedTime\":1794727708085,"
	    "\"lastPlayedTrackUri\":\"spotify:track:6EGkbPJrb88STcdfMu1j1y\"},"
	    "{\"uri\":\"spotify:album:2noRn2Aes5aoNVsU6iWThc\","
	    "\"lastPlayedTime\":1794529389812},"
	    "{\"uri\": \"spotify:playlist:5pi5MhptTMnmV8L8Q6cJv1\","
	    "\"lastPlayedTime\":1794529389811},"
	    "{\"uri\":\"spotify:playlist:37i9dQZF1EYkqdzj48dyYq\","
	    "\"lastPlayedTime\":1794500000000},"
	    "{\"uri\":\"spotify:playlist:4kdfYi4mA3WKP0fqpvY3bD\"}"
	    "]}";
	char uris[4][SP_URI_MAX], last[4][SP_URI_MAX];
	int n = sp_parse_recent_playlists(JSON, strlen(JSON), uris, last, 4);
	assert(n == 3);
	/* Each playlist's last track, from its own object only. */
	assert(strcmp(last[0], "spotify:track:6EGkbPJrb88STcdfMu1j1y") == 0);
	assert(!last[1][0] && !last[2][0]);
	assert(strcmp(uris[0], "spotify:playlist:37i9dQZF1EYkqdzj48dyYq") == 0);
	assert(strcmp(uris[1], "spotify:playlist:5pi5MhptTMnmV8L8Q6cJv1") == 0);
	assert(strcmp(uris[2], "spotify:playlist:4kdfYi4mA3WKP0fqpvY3bD") == 0);
	assert(sp_parse_recent_playlists(JSON, strlen(JSON), uris, NULL, 1) == 1);
	/* Cut short: a value that ends before its quote is dropped, one that
	 * closed before the cut is kept. */
	assert(sp_parse_recent_playlists(JSON, 60, uris, NULL, 4) == 0);
	assert(sp_parse_recent_playlists(JSON, 70, uris, NULL, 4) == 1);
	assert(sp_parse_recent_playlists("{}", 2, uris, NULL, 4) == 0);

	/* Liked Songs, trimmed from the real answer (probe_meta step 16). */
	static const char LIKED[] =
	    "{\"metadata\":{\"context_owner\":\"spotify:user:abc\"},"
	    "\"pages\":[{\"tracks\":["
	    "{\"uri\":\"spotify:track:1BLOVHYYlH4JUHQGcpt75R\","
	    "\"metadata\":{\"added_at\":\"1790203910000\"}},"
	    "{\"uri\":\"spotify:track:2to8o6eQqWD4wBjMIvizGW\"},"
	    "{\"uri\":\"spotify:track:3Y4rUyw7XBCK6hGHCOt6rp\"}]}],"
	    "\"uri\":\"spotify:user:abc:collection\"}";
	char liked[2][SP_URI_MAX];
	int total = 0;
	assert(sp_parse_liked(LIKED, strlen(LIKED), liked, 2, &total) == 2);
	assert(total == 3);
	assert(strcmp(liked[0], "spotify:track:1BLOVHYYlH4JUHQGcpt75R") == 0);
	assert(strcmp(liked[1], "spotify:track:2to8o6eQqWD4wBjMIvizGW") == 0);
	assert(sp_parse_liked("{}", 2, liked, 2, &total) == 0 && total == 0);
}

static void set_ref(sp_playlist_ref *r, const char *uri, const char *name)
{
	memset(r, 0, sizeof(*r));
	snprintf(r->uri, sizeof(r->uri), "%s", uri);
	snprintf(r->name, sizeof(r->name), "%s", name);
}

static void test_home_merge(void)
{
	static sp_playlist_ref root[4], out[8];
	bool missing[8];
	set_ref(&root[0], "spotify:playlist:A", "a");
	set_ref(&root[1], "spotify:playlist:B", "b");
	set_ref(&root[2], "spotify:playlist:C", "c");
	set_ref(&root[3], "spotify:playlist:D", "d");
	/* History: C, X (not in the folder), C again, A. */
	static const char recent[4][SP_URI_MAX] = {
		"spotify:playlist:C", "spotify:playlist:X", "spotify:playlist:C",
		"spotify:playlist:A",
	};
	int total = 0;
	int n = home_merge(recent, 4, root, 4, 4, out, missing, 8, &total);
	assert(n == 5 && total == 5);
	/* Recent first, named from the folder; then the rest in its order. */
	assert(strcmp(out[0].uri, "spotify:playlist:C") == 0 && !missing[0]);
	assert(strcmp(out[0].name, "c") == 0);
	assert(strcmp(out[1].uri, "spotify:playlist:X") == 0 && missing[1]);
	assert(!out[1].name[0]);
	assert(strcmp(out[2].uri, "spotify:playlist:A") == 0);
	assert(strcmp(out[3].uri, "spotify:playlist:B") == 0);
	assert(strcmp(out[4].uri, "spotify:playlist:D") == 0 && !missing[4]);

	/* Capped: the total still counts everything, folder entries that were
	 * never fetched included. */
	n = home_merge(recent, 4, root, 4, 30, out, missing, 2, &total);
	assert(n == 2 && total == 5 + 26);
	/* No history: the folder as it is. */
	n = home_merge(NULL, 0, root, 4, 4, out, missing, 8, &total);
	assert(n == 4 && strcmp(out[0].uri, "spotify:playlist:A") == 0);
	/* No folder: the history alone. */
	n = home_merge(recent, 4, root, 0, 0, out, missing, 8, &total);
	assert(n == 3 && missing[0] && missing[1] && missing[2]);

	/* A missing playlist whose header failed goes; the rest close ranks. */
	n = home_merge(recent, 4, root, 4, 4, out, missing, 8, &total);
	n = home_drop(out, missing, n, 1);
	assert(n == 4 && strcmp(out[1].uri, "spotify:playlist:A") == 0);
	assert(!missing[1]);
	assert(home_drop(out, missing, n, 9) == n);
}

static void test_meta_parse_artist(void)
{
	static uint8_t top_s[2][256], artist_s[1024];
	pb top[2], artist;

	/* Two groups (the service sends one, but the parser must choose). */
	static const char *const COUNTRY[] = { "US", "ES" };
	int seed = 200;
	for (int g = 0; g < 2; g++) {
		pb_init(&top[g], top_s[g], sizeof(top_s[g]));
		pb_str(&top[g], 1, COUNTRY[g]);
		for (int i = 0; i < 2 + g; i++)
			add_gid_only(&top[g], 2, seed++);
	}
	pb_init(&artist, artist_s, sizeof(artist_s));
	add_gid(&artist, 1, 3);
	pb_str(&artist, 2, "The Killers");
	pb_sub(&artist, 4, &top[0]);
	pb_sub(&artist, 4, &top[1]);
	/* Older metadata: portrait images straight on the artist. */
	add_image(&artist, 11, 0xee, 1);
	assert(pb_ok(&artist));

	static sp_artist a;
	pbv av = { artist.p, artist.len };
	assert(sp_parse_artist(&av, "ES", &a));
	expect_uri(a.uri, SP_KIND_ARTIST, 3);
	assert(strcmp(a.name, "The Killers") == 0);
	assert(strcmp(a.art_small, "https://i.scdn.co/image/"
	              "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee") == 0);
	assert(strcmp(a.art_small, a.art_large) == 0);
	assert(a.track_count == 3);
	expect_uri(a.track_uris[0], SP_KIND_TRACK, 202);

	/* No country, or one that does not match: the first group. */
	assert(sp_parse_artist(&av, NULL, &a) && a.track_count == 2);
	expect_uri(a.track_uris[1], SP_KIND_TRACK, 201);
	assert(sp_parse_artist(&av, "", &a) && a.track_count == 2);
	assert(sp_parse_artist(&av, "E", &a) && a.track_count == 2);
	assert(sp_parse_artist(&av, "FR", &a) && a.track_count == 2);
}

static void test_detail_stack(void)
{
	assert(detail_tab_stack(TAB_SEARCH) == 0);
	assert(detail_tab_stack(TAB_PLAYER) == 1);
	assert(detail_tab_stack(TAB_HOME) == 2);
	assert(detail_tab_stack(TAB_SETTINGS) < 0);

	detail_stack s;
	detail_stack_clear(&s);
	assert(detail_stack_top(&s) == -1);
	assert(detail_stack_pop(&s) == -1);
	assert(detail_stack_drop_oldest(&s) == -1);

	/* Up to the depth nothing is dropped. */
	for (int i = 0; i < DETAIL_DEPTH; i++) {
		assert(detail_stack_push(&s, 10 + i) == -1);
		assert(detail_stack_top(&s) == 10 + i);
	}
	assert(s.count == DETAIL_DEPTH);
	/* Past it the oldest goes, and the order of the rest holds. */
	assert(detail_stack_push(&s, 99) == 10);
	assert(s.count == DETAIL_DEPTH);
	assert(s.slot[0] == 11 && detail_stack_top(&s) == 99);
	assert(detail_stack_holds(&s, 99) && !detail_stack_holds(&s, 10));

	/* B pops back down to the tab's own view. */
	assert(detail_stack_pop(&s) == 99);
	for (int i = DETAIL_DEPTH - 1; i >= 1; i--)
		assert(detail_stack_pop(&s) == 10 + i);
	assert(s.count == 0 && detail_stack_top(&s) == -1);
}

static void test_detail_buttons(void)
{
	/* The Settings geometry: four full-width buttons of TOUCH_MIN under
	 * the tab bar, SP_3 apart, clear of the button bar. */
	assert(DETAIL_BUTTON_COUNT == 4);
	for (int i = 0; i < DETAIL_BUTTON_COUNT; i++) {
		ui_rect r = detail_button_rect(i);
		ui_rect s = settings_item_rect(i);
		assert(r.x == s.x && r.y == s.y && r.w == s.w && r.h == s.h);
		assert(r.h >= TOUCH_MIN);
		/* Its shadow, or its pressed shift, clear of the button bar. */
		assert(r.y + r.h + SHADOW_OFF + PRESS_SHIFT <= detail_hints_rect().y);
		if (i > 0)
			assert(r.y - (detail_button_rect(i - 1).y +
			              detail_button_rect(i - 1).h) == SP_3);
	}
	assert(detail_button_rect(0).y == 44);
	assert(detail_button_rect(DETAIL_BUTTON_DOWNLOAD).y == 88);
	assert(detail_button_rect(DETAIL_BUTTON_ALBUM).y == 176);

	bool all[DETAIL_BUTTON_COUNT] = { true, true, true, true };
	bool no_artist[DETAIL_BUTTON_COUNT] = { true, true, false, true };
	bool only_play[DETAIL_BUTTON_COUNT] = { true, false, false, false };
	/* A download queued or under way: its button is disabled. */
	bool queued[DETAIL_BUTTON_COUNT] = { true, false, true, true };
	ui_rect artist = detail_button_rect(DETAIL_BUTTON_ARTIST);
	int ax = artist.x + artist.w / 2, ay = artist.y + artist.h / 2;
	ui_rect save = detail_button_rect(DETAIL_BUTTON_DOWNLOAD);
	int sx = save.x + save.w / 2, sy = save.y + save.h / 2;

	/* A disabled button does not answer the stylus (6.1). */
	assert(detail_hit_test(ax, ay, all) == DETAIL_BUTTON_ARTIST);
	assert(detail_hit_test(ax, ay, no_artist) == DETAIL_BUTTON_NONE);
	assert(detail_hit_test(sx, sy, all) == DETAIL_BUTTON_DOWNLOAD);
	assert(detail_hit_test(sx, sy, queued) == DETAIL_BUTTON_NONE);
	assert(detail_hit_test(0, 0, all) == DETAIL_BUTTON_NONE);

	/* The D-pad stops at the ends and steps over disabled buttons. */
	assert(detail_focus_move(DETAIL_BUTTON_PLAY, -1, all) ==
	       DETAIL_BUTTON_PLAY);
	assert(detail_focus_move(DETAIL_BUTTON_PLAY, 1, all) ==
	       DETAIL_BUTTON_DOWNLOAD);
	assert(detail_focus_move(DETAIL_BUTTON_DOWNLOAD, 1, all) ==
	       DETAIL_BUTTON_ARTIST);
	assert(detail_focus_move(DETAIL_BUTTON_ALBUM, 1, all) ==
	       DETAIL_BUTTON_ALBUM);
	assert(detail_focus_move(DETAIL_BUTTON_DOWNLOAD, 1, no_artist) ==
	       DETAIL_BUTTON_ALBUM);
	assert(detail_focus_move(DETAIL_BUTTON_ALBUM, -1, no_artist) ==
	       DETAIL_BUTTON_DOWNLOAD);
	assert(detail_focus_move(DETAIL_BUTTON_PLAY, 1, queued) ==
	       DETAIL_BUTTON_ARTIST);
	assert(detail_focus_move(DETAIL_BUTTON_ARTIST, -1, queued) ==
	       DETAIL_BUTTON_PLAY);
	assert(detail_focus_move(DETAIL_BUTTON_PLAY, 1, only_play) ==
	       DETAIL_BUTTON_PLAY);
	/* A focus that is not usable falls back to Play: the download button
	 * that its own press queued, too. */
	assert(detail_focus_move(DETAIL_BUTTON_ARTIST, 1, no_artist) ==
	       DETAIL_BUTTON_PLAY);
	assert(detail_focus_move(DETAIL_BUTTON_DOWNLOAD, 0, queued) ==
	       DETAIL_BUTTON_PLAY);
	assert(detail_focus_move(DETAIL_BUTTON_NONE, 1, all) ==
	       DETAIL_BUTTON_PLAY);
	assert(detail_focus_move(DETAIL_BUTTON_ARTIST, 0, all) ==
	       DETAIL_BUTTON_ARTIST);

	/* An album's list and the button over it: up from the first row only,
	 * down back to the list; a disabled button never holds the focus. */
	assert(detail_header_focus(false, 0, -1, true));
	assert(!detail_header_focus(false, 1, -1, true));
	assert(!detail_header_focus(false, 0, 1, true));
	assert(!detail_header_focus(false, 0, -1, false));
	assert(detail_header_focus(true, 0, -1, true));
	assert(detail_header_focus(true, 3, 0, true));
	assert(!detail_header_focus(true, 0, 1, true));
	assert(!detail_header_focus(true, 0, 0, false));
}

/* The list under an album's or playlist's download button: three rows
 * where the results have four, the same pitch, the same edges. */
static void test_list_frames(void)
{
	list_frame full = list_frame_full();
	list_frame under = list_frame_under_button();
	assert(full.top == TAB_H && full.rows == RESULTS_PER_PAGE);
	ui_rect button = list_header_button_rect();
	assert(button.y == TAB_H + SP_1 && button.h == TOUCH_MIN);
	assert(button.x == SP_2 && button.w == SCREEN_BOTTOM_W - 2 * SP_2);
	assert(under.top == button.y + button.h + SP_1);
	assert(under.rows == 3);
	/* The rows: 76..117, 121..162, 166..207, clear of the button bar. */
	assert(list_row_rect_in(under, 0).y == 76);
	ui_rect last = list_row_rect_in(under, under.rows - 1);
	assert(last.y == 166 && last.y + last.h <= SCREEN_H - BAR_H - SP_1);
	/* The full frame is the one the functions without a frame use. */
	for (int i = 0; i < RESULTS_PER_PAGE; i++) {
		ui_rect a = list_row_rect(i), b = list_row_rect_in(full, i);
		assert(a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h);
	}
	/* Ten tracks: seven rows of scroll under the button, six without. */
	assert(list_max_scroll_in(under, 10) == 7 * LIST_PITCH);
	assert(list_max_scroll(10) == 6 * LIST_PITCH);
	assert(list_max_scroll_in(under, 3) == 0);
	/* A touch on the button is not a row, nor the start of a drag. */
	assert(!list_area_contains_in(under, 10, button.y + 1));
	assert(list_hit_row_in(under, 10, button.y + 1, 0, 10) == -1);
	assert(list_hit_row_in(under, 10, 80, 0, 10) == 0);
	assert(list_hit_row_in(under, 10, 170, 0, 10) == 2);
	assert(list_hit_row_in(under, 10, 170, LIST_PITCH, 10) == 3);
	/* Whole rows, and the buttons' realignment, in three. */
	int from, to;
	list_full_rows_in(under, 0, 10, &from, &to);
	assert(from == 0 && to == 2);
	assert(list_row_whole_in(under, 2, 0, 10));
	assert(!list_row_whole_in(under, 3, 0, 10));
	assert(list_align_in(under, 0, 5, 10) == 3);
	assert(list_align_in(under, 0, 1, 10) == 0);
	list_drag d;
	list_drag_begin(&d, 150, 0);
	assert(list_drag_scroll_in(under, &d, -400, 10) == 7 * LIST_PITCH);
	/* The scroll indicator runs between the button and the button bar. */
	ui_rect track = list_scroll_track_in(under);
	assert(track.y == under.top && track.y + track.h == SCREEN_H - BAR_H);
	ui_rect thumb = list_scroll_thumb_in(under, 7 * LIST_PITCH, 10);
	assert(thumb.y + thumb.h == track.y + track.h);
	assert(list_scroll_thumb_in(under, 0, 3).h == track.h);
}

/* What the download buttons say and do, from where the download is. */
/* An index for the manager: tracks in every state, an album with one
 * track waiting, a whole playlist, and a lone track on the card. */
static void manager_index(dl_index *idx)
{
	dl_index_clear(idx);
	dl_track t;
	const char *names[] = { "q1", "q2", "f1", "d1", "d2", "a1", "p1", "p2",
	                        "lone" };
	for (int i = 0; i < 9; i++) {
		memset(&t, 0, sizeof(t));
		snprintf(t.uri, sizeof(t.uri), "spotify:track:%s", names[i]);
		snprintf(t.name, sizeof(t.name), "%s", names[i]);
		dl_add_track(idx, &t, i >= 2, 100 + (uint32_t)i);
	}
	/* q1, q2 queued (q1 first); f1 failed; d1, d2, p1, p2, lone on the
	 * card; a1 downloading. */
	idx->tracks[2].state = DL_FAILED;
	for (int i = 3; i < 9; i++)
		if (i != 5)
			idx->tracks[i].state = DL_DONE;
	idx->tracks[5].state = DL_ACTIVE;
	idx->tracks[3].added_at = 300;   /* d1: the newest of the tracks */
	idx->tracks[8].added_at = 50;    /* lone: the oldest */
	dl_collection c;
	memset(&c, 0, sizeof(c));
	snprintf(c.uri, sizeof(c.uri), "spotify:album:a");
	c.kind = DL_COLL_ALBUM;
	int album = dl_add_collection(idx, &c, 200);
	dl_add_ref(idx, album, 0);    /* q1: the album still waits */
	dl_add_ref(idx, album, 3);    /* d1 */
	snprintf(c.uri, sizeof(c.uri), "spotify:playlist:p");
	c.kind = DL_COLL_PLAYLIST;
	int playlist = dl_add_collection(idx, &c, 250);
	dl_add_ref(idx, playlist, 6);
	dl_add_ref(idx, playlist, 7);
}

static void test_downloads_manager_list(void)
{
	static dl_index idx;
	static dlv_list l;
	static dlv_done_item scratch[DL_TRACKS_MAX + DL_COLLS_MAX];
	manager_index(&idx);
	dlv_build(&l, &idx, 1, scratch);

	/* In progress: a1. Queued: the collection being resolved, the album
	 * that waits (for q1), then q2 on its own: q1 is the album's row.
	 * Failed: f1. Downloaded, newest first: d1 (300), the playlist (250),
	 * d2 (104), lone (50); p1 and p2 are the playlist's row, and d1 is a
	 * row of its own, since its album still waits. */
	struct { int kind, section, index; } want[] = {
		{ DLV_HEADER, DLV_SEC_ACTIVE, DLV_SEC_ACTIVE },
		{ DLV_TRACK, DLV_SEC_ACTIVE, 5 },
		{ DLV_HEADER, DLV_SEC_QUEUED, DLV_SEC_QUEUED },
		{ DLV_PENDING, DLV_SEC_QUEUED, 0 },
		{ DLV_COLL, DLV_SEC_QUEUED, 0 },
		{ DLV_TRACK, DLV_SEC_QUEUED, 1 },
		{ DLV_HEADER, DLV_SEC_FAILED, DLV_SEC_FAILED },
		{ DLV_TRACK, DLV_SEC_FAILED, 2 },
		{ DLV_HEADER, DLV_SEC_DONE, DLV_SEC_DONE },
		{ DLV_TRACK, DLV_SEC_DONE, 3 },
		{ DLV_COLL, DLV_SEC_DONE, 1 },
		{ DLV_TRACK, DLV_SEC_DONE, 4 },
		{ DLV_TRACK, DLV_SEC_DONE, 8 },
	};
	int n = (int)(sizeof(want) / sizeof(want[0]));
	assert(l.count == n);
	for (int i = 0; i < n; i++) {
		assert(l.entries[i].kind == want[i].kind);
		assert(l.entries[i].section == want[i].section);
		assert(l.entries[i].index == want[i].index);
	}
	assert(l.rows[DLV_SEC_ACTIVE] == 1 && l.rows[DLV_SEC_QUEUED] == 3 &&
	       l.rows[DLV_SEC_FAILED] == 1 && l.rows[DLV_SEC_DONE] == 4);
	/* Headers and rows one under the other. */
	assert(l.entries[0].y == 0 && l.entries[1].y == DLV_HEADER_H);
	assert(l.entries[2].y == DLV_HEADER_H + LIST_PITCH);
	assert(l.height == 4 * DLV_HEADER_H + 9 * LIST_PITCH);

	/* The focus skips the headers and stops at the ends. */
	bool bumped;
	assert(dlv_first_row(&l) == 1);
	assert(dlv_move(&l, 1, 1, &bumped) == 3 && !bumped);
	assert(dlv_move(&l, 5, 1, &bumped) == 7);
	assert(dlv_move(&l, 3, -1, &bumped) == 1);
	assert(dlv_move(&l, 1, -1, &bumped) == 1 && bumped);
	assert(dlv_move(&l, 12, 1, &bumped) == 12 && bumped);
	assert(dlv_clamp_focus(&l, 6) == 7);
	assert(dlv_clamp_focus(&l, 99) == 12);
	assert(dlv_clamp_focus(&l, -3) == 1);

	/* Scrolling: the first row of a section brings its header; the list
	 * never scrolls past either end. */
	int max = dlv_max_scroll(&l);
	assert(max == SP_1 + l.height - dlv_view_h());
	assert(dlv_scroll_to(&l, 1, 200) == 0);
	int s = dlv_scroll_to(&l, 12, 0);
	assert(s == max);
	assert(dlv_row_whole(&l, 12, s) && !dlv_row_whole(&l, 1, s));
	s = dlv_scroll_to(&l, 7, max);
	assert(s == l.entries[6].y);
	assert(dlv_clamp_scroll(&l, -5) == 0 && dlv_clamp_scroll(&l, 9999) == max);
	/* Taps land on rows, never on headers or the bars. */
	ui_rect r = dlv_entry_rect(&l, 1, 0);
	assert(r.y == TAB_H + SP_1 + DLV_HEADER_H && r.h == ROW_H);
	assert(dlv_hit_row(&l, r.x + 1, r.y + 1, 0) == 1);
	assert(dlv_hit_row(&l, r.x + 1, TAB_H + SP_1 + 2, 0) == -1);
	assert(dlv_hit_row(&l, r.x + 1, 230, 0) == -1);
	int from, to;
	dlv_visible(&l, 0, &from, &to);
	assert(from == 0 && to >= 3 && to < l.count);
	ui_rect thumb = dlv_scroll_thumb(&l, max);
	ui_rect track = dlv_scroll_track();
	assert(thumb.y + thumb.h == track.y + track.h);

	/* Nothing to list, or no index: an empty list, no focus. */
	dlv_build(&l, NULL, 0, scratch);
	assert(l.count == 0 && dlv_first_row(&l) == -1 &&
	       dlv_clamp_focus(&l, 0) == -1 && dlv_max_scroll(&l) == 0);
	static dl_index none;
	dl_index_clear(&none);
	dlv_build(&l, &none, 0, scratch);
	assert(l.count == 0);

	/* An album whose tracks all failed has nothing waiting: it is a
	 * Downloaded row (to remove it), its tracks Failed rows. */
	manager_index(&idx);
	idx.tracks[0].state = DL_FAILED;
	idx.tracks[1].state = DL_DONE;
	idx.tracks[5].state = DL_DONE;
	dlv_build(&l, &idx, 0, scratch);
	assert(l.rows[DLV_SEC_ACTIVE] == 0 && l.rows[DLV_SEC_QUEUED] == 0);
	assert(l.rows[DLV_SEC_FAILED] == 2);
	bool album_done = false;
	for (int i = 0; i < l.count; i++)
		if (l.entries[i].kind == DLV_COLL && l.entries[i].index == 0)
			album_done = l.entries[i].section == DLV_SEC_DONE;
	assert(album_done);
	/* d1 is the album's now: no row of its own. */
	for (int i = 0; i < l.count; i++)
		assert(!(l.entries[i].kind == DLV_TRACK && l.entries[i].index == 3));

	/* The summary: on the card, its size, what waits. */
	manager_index(&idx);
	idx.tracks[3].size = 3u * 1024 * 1024;
	idx.tracks[4].size = 2u * 1024 * 1024;
	dlv_summary sum = dlv_summarize(&idx);
	assert(sum.tracks == 5 && sum.albums == 1 && sum.playlists == 1);
	assert(sum.used == 5u * 1024 * 1024 && sum.waiting == 3);
	char text[32];
	dlv_format_size(5u * 1024 * 1024, ',', text, sizeof(text));
	assert(strcmp(text, "5 MB") == 0);
	dlv_format_size(0, ',', text, sizeof(text));
	assert(strcmp(text, "0 MB") == 0);
	dlv_format_size((uint64_t)5 * 1024 * 1024 * 1024 + 100u * 1024 * 1024,
	                ',', text, sizeof(text));
	assert(strcmp(text, "5,1 GB") == 0);
	dlv_format_size((uint64_t)1024 * 1024 * 1024, '.', text, sizeof(text));
	assert(strcmp(text, "1.0 GB") == 0);

	/* The manager's top screen is Settings' in another mode. */
	assert(top_mode_with_manager(TOP_ABOUT, true) == TOP_DOWNLOADS);
	assert(top_mode_with_manager(TOP_ABOUT, false) == TOP_ABOUT);
	assert(top_mode_with_manager(TOP_DETAIL, true) == TOP_DETAIL);
	assert(top_mode_with_manager(TOP_NOW_CARD, true) == TOP_NOW_CARD);
}

static void test_offline_rows(void)
{
	static dl_index idx;
	static track_row rows[64];
	static offline_item scratch[DL_TRACKS_MAX];
	dl_index_clear(&idx);
	dl_track t;
	/* t0..t4: t0, t1 alone (t1 newest), t2..t4 an album's; t3 queued. */
	for (int i = 0; i < 5; i++) {
		memset(&t, 0, sizeof(t));
		snprintf(t.uri, sizeof(t.uri), "spotify:track:t%d", i);
		dl_add_track(&idx, &t, i < 2, 10 + (uint32_t)i);
		idx.tracks[i].state = DL_DONE;
	}
	idx.tracks[3].state = DL_QUEUED;
	idx.tracks[4].state = DL_FAILED;
	/* A sixth, alone but still queued: not in the Downloads yet. */
	memset(&t, 0, sizeof(t));
	snprintf(t.uri, sizeof(t.uri), "spotify:track:t5");
	dl_add_track(&idx, &t, true, 99);
	dl_collection c;
	memset(&c, 0, sizeof(c));
	snprintf(c.uri, sizeof(c.uri), "spotify:album:x");
	c.kind = DL_COLL_ALBUM;
	int album = dl_add_collection(&idx, &c, 5);
	for (int i = 2; i < 5; i++)
		dl_add_ref(&idx, album, i);
	snprintf(c.uri, sizeof(c.uri), "spotify:playlist:y");
	c.kind = DL_COLL_PLAYLIST;
	int playlist = dl_add_collection(&idx, &c, 50);
	dl_add_ref(&idx, playlist, 0);

	/* Collections first, newest first; then the tracks alone on the card,
	 * newest first. */
	int colls;
	int n = offline_home_rows(&idx, rows, 64, scratch, &colls);
	assert(n == 4 && colls == 2);
	assert(rows[0].kind == TRACK_ROW_PLAYLIST && rows[0].index == playlist);
	assert(rows[0].done == 1 && rows[0].count == 1);
	assert(rows[1].kind == TRACK_ROW_ALBUM && rows[1].index == album);
	assert(rows[1].done == 1 && rows[1].count == 3);
	assert(rows[2].kind == TRACK_ROW_TRACK && rows[2].index == 1);
	assert(rows[3].kind == TRACK_ROW_TRACK && rows[3].index == 0);
	for (int i = 0; i < n; i++)
		assert(!rows[i].off);
	/* Room for fewer: the first ones. */
	assert(offline_home_rows(&idx, rows, 3, scratch, &colls) == 3);
	assert(offline_home_rows(NULL, rows, 64, scratch, &colls) == 0 &&
	       colls == 0);

	/* An album on the card: its order; what is not downloaded is off. */
	n = offline_collection_rows(&idx, album, rows, 64);
	assert(n == 3);
	assert(rows[0].index == 2 && !rows[0].off);
	assert(rows[1].index == 3 && rows[1].off && rows[1].state == DL_QUEUED);
	assert(rows[2].index == 4 && rows[2].off && rows[2].state == DL_FAILED);
	assert(offline_collection_rows(&idx, 7, rows, 64) == 0);

	/* The D-pad over rows that are off: up/down to the next on, pages
	 * counting only those, bumping at the ends. */
	track_row r[6];
	memset(r, 0, sizeof(r));
	r[1].off = r[2].off = r[4].off = true;
	bool bumped;
	assert(track_row_move(r, 6, 0, 1, &bumped) == 3 && !bumped);
	assert(track_row_move(r, 6, 3, 1, &bumped) == 5);
	assert(track_row_move(r, 6, 5, 1, &bumped) == 5 && bumped);
	assert(track_row_move(r, 6, 3, -1, &bumped) == 0);
	assert(track_row_move(r, 6, 0, -1, &bumped) == 0 && bumped);
	assert(track_row_move(r, 6, 0, 4, &bumped) == 5 && !bumped);
	assert(track_row_move(r, 6, 5, -2, &bumped) == 0);
	assert(track_row_move(NULL, 6, 2, 1, &bumped) == 3);
	assert(track_row_first_on(r, 6, 1) == 3);
	assert(track_row_first_on(r, 6, 4) == 5);
	r[5].off = true;
	r[3].off = true;
	assert(track_row_first_on(r, 6, 4) == 0);
	r[0].off = true;
	assert(track_row_first_on(r, 6, 0) == -1);
	assert(track_row_on(NULL, 3) && !track_row_on(r, 3));

	/* Home: Liked Songs, the Downloads, then the feed's playlists. */
	assert(home_tile_of_feed(0) == 0);
	assert(home_downloads_tile() == 1);
	assert(home_tile_of_feed(1) == 2 && home_tile_of_feed(5) == 6);
	assert(HOME_PINNED == 2);
}

static void test_download_buttons(void)
{
	assert(dl_track_button(false, DL_DONE, true) == DL_BUTTON_DOWNLOAD);
	assert(dl_track_button(true, DL_QUEUED, true) == DL_BUTTON_QUEUED);
	assert(dl_track_button(true, DL_ACTIVE, false) == DL_BUTTON_PROGRESS);
	assert(dl_track_button(true, DL_FAILED, true) == DL_BUTTON_RETRY);
	assert(dl_track_button(true, DL_DONE, true) == DL_BUTTON_REMOVE);
	/* On the card for an album only: removed with the album. */
	assert(dl_track_button(true, DL_DONE, false) == DL_BUTTON_HELD);
	assert(dl_button_enabled(DL_BUTTON_DOWNLOAD));
	assert(dl_button_enabled(DL_BUTTON_RETRY));
	assert(dl_button_enabled(DL_BUTTON_REMOVE));
	assert(!dl_button_enabled(DL_BUTTON_QUEUED));
	assert(!dl_button_enabled(DL_BUTTON_PROGRESS));
	assert(!dl_button_enabled(DL_BUTTON_HELD));

	assert(dl_collection_button(false, false, 0, 0) ==
	       DL_COLL_BUTTON_DOWNLOAD);
	assert(dl_collection_button(true, false, 0, 0) ==
	       DL_COLL_BUTTON_WAITING);
	/* Waiting to be resolved again wins over what the index holds. */
	assert(dl_collection_button(true, true, 12, 12) ==
	       DL_COLL_BUTTON_WAITING);
	assert(dl_collection_button(false, true, 8, 12) ==
	       DL_COLL_BUTTON_PROGRESS);
	assert(dl_collection_button(false, true, 12, 12) ==
	       DL_COLL_BUTTON_DONE);
	assert(dl_collection_button(false, true, 0, 0) == DL_COLL_BUTTON_DONE);

	/* The player's icon: right of the album link, a TOUCH_MIN square
	 * SP_1 from it, ending at the margin, on the links' second row. */
	ui_rect icon = player_download_rect();
	ui_rect album = player_link_rect(PLAYER_LINK_ALBUM);
	assert(icon.w == TOUCH_MIN && icon.h == TOUCH_MIN);
	assert(icon.y == album.y && icon.x == album.x + album.w + SP_1);
	assert(icon.x + icon.w == SCREEN_BOTTOM_W - SP_2);
	bool links_all[PLAYER_LINK_COUNT] = { true, true, true };
	assert(player_link_hit(icon.x + 1, icon.y + 1, links_all) ==
	       PLAYER_LINK_NONE);
}

static void test_detail_pool(void)
{
	detail_stack stacks[DETAIL_STACKS];
	for (int i = 0; i < DETAIL_STACKS; i++)
		detail_stack_clear(&stacks[i]);
	assert(detail_pool_free(stacks) == 0);

	/* Opening, going back and switching tabs in a long, fixed pattern:
	 * the pool never runs out and no document is on two levels at once. */
	unsigned rng = 12345;
	for (int step = 0; step < 5000; step++) {
		rng = rng * 1103515245u + 12345u;
		detail_stack *s = &stacks[(rng >> 8) % DETAIL_STACKS];
		if ((rng >> 12) % 4 == 0) {
			detail_stack_pop(s);
			continue;
		}
		/* The order details.c uses: room first, then a free document. */
		if (s->count >= DETAIL_DEPTH)
			assert(detail_stack_drop_oldest(s) >= 0);
		int slot = detail_pool_free(stacks);
		assert(slot >= 0 && slot < DETAIL_POOL);
		assert(detail_stack_push(s, slot) == -1);

		bool seen[DETAIL_POOL] = { false };
		for (int i = 0; i < DETAIL_STACKS; i++) {
			assert(stacks[i].count <= DETAIL_DEPTH);
			for (int j = 0; j < stacks[i].count; j++) {
				assert(!seen[stacks[i].slot[j]]);
				seen[stacks[i].slot[j]] = true;
			}
		}
	}

	/* Both stacks full: every document is taken, and the oldest of one
	 * stack is what makes room. */
	for (int i = 0; i < DETAIL_STACKS; i++)
		detail_stack_clear(&stacks[i]);
	for (int i = 0; i < DETAIL_STACKS; i++)
		for (int j = 0; j < DETAIL_DEPTH; j++)
			detail_stack_push(&stacks[i], detail_pool_free(stacks));
	assert(detail_pool_free(stacks) == -1);
	int oldest = detail_stack_drop_oldest(&stacks[1]);
	assert(detail_pool_free(stacks) == oldest);
}

/* One EntityExtensionData { entity_uri 2, extension_data 3 (Any) } holding a
 * Track that only has a name. */
static void add_batch_entry(pb *arr, const char *uri, const char *name)
{
	uint8_t track_s[64], any_s[96], ed_s[160];
	pb track, any, ed;
	pb_init(&track, track_s, sizeof(track_s));
	pb_str(&track, 2, name);
	pb_init(&any, any_s, sizeof(any_s));
	pb_sub(&any, 2, &track);
	pb_init(&ed, ed_s, sizeof(ed_s));
	pb_str(&ed, 2, uri);
	pb_sub(&ed, 3, &any);
	pb_sub(arr, 3, &ed);
}

static int parse_batch(pb *arr, const char *const *uris, int n, sp_track *out)
{
	static uint8_t body_s[1024];
	pb body;
	pb_init(&body, body_s, sizeof(body_s));
	pb_sub(&body, 2, arr);
	assert(pb_ok(&body));
	return sp_parse_track_batch(body.p, body.len, uris, n, out);
}

static void test_meta_parse_batch(void)
{
	static const char *const ASK[] = {
		"spotify:track:a", "spotify:track:b", "spotify:track:c",
	};
	static sp_track out[3];
	static uint8_t arr_s[768];
	pb arr;

	/* Shuffled, as the service answers: back in the order asked. */
	pb_init(&arr, arr_s, sizeof(arr_s));
	add_batch_entry(&arr, "spotify:track:c", "C");
	add_batch_entry(&arr, "spotify:track:a", "A");
	add_batch_entry(&arr, "spotify:track:b", "B");
	assert(parse_batch(&arr, ASK, 3, out) == 3);
	for (int i = 0; i < 3; i++) {
		assert(strcmp(out[i].uri, ASK[i]) == 0);
		assert(out[i].name[0] == 'A' + i && out[i].name[1] == '\0');
	}

	/* One left out: the others close ranks in order. */
	pb_init(&arr, arr_s, sizeof(arr_s));
	add_batch_entry(&arr, "spotify:track:c", "C");
	add_batch_entry(&arr, "spotify:track:a", "A");
	assert(parse_batch(&arr, ASK, 3, out) == 2);
	assert(strcmp(out[0].name, "A") == 0 && strcmp(out[1].name, "C") == 0);
	assert(strcmp(out[1].uri, "spotify:track:c") == 0);

	/* A stray URI arriving first does not steal a slot that a later entry
	 * names; it takes what is left. */
	pb_init(&arr, arr_s, sizeof(arr_s));
	add_batch_entry(&arr, "spotify:track:relinked", "X");
	add_batch_entry(&arr, "spotify:track:a", "A");
	add_batch_entry(&arr, "spotify:track:c", "C");
	assert(parse_batch(&arr, ASK, 3, out) == 3);
	assert(strcmp(out[0].name, "A") == 0);
	assert(strcmp(out[1].name, "X") == 0 &&
	       strcmp(out[1].uri, "spotify:track:b") == 0);
	assert(strcmp(out[2].name, "C") == 0);

	/* The same track asked twice and answered twice fills both. */
	static const char *const TWICE[] = { "spotify:track:a", "spotify:track:a" };
	pb_init(&arr, arr_s, sizeof(arr_s));
	add_batch_entry(&arr, "spotify:track:a", "A");
	add_batch_entry(&arr, "spotify:track:a", "A");
	assert(parse_batch(&arr, TWICE, 2, out) == 2);

	/* Nothing usable. */
	pb_init(&arr, arr_s, sizeof(arr_s));
	assert(parse_batch(&arr, ASK, 3, out) == 0);
	assert(sp_parse_track_batch(arr_s, 0, ASK, 3, out) == 0);
	assert(sp_parse_track_batch(arr_s, 0, ASK, 0, out) == 0);
}

static void test_queue_boundaries(void)
{
	assert(queue_step(0, 3, -1) == -1);
	assert(queue_step(0, 3, 1) == 1);
	assert(queue_step(1, 3, -1) == 0);
	assert(queue_step(1, 3, 1) == 2);
	assert(queue_step(2, 3, 1) == -1);
	assert(queue_step(0, 0, 1) == -1);
	assert(queue_step(3, 3, -1) == -1);
	assert(queue_step(1, 3, 0) == -1);

	assert(!queue_wants_more(-1, 3));
	assert(!queue_wants_more(0, 4));
	assert(queue_wants_more(0, 3));
	assert(queue_wants_more(5, 6));
}

/* Every index of 0..count-1 exactly once. */
static bool queue_is_permutation(const int *order, int count)
{
	bool seen[64] = {0};
	assert(count <= 64);
	for (int i = 0; i < count; i++) {
		if (order[i] < 0 || order[i] >= count || seen[order[i]])
			return false;
		seen[order[i]] = true;
	}
	return true;
}

static void test_queue_order(void)
{
	int order[64];
	uint32_t rng = 12345;

	/* In order: the list from the chosen track, the earlier ones behind. */
	assert(queue_order_start(order, 5, 2, false, &rng) == 2);
	for (int i = 0; i < 5; i++)
		assert(order[i] == i);
	assert(queue_order_start(order, 5, 5, false, &rng) == -1);
	assert(queue_order_start(order, 0, 0, true, &rng) == -1);

	/* Shuffled: the chosen track first, every track once, and not the
	 * list's order every time. */
	bool moved = false;
	for (int round = 0; round < 20; round++) {
		assert(queue_order_start(order, 40, 7, true, &rng) == 0);
		assert(order[0] == 7);
		assert(queue_is_permutation(order, 40));
		for (int i = 1; i < 40 && !moved; i++)
			moved = order[i] != (i == 7 ? 0 : i);
	}
	assert(moved);
	/* A single track, and the first one, shuffle fine. */
	assert(queue_order_start(order, 1, 0, true, &rng) == 0 && order[0] == 0);
	assert(queue_order_start(order, 3, 0, true, &rng) == 0 && order[0] == 0);
	assert(queue_is_permutation(order, 3));

	/* Switched off while position 4 plays: the list's order again, from
	 * that same track. */
	queue_order_start(order, 10, 3, true, &rng);
	int track = order[4];
	int pos = queue_order_reshuffle(order, 10, 4, false, &rng);
	assert(pos == track && order[pos] == track);
	for (int i = 0; i < 10; i++)
		assert(order[i] == i);
	/* Switched on: that track first, the rest after it. */
	pos = queue_order_reshuffle(order, 10, pos, true, &rng);
	assert(pos == 0 && order[0] == track);
	assert(queue_is_permutation(order, 10));

	/* Appended in order: after the rest. */
	queue_order_start(order, 3, 1, false, &rng);
	assert(queue_order_append(order, 3, 2, 1, false, &rng) == 5);
	for (int i = 0; i < 5; i++)
		assert(order[i] == i);
	/* Appended shuffled: what already played keeps its place. */
	queue_order_start(order, 6, 4, true, &rng);
	int played[3] = { order[0], order[1], order[2] };
	assert(queue_order_append(order, 6, 20, 2, true, &rng) == 26);
	assert(queue_is_permutation(order, 26));
	for (int i = 0; i < 3; i++)
		assert(order[i] == played[i]);
}

static void test_queue_drop_known(void)
{
	static sp_track queue[2], fresh[5];
	memset(queue, 0, sizeof(queue));
	memset(fresh, 0, sizeof(fresh));
	strcpy(queue[0].uri, "spotify:track:a");
	strcpy(queue[1].uri, "spotify:track:b");
	strcpy(fresh[0].uri, "spotify:track:a");   /* the seed back */
	strcpy(fresh[1].uri, "spotify:track:c");
	strcpy(fresh[2].uri, "spotify:track:c");   /* twice in one batch */
	/* fresh[3]: no URI */
	strcpy(fresh[4].uri, "spotify:track:d");
	strcpy(fresh[4].name, "D");
	assert(queue_drop_known(queue, 2, fresh, 5) == 2);
	assert(strcmp(fresh[0].uri, "spotify:track:c") == 0);
	assert(strcmp(fresh[1].uri, "spotify:track:d") == 0);
	assert(strcmp(fresh[1].name, "D") == 0);
	assert(queue_drop_known(queue, 2, fresh, 0) == 0);
}

/* The conversion specifiers of a printf format, flags and widths included,
 * so "%02d" in one language and "%2d" in another count as different. A
 * '%' before a space or at the end is a plain percent sign ("100%") in a
 * text that is drawn as it is, never formatted. */
static void format_signature(const char *text, char *out, size_t outlen)
{
	size_t n = 0;
	for (const char *p = text; *p; p++) {
		if (*p != '%')
			continue;
		if (p[1] == ' ' || p[1] == '\0')
			continue;
		p++;
		if (*p == '%')
			continue;
		while (*p && !strchr("diouxXeEfgGcsp", *p)) {
			assert(n + 1 < outlen);
			out[n++] = *p++;
		}
		assert(*p && n + 2 < outlen);
		out[n++] = *p;
		out[n++] = ';';
	}
	out[n] = '\0';
}

static void test_language_tables(void)
{
	for (int lang = 0; lang < LANG_COUNT; lang++) {
		for (int id = 0; id < STR_COUNT; id++) {
			const char *text = lang_text((lang_id)lang, (str_id)id);
			/* Every entry is filled in, with no fallback to English. */
			assert(text[0]);
			assert(lang == LANG_EN ||
			       text != lang_text(LANG_EN, (str_id)id) ||
			       strcmp(text, "Error") == 0 ||
			       strcmp(text, "Album") == 0 ||
			       strcmp(text, "Album: %s") == 0 ||
			       strcmp(text, "EP") == 0 ||
			       strcmp(text, "Premium") == 0 ||
			       strcmp(text, "3DSpotify") == 0);
			/* Valid UTF-8 with no control characters (lang.h). */
			uint32_t cp;
			size_t len;
			for (const char *p = text;
			     (len = text_utf8_decode(p, &cp)) > 0; p += len)
				assert(cp >= 0x20 && cp != 0x7f && cp != 0xFFFD);
			char expected[64], actual[64];
			format_signature(lang_text(LANG_EN, (str_id)id), expected,
			                 sizeof(expected));
			format_signature(text, actual, sizeof(actual));
			assert(strcmp(expected, actual) == 0);
		}
	}
}

static void test_language_switching(void)
{
	lang_id lang;
	assert(lang_get() == LANG_EN);
	assert(lang_next(LANG_EN) == LANG_ES);
	assert(lang_next(LANG_ES) == LANG_EN);
	assert(lang_from_code("es", &lang) && lang == LANG_ES);
	assert(lang_from_code(lang_code(LANG_EN), &lang) && lang == LANG_EN);
	assert(!lang_from_code("fr", &lang));
	assert(!lang_from_code(NULL, &lang));
	lang_set(LANG_ES);
	assert(strcmp(tr(STR_PLAYER_FINISHED_TITLE), "Pista terminada") == 0);
	lang_set(lang_next(lang_get()));
	assert(strcmp(tr(STR_PLAYER_FINISHED_TITLE), "Track finished") == 0);
	lang_set((lang_id)LANG_COUNT);
	assert(lang_get() == LANG_EN);
	assert(strcmp(lang_text(LANG_ES, (str_id)STR_COUNT), "") == 0);
}

static void test_image_fit(void)
{
	int w = 0, h = 0;
	image_fit(640, 640, 256, &w, &h);
	assert(w == 256 && h == 256);
	image_fit(640, 320, 256, &w, &h);
	assert(w == 256 && h == 128);
	image_fit(300, 600, 256, &w, &h);
	assert(w == 128 && h == 256);
	image_fit(1000, 1, 256, &w, &h);
	assert(w == 256 && h == 1);
	image_fit(0, 10, 256, &w, &h);
	assert(w == 1 && h == 1);
}

static void test_image_morton_offset(void)
{
	/* Inside the first 8x8 tile: Z-order, x in the low bit. */
	assert(image_morton_offset(0, 0, 256) == 0);
	assert(image_morton_offset(1, 0, 256) == 1);
	assert(image_morton_offset(0, 1, 256) == 2);
	assert(image_morton_offset(1, 1, 256) == 3);
	assert(image_morton_offset(2, 0, 256) == 4);
	assert(image_morton_offset(0, 2, 256) == 8);
	assert(image_morton_offset(4, 0, 256) == 16);
	assert(image_morton_offset(0, 4, 256) == 32);
	assert(image_morton_offset(7, 7, 256) == 63);
	/* Tiles go left to right, then down. */
	assert(image_morton_offset(8, 0, 256) == 64);
	assert(image_morton_offset(0, 8, 256) == 32 * 64);
	assert(image_morton_offset(255, 255, 256) == 256 * 256 - 1);
	assert(image_morton_offset(9, 8, 16) == 3 * 64 + 1);
}

static void test_image_to_texture(void)
{
	/* 2x2 RGBA box-filtered to one texel, written as A, B, G, R. */
	const uint8_t src[16] = {
		10, 20, 30, 255,   30, 40, 50, 255,
		50, 60, 70, 255,   70, 80, 90, 255,
	};
	uint8_t tex[8 * 8 * 4];
	memset(tex, 0xEE, sizeof(tex));
	image_to_texture(src, 2, 2, tex, 8, 1, 1);
	assert(tex[0] == 255 && tex[1] == 60 && tex[2] == 50 && tex[3] == 40);
	/* Only the dw x dh corner is written. */
	assert(tex[4] == 0xEE && tex[sizeof(tex) - 1] == 0xEE);

	/* Same size: every pixel lands at its Morton offset. */
	uint8_t big[8 * 8 * 4];
	for (int i = 0; i < 64; i++) {
		big[i * 4] = (uint8_t)i;      /* R carries the source index */
		big[i * 4 + 1] = 0;
		big[i * 4 + 2] = 0;
		big[i * 4 + 3] = 255;
	}
	image_to_texture(big, 8, 8, tex, 8, 8, 8);
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++) {
			uint32_t at = image_morton_offset((uint32_t)x, (uint32_t)y, 8);
			assert(tex[at * 4 + 3] == y * 8 + x);
			assert(tex[at * 4] == 255);
		}

	/* Upscaling repeats source pixels instead of reading past them. */
	const uint8_t one[4] = {1, 2, 3, 4};
	image_to_texture(one, 1, 1, tex, 8, 8, 8);
	for (int i = 0; i < 64; i++)
		assert(tex[i * 4] == 4 && tex[i * 4 + 3] == 1);

	/* Bad arguments leave the texture alone. */
	memset(tex, 0x11, sizeof(tex));
	image_to_texture(src, 2, 2, tex, 8, 16, 16);
	image_to_texture(NULL, 2, 2, tex, 8, 1, 1);
	assert(tex[0] == 0x11);
}

static void test_image_pow2(void)
{
	assert(image_pow2(0) == 8);
	assert(image_pow2(1) == 8);
	assert(image_pow2(8) == 8);
	assert(image_pow2(9) == 16);
	assert(image_pow2(128) == 128);
	assert(image_pow2(129) == 256);
	assert(image_pow2(1024) == 1024);
	assert(image_pow2(1025) == 0);
}

static void test_image_copy_to_texture(void)
{
	/* A 10x3 image in a 16x8 texture: every pixel lands unfiltered at its
	 * Morton offset, as A, B, G, R. */
	enum { W = 10, H = 3, TW = 16, TH = 8 };
	uint8_t src[W * H * 4];
	for (int i = 0; i < W * H; i++) {
		src[i * 4] = (uint8_t)i;           /* R carries the index */
		src[i * 4 + 1] = (uint8_t)(i + 1); /* G */
		src[i * 4 + 2] = (uint8_t)(i + 2); /* B */
		src[i * 4 + 3] = (i & 1) ? 255 : 0;
	}
	uint8_t tex[TW * TH * 4];
	memset(tex, 0xEE, sizeof(tex));
	image_copy_to_texture(src, W, H, tex, TW, TH);
	for (int y = 0; y < TH; y++)
		for (int x = 0; x < TW; x++) {
			uint32_t at = image_morton_offset((uint32_t)x, (uint32_t)y,
			                                  TW) * 4;
			if (x < W && y < H) {
				int i = y * W + x;
				assert(tex[at] == ((i & 1) ? 255 : 0));
				assert(tex[at + 1] == (uint8_t)(i + 2));
				assert(tex[at + 2] == (uint8_t)(i + 1));
				assert(tex[at + 3] == (uint8_t)i);
			} else {
				/* Nothing outside w x h is written. */
				assert(tex[at] == 0xEE && tex[at + 1] == 0xEE &&
				       tex[at + 2] == 0xEE && tex[at + 3] == 0xEE);
			}
		}

	/* Images that do not fit, bad widths and NULLs leave it alone. */
	memset(tex, 0x11, sizeof(tex));
	image_copy_to_texture(src, TW + 1, 1, tex, TW, TH);
	image_copy_to_texture(src, 1, TH + 1, tex, TW, TH);
	image_copy_to_texture(src, 1, 1, tex, 12, TH);
	image_copy_to_texture(NULL, 1, 1, tex, TW, TH);
	image_copy_to_texture(src, 0, 1, tex, TW, TH);
	for (size_t i = 0; i < sizeof(tex); i++)
		assert(tex[i] == 0x11);
}

/* Every byte is 6 px wide, so a multibyte character is wider than it looks:
 * the point is that a cut never lands inside one. */
static float fake_measure(const char *text, void *ctx)
{
	int *calls = ctx;
	if (calls)
		(*calls)++;
	return 6.0f * (float)strlen(text);
}

static bool valid_utf8(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		int len = *p < 0x80 ? 1 : *p >= 0xF0 ? 4 : *p >= 0xE0 ? 3
		          : *p >= 0xC2 ? 2 : 0;
		if (len == 0)
			return false;
		for (int i = 1; i < len; i++)
			if ((p[i] & 0xC0) != 0x80)
				return false;
		p += len;
	}
	return true;
}

static bool ends_with_ellipsis(const char *s)
{
	size_t n = strlen(s), e = strlen(TEXT_FIT_ELLIPSIS);
	return n >= e && strcmp(s + n - e, TEXT_FIT_ELLIPSIS) == 0;
}

static void test_marquee(void)
{
	/* A text that fits never moves. */
	assert(!marquee_rolls(200, 200));
	for (unsigned ms = 0; ms < 20000; ms += 37)
		assert(marquee_offset(ms, 150, 200) == 0);

	int w = 320, box = 200;
	assert(marquee_rolls(w, box));
	unsigned period = marquee_period_ms(w);
	assert(period == MARQUEE_PAUSE_MS +
	                 (unsigned)(w + MARQUEE_GAP) * 1000u / MARQUEE_SPEED);
	/* It waits at its start, then slides left, never backwards, and
	 * stays inside one span. */
	assert(marquee_offset(0, w, box) == 0);
	assert(marquee_offset(MARQUEE_PAUSE_MS - 1, w, box) == 0);
	assert(marquee_offset(MARQUEE_PAUSE_MS + 1000, w, box) == MARQUEE_SPEED);
	int last = 0;
	for (unsigned ms = MARQUEE_PAUSE_MS; ms < period; ms++) {
		int offset = marquee_offset(ms, w, box);
		assert(offset >= last && offset < marquee_span(w));
		last = offset;
	}
	/* It reaches the copy's start, then begins again from rest. */
	assert(last >= marquee_span(w) - 1);
	assert(marquee_offset(period, w, box) == 0);
	assert(marquee_offset(period + MARQUEE_PAUSE_MS + 1000, w, box) ==
	       marquee_offset(MARQUEE_PAUSE_MS + 1000, w, box));
}

static void test_text_fit(void)
{
	char out[64];
	/* Fits: copied as is. */
	assert(!text_fit("daft punk", 54.0f, fake_measure, NULL, out,
	                 sizeof(out)));
	assert(strcmp(out, "daft punk") == 0);
	/* Too wide: prefix + ellipsis (3 bytes = 18 px). 60 px = 7 bytes. */
	assert(text_fit("daft punk", 53.0f, fake_measure, NULL, out,
	                sizeof(out)));
	assert(strcmp(out, "daft" TEXT_FIT_ELLIPSIS) == 0);
	/* Trailing spaces before the ellipsis are dropped. */
	assert(text_fit("ab   cdef", 48.0f, fake_measure, NULL, out,
	                sizeof(out)));
	assert(strcmp(out, "ab" TEXT_FIT_ELLIPSIS) == 0);
	/* Not even the ellipsis fits: empty. */
	assert(text_fit("abc", 10.0f, fake_measure, NULL, out, sizeof(out)));
	assert(out[0] == '\0');
	assert(!text_fit(NULL, 100.0f, fake_measure, NULL, out, sizeof(out)));
	assert(out[0] == '\0');

	/* Never cuts inside a multibyte character, at any width. */
	const char *mixed = "canci\xc3\xb3n \xe6\x97\xa5\xe6\x9c\xac "
	                    "\xf0\x9f\x8e\xb5 m\xc3\xbasica";
	assert(valid_utf8(mixed));
	for (int px = 0; px <= 6 * 40; px++) {
		bool cut = text_fit(mixed, (float)px, fake_measure, NULL, out,
		                    sizeof(out));
		assert(valid_utf8(out));
		assert(fake_measure(out, NULL) <= (float)px);
		if (cut && out[0])
			assert(ends_with_ellipsis(out));
		if (!cut)
			assert(strcmp(out, mixed) == 0);
	}
	/* The longest prefix that fits is chosen: one more character would
	 * not fit. "canci" + "o-acute" is 7 bytes, + ellipsis = 60 px. */
	assert(text_fit(mixed, 60.0f, fake_measure, NULL, out, sizeof(out)));
	assert(strcmp(out, "canci\xc3\xb3" TEXT_FIT_ELLIPSIS) == 0);
	assert(text_fit(mixed, 59.0f, fake_measure, NULL, out, sizeof(out)));
	assert(strcmp(out, "canci" TEXT_FIT_ELLIPSIS) == 0);

	/* A small output buffer truncates even when the width would allow
	 * more, and stays terminated. */
	char tiny[8];
	assert(text_fit("abcdefghij", 1000.0f, fake_measure, NULL, tiny,
	                sizeof(tiny)));
	assert(strcmp(tiny, "abcd" TEXT_FIT_ELLIPSIS) == 0);

	/* Binary search: a long string costs a handful of measurements. */
	char long_text[96];
	memset(long_text, 'x', sizeof(long_text) - 1);
	long_text[sizeof(long_text) - 1] = '\0';
	int calls = 0;
	text_fit(long_text, 120.0f, fake_measure, &calls, out, sizeof(out));
	assert(calls <= 10);
	assert(fake_measure(out, NULL) <= 120.0f);
}

static void test_home_layout(void)
{
	ui_rect tabs = tabs_bar_rect();
	ui_rect hints = home_hints_rect();
	ui_rect label = home_label_rect();
	grid_geom six = home_six_geom();
	grid_geom more = home_more_geom();
	ui_rect button = home_more_rect();

	/* Plan 3.4: label 36..52, six tiles 56..160, "Show more" 176..208. */
	assert(tabs.y == 0 && tabs.h == TAB_H);
	assert(label.x == SP_2 && label.y == 36 && label.y + label.h == 52);
	assert(six.cols == 2 && six.rows == 3 && six.top == 56);
	ui_rect last = grid_cell_rect(&six, HOME_RECENT_SHOWN - 1, 0);
	assert(last.y + last.h == 160);
	assert(button.x == SP_2 && button.y == 176 && button.h == TOUCH_MIN);
	assert(button.w == GRID_WIDTH);
	/* Clear of the button bar, its shadow and its press included (4.4). */
	int ring = FOCUS_GAP + FOCUS_W;
	int below = ring > SHADOW_OFF ? ring : SHADOW_OFF;
	assert(button.y + button.h + below + PRESS_SHIFT <= hints.y);
	assert(button.y - (last.y + last.h) >= SP_4);
	/* "Show more": five rows exactly between the bars. */
	ui_rect area = grid_area(&more);
	assert(area.y == TAB_H && area.y + area.h == hints.y);
	/* The label sits over the six, not over a tile. */
	assert(label.y + label.h <= grid_area(&six).y + SP_1);
	assert(!home_has_more(6) && home_has_more(7));
}

static void test_home_focus(void)
{
	/* Down from a tile that can still move stays on the tiles. */
	assert(home_focus_vertical(HOME_FOCUS_TILES, true, true, true) ==
	       HOME_FOCUS_TILES);
	/* From the last row, to "Show more" when it exists. */
	assert(home_focus_vertical(HOME_FOCUS_TILES, true, false, true) ==
	       HOME_FOCUS_MORE);
	assert(home_focus_vertical(HOME_FOCUS_TILES, true, false, false) ==
	       HOME_FOCUS_TILES);
	assert(home_focus_vertical(HOME_FOCUS_TILES, false, true, true) ==
	       HOME_FOCUS_TILES);
	/* Up from the button, back to the tiles; down stays. */
	assert(home_focus_vertical(HOME_FOCUS_MORE, false, false, true) ==
	       HOME_FOCUS_TILES);
	assert(home_focus_vertical(HOME_FOCUS_MORE, true, false, true) ==
	       HOME_FOCUS_MORE);
}

static void test_layout_rects(void)
{
	ui_rect r = { 10, 20, 100, 40 };
	ui_rect g = ui_rect_inset(r, -3);
	assert(g.x == 7 && g.y == 17 && g.w == 106 && g.h == 46);
	ui_rect o = ui_rect_offset(r, 2, 2);
	assert(o.x == 12 && o.y == 22 && o.w == r.w && o.h == r.h);
	assert(ui_rect_contains(r, 10, 20) && !ui_rect_contains(r, 110, 20));
	assert(ui_px_ceil(12.0f) == 12 && ui_px_ceil(12.01f) == 13);
	assert(ui_px_ceil(0.0f) == 0);
}

/* Ticks `n` frames. */
static void ticks(press_state *p, int n)
{
	while (n-- > 0)
		press_tick(p);
}

static void test_press_timing(void)
{
	press_state p;
	press_reset(&p);
	assert(!press_down(&p) && press_shown(&p));

	/* A quick tap still shows the pressed look ANIM_PRESS_MIN frames,
	 * then returns to rest in a single frame (6.3). */
	press_begin(&p);
	press_end(&p);
	int pressed_frames = 0;
	while (press_down(&p)) {
		assert(!press_shown(&p));
		pressed_frames++;
		press_tick(&p);
	}
	assert(pressed_frames == ANIM_PRESS_MIN);
	assert(press_shown(&p));

	/* Held: pressed for as long as the button is down. */
	press_begin(&p);
	ticks(&p, ANIM_PRESS_MIN * 10);
	assert(press_down(&p) && press_shown(&p));
	press_end(&p);
	press_tick(&p);
	assert(!press_down(&p));

	/* Cancelled (stylus slid out): back to rest at once. */
	press_begin(&p);
	press_tick(&p);
	press_cancel(&p);
	assert(!press_down(&p));
}

static void test_touch_tracking(void)
{
	touch_track t = { -1 };
	int control;
	/* Touching outside every control: nothing, ever. */
	assert(touch_track_update(&t, true, true, false, -1, &control) ==
	       TOUCH_NONE);
	assert(touch_track_update(&t, false, false, true, -1, &control) ==
	       TOUCH_NONE);

	/* Press inside, lift inside: activate on release. */
	assert(touch_track_update(&t, true, true, false, 1, &control) ==
	       TOUCH_PRESS && control == 1);
	assert(touch_track_update(&t, false, true, false, 1, &control) ==
	       TOUCH_NONE);
	assert(touch_track_update(&t, false, false, true, -1, &control) ==
	       TOUCH_ACTIVATE && control == 1);
	assert(t.target == -1);

	/* Sliding out cancels for good, even when coming back. */
	assert(touch_track_update(&t, true, true, false, 0, &control) ==
	       TOUCH_PRESS);
	assert(touch_track_update(&t, false, true, false, -1, &control) ==
	       TOUCH_CANCEL && control == 0);
	assert(touch_track_update(&t, false, true, false, 0, &control) ==
	       TOUCH_NONE);
	assert(touch_track_update(&t, false, false, true, -1, &control) ==
	       TOUCH_NONE);

	/* Sliding onto another control cancels too. */
	assert(touch_track_update(&t, true, true, false, 0, &control) ==
	       TOUCH_PRESS);
	assert(touch_track_update(&t, false, true, false, 1, &control) ==
	       TOUCH_CANCEL && control == 0);
}

/* WCAG relative luminance and contrast ratio. */
static double channel_luminance(uint32_t c, int shift)
{
	double v = (double)((c >> shift) & 0xFF) / 255.0;
	return v <= 0.03928 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double luminance(uint32_t c)
{
	return 0.2126 * channel_luminance(c, 0) +
	       0.7152 * channel_luminance(c, 8) +
	       0.0722 * channel_luminance(c, 16);
}

static double contrast(uint32_t a, uint32_t b)
{
	double la = luminance(a), lb = luminance(b);
	double hi = la > lb ? la : lb, lo = la > lb ? lb : la;
	return (hi + 0.05) / (lo + 0.05);
}

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static void test_theme_colors(void)
{
	/* Every colour is opaque (2, 2.7). */
	const uint32_t all[] = {
		COL_FONDO_SUP, COL_FONDO_INF, COL_CAPA, COL_CAPA_ALTA, COL_CAPA_LUZ,
		COL_SOMBRA, COL_CLOROFILA, COL_MENTA, COL_MUSGO, COL_LIMA,
		COL_TINTA, COL_TEXTO, COL_TEXTO_2, COL_TEXTO_OFF, COL_CORAL,
		COL_CORAL_LUZ, COL_CORAL_OSC, COL_AMBAR, COL_AMBAR_LUZ,
		COL_AMBAR_OSC, COL_BTN_A, COL_BTN_B, COL_BTN_X, COL_BTN_Y,
	};
	for (size_t i = 0; i < COUNT(all); i++)
		assert(all[i] >> 24 == 0xFF);
	/* Packing matches C2D_Color32: R in the low byte. */
	assert(COL_BTN_X == (0x3Fu | 0xA2u << 8 | 0xFFu << 16 | 0xFFu << 24));
	/* Never Spotify's greens (2.7). */
	for (size_t i = 0; i < COUNT(all); i++) {
		assert(all[i] != THEME_RGB(0x1D, 0xB9, 0x54));
		assert(all[i] != THEME_RGB(0x1E, 0xD7, 0x60));
	}
}

static void test_theme_contrast(void)
{
	/* The text combinations of STYLES.md 2.6, >= 4.5:1 (10.2). */
	const uint32_t on_background[] = { COL_TEXTO, COL_TEXTO_2 };
	const uint32_t backgrounds[] = { COL_FONDO_SUP, COL_FONDO_INF };
	for (size_t i = 0; i < COUNT(on_background); i++)
		for (size_t j = 0; j < COUNT(backgrounds); j++)
			assert(contrast(on_background[i], backgrounds[j]) >= 4.5);
	/* A pressed link of the Playing tab is `lima` text on the background
	 * (plan 4.4): the only accent that reads on both tones. */
	for (size_t j = 0; j < COUNT(backgrounds); j++)
		assert(contrast(COL_LIMA, backgrounds[j]) >= 4.5);

	/* `capa` also holds disabled text and a pressed secondary button's
	 * label (6.1, 6.3). */
	const uint32_t on_capa[] = {
		COL_TEXTO, COL_TEXTO_2, COL_TEXTO_OFF, COL_CLOROFILA, COL_LIMA
	};
	for (size_t i = 0; i < COUNT(on_capa); i++)
		assert(contrast(on_capa[i], COL_CAPA) >= 4.5);

	assert(contrast(COL_TEXTO, COL_CAPA_ALTA) >= 4.5);
	assert(contrast(COL_TEXTO_2, COL_CAPA_ALTA) >= 4.5);

	/* Includes the pressed fills `musgo` and `coral_osc` (6.3). */
	const uint32_t under_tinta[] = {
		COL_CLOROFILA, COL_MUSGO, COL_LIMA, COL_CORAL, COL_CORAL_OSC,
		COL_AMBAR, COL_BTN_A, COL_BTN_B, COL_BTN_X, COL_BTN_Y,
	};
	for (size_t i = 0; i < COUNT(under_tinta); i++)
		assert(contrast(COL_TINTA, under_tinta[i]) >= 4.5);

	/* Disabled text stays dimmer than secondary text. */
	assert(luminance(COL_TEXTO_OFF) < luminance(COL_TEXTO_2));
	/* Pressed fills stay darker than their resting colour. */
	assert(luminance(COL_CORAL_OSC) < luminance(COL_CORAL));
	assert(luminance(COL_MUSGO) < luminance(COL_CLOROFILA));
}

static void test_list_logic(void)
{
	bool bumped;
	/* Clamping. */
	assert(list_clamp(5, 3) == 2 && list_clamp(-1, 3) == 0);
	assert(list_clamp(4, 0) == 0);

	/* Moving stops at the ends and reports the bump (6.6). */
	assert(list_move(0, 1, 24, &bumped) == 1 && !bumped);
	assert(list_move(0, -1, 24, &bumped) == 0 && bumped);
	assert(list_move(23, 1, 24, &bumped) == 23 && bumped);
	assert(list_move(2, -RESULTS_PER_PAGE, 24, &bumped) == 0 && !bumped);
	assert(list_move(21, RESULTS_PER_PAGE, 24, &bumped) == 23 && !bumped);
	assert(list_move(0, 1, 0, &bumped) == 0 && !bumped);

	/* Scrolling moves just enough to keep the selection visible. */
	assert(list_scroll(0, 3, 24, 4) == 0);
	assert(list_scroll(0, 4, 24, 4) == 1);
	assert(list_scroll(5, 4, 24, 4) == 4);
	assert(list_scroll(5, 6, 24, 4) == 5);
	assert(list_scroll(0, 23, 24, 4) == 20);
	assert(list_scroll(30, 23, 24, 4) == 20);
	assert(list_scroll(2, 1, 3, 4) == 0);
	assert(list_scroll(0, 0, 0, 4) == 0);

	/* "from-to of count". */
	int from, to;
	list_range(0, 24, 4, &from, &to);
	assert(from == 1 && to == 4);
	list_range(20, 24, 4, &from, &to);
	assert(from == 21 && to == 24);
	list_range(0, 3, 4, &from, &to);
	assert(from == 1 && to == 3);
	list_range(0, 0, 4, &from, &to);
	assert(from == 0 && to == 0);

	/* 7.4: four rows of ROW_H, SP_1 apart and SP_1 from both bars, fill
	 * the 184 px between the tab bar and the button bar exactly. */
	ui_rect hints = list_hints_rect();
	assert(list_top() == TAB_H && hints.h == BAR_H);
	assert(4 * ROW_H + 5 * SP_1 == SCREEN_H - TAB_H - BAR_H);
	assert(list_row_rect(0).y == list_top() + SP_1);
	for (int slot = 1; slot < RESULTS_PER_PAGE; slot++)
		assert(list_row_rect(slot).y ==
		       list_row_rect(slot - 1).y + ROW_H + SP_1);
	ui_rect last = list_row_rect(RESULTS_PER_PAGE - 1);
	/* The same gap over the button bar as under the tab bar. */
	assert(last.y + last.h + SP_1 == hints.y);
	assert(list_row_rect(0).y - list_top() == hints.y - (last.y + last.h));
	assert(list_row_rect(0).x == SP_2 &&
	       list_row_rect(0).w == SCREEN_BOTTOM_W - 2 * SP_2);
	/* Pressed rows move PRESS_SHIFT right and stay on screen. */
	assert(last.x + last.w + PRESS_SHIFT <= SCREEN_BOTTOM_W);

	/* 5.3: a thumbnail SP_1 from the row's edges, text SP_2 after. */
	ui_rect row = list_row_rect(1);
	ui_rect thumb = list_thumb_rect(row);
	assert(thumb.w == THUMB_SIZE && thumb.h == THUMB_SIZE);
	assert(thumb.x == row.x + SP_1 && thumb.y == row.y + SP_1);
	assert(thumb.y + thumb.h + SP_1 == row.y + row.h);
	assert(list_text_x(row) == thumb.x + thumb.w + SP_2);

	/* Hit testing: rows only, offset by the scroll, not past the end. */
	ui_rect r0 = list_row_rect(0);
	assert(list_hit_row(r0.x + 1, r0.y + 1, 0, 24) == 0);
	assert(list_hit_row(r0.x + 1, r0.y + 1, 7 * LIST_PITCH, 24) == 7);
	assert(list_hit_row(r0.x + 1, r0.y + r0.h, 0, 24) == -1);  /* gap */
	assert(list_hit_row(r0.x + 1, last.y + 1, 0, 24) == 3);
	assert(list_hit_row(r0.x + 1, last.y + 1, 0, 3) == -1);
	assert(list_hit_row(r0.x + 1, 5, 0, 24) == -1);            /* tabs */
	assert(list_hit_row(r0.x + 1, 230, 0, 24) == -1);          /* bar */
	assert(list_hit_row(2, r0.y + 1, 0, 24) == -1);            /* margin */

	/* Empty and error states: notice, then a 32 px button SP_4 below,
	 * clear of the bar with its shadow. */
	ui_rect notice = list_notice_rect();
	ui_rect button = list_action_rect(100, false);
	ui_rect centred = list_action_rect(100, true);
	assert(notice.y == list_top() + SP_4);
	assert(button.y == notice.y + notice.h + SP_4);
	assert(button.h == TOUCH_MIN && button.w == 100 + 2 * SP_3);
	assert(button.x == SP_2);
	assert(centred.x * 2 + centred.w == SCREEN_BOTTOM_W);
	assert(button.y + button.h + SHADOW_OFF + SP_1 <= hints.y);
	assert(list_action_rect(1000, false).w == notice.w);

	/* Durations: never cut, never negative. */
	char text[16];
	list_format_duration(320000, text, sizeof(text));
	assert(strcmp(text, "5:20") == 0);
	list_format_duration(9000, text, sizeof(text));
	assert(strcmp(text, "0:09") == 0);
	list_format_duration(-5, text, sizeof(text));
	assert(strcmp(text, "0:00") == 0);
	list_format_duration(3725000, text, sizeof(text));
	assert(strcmp(text, "62:05") == 0);
}

static void test_art_url(void)
{
	assert(art_url_allowed("https://i.scdn.co/image/ab67616d00004851"));
	assert(strcmp(art_url_path("https://i.scdn.co/image/abc"),
	              "/image/abc") == 0);
	/* Other hosts, schemes, and anything that could escape the path. */
	assert(!art_url_allowed("http://i.scdn.co/image/abc"));
	assert(!art_url_allowed("https://i.scdn.co.evil.com/image/abc"));
	assert(!art_url_allowed("https://evil.com/i.scdn.co/abc"));
	assert(!art_url_allowed("https://i.scdn.co/"));
	assert(!art_url_allowed("https://i.scdn.co/a b"));
	assert(!art_url_allowed("https://i.scdn.co/a\r\nHost: x"));
	assert(!art_url_allowed("https://i.scdn.co/a\\b"));
	assert(!art_url_allowed("https://i.scdn.co/a#b"));
	assert(!art_url_allowed("https://i.scdn.co/\xc3\xa1"));
	assert(!art_url_allowed(""));
	assert(!art_url_allowed(NULL));
	assert(art_url_path("https://evil.com/x") == NULL);
}

static void test_thumb_queue(void)
{
	thumb_state s[24];
	for (int i = 0; i < 24; i++)
		s[i] = THUMB_WAITING;
	/* Visible rows first, in order. */
	assert(thumb_pick(s, 24, 0, 4) == 0);
	s[0] = s[1] = THUMB_READY;
	s[2] = THUMB_LOADING;
	s[3] = THUMB_NONE;
	/* Then the next page. */
	assert(thumb_pick(s, 24, 0, 4) == 4);
	for (int i = 4; i < 8; i++)
		s[i] = THUMB_FAILED;
	/* Nothing beyond the next page until the list scrolls. */
	assert(thumb_pick(s, 24, 0, 4) == -1);
	assert(thumb_pick(s, 24, 5, 4) == 8);
	/* Scrolling back up picks what was skipped there first. */
	s[0] = THUMB_WAITING;
	assert(thumb_pick(s, 24, 0, 4) == 0);
	/* The window is cut at the end of the list. */
	assert(thumb_pick(s, 10, 8, 4) == 8);
	assert(thumb_pick(s, 0, 0, 4) == -1);
	assert(thumb_pick(s, 24, -3, 4) == 0);

	/* What survives a new list at the same position. */
	assert(thumb_keep(THUMB_READY, true));
	assert(thumb_keep(THUMB_NONE, true));
	assert(thumb_keep(THUMB_DECODED, true));
	assert(!thumb_keep(THUMB_READY, false));
	assert(!thumb_keep(THUMB_FAILED, true));   /* try again */
	assert(!thumb_keep(THUMB_WAITING, true));
	assert(!thumb_keep(THUMB_LOADING, true));
}

static void test_grid_layout(void)
{
	/* §3.3 of the home plan: widths, columns and text per column. */
	static const int widths[3][3] = { { 304 }, { 150, 150 }, { 99, 99, 98 } };
	static const int xs[3][3] = { { 8 }, { 8, 162 }, { 8, 111, 214 } };
	static const int text[3][3] = { { 260 }, { 106, 106 }, { 55, 55, 54 } };
	for (int cols = 1; cols <= 3; cols++) {
		grid_geom g = { cols, 36, 5 };
		int sum = 0;
		for (int c = 0; c < cols; c++) {
			assert(grid_col_width(&g, c) == widths[cols - 1][c]);
			assert(grid_col_x(&g, c) == xs[cols - 1][c]);
			assert(grid_text_width(&g, c) == text[cols - 1][c]);
			sum += grid_col_width(&g, c);
		}
		assert(sum + (cols - 1) * GRID_GAP == 304);
		/* No overlap, every cell inside the screen with its margins. */
		for (int i = 0; i < 3 * cols; i++)
			for (int j = i + 1; j < 3 * cols; j++) {
				ui_rect a = grid_cell_rect(&g, i, 0);
				ui_rect b = grid_cell_rect(&g, j, 0);
				assert(a.x + a.w <= b.x || b.x + b.w <= a.x ||
				       a.y + a.h <= b.y || b.y + b.h <= a.y);
				assert(a.x >= SP_2 && a.x + a.w <= SCREEN_BOTTOM_W - SP_2);
			}
	}
	assert(TILE_H >= TOUCH_MIN);
	assert(TILE_COVER + 2 * SP_1 == TILE_H);

	/* Five rows from 36 fill the space between the bars exactly. */
	grid_geom more = { 2, 36, 5 };
	ui_rect area = grid_area(&more);
	assert(area.y == TAB_H && area.y + area.h == SCREEN_H - BAR_H);
	ui_rect last = grid_cell_rect(&more, 9, 0);
	assert(last.y + last.h + SP_1 == SCREEN_H - BAR_H);
	/* The home grid of six: three rows from 56 end at 160. */
	grid_geom six = { 2, 56, 3 };
	ui_rect six_last = grid_cell_rect(&six, 5, 0);
	assert(six_last.y == 128 && six_last.y + six_last.h == 160);
	assert(grid_max_scroll(&six, 6) == 0);

	/* Hits: tiles only, not gaps, not past the count, not outside. */
	assert(grid_hit(&more, 24, 10, 40, 0) == 0);
	assert(grid_hit(&more, 24, 170, 40, 0) == 1);
	assert(grid_hit(&more, 24, 159, 40, 0) == -1);   /* column gap */
	assert(grid_hit(&more, 24, 10, 69, 0) == -1);    /* row gap */
	assert(grid_hit(&more, 24, 10, 73, 0) == 2);
	assert(grid_hit(&more, 3, 170, 73, 0) == -1);    /* past the count */
	assert(grid_hit(&more, 24, 10, 220, 0) == -1);   /* on the button bar */
	assert(grid_hit(&more, 24, 10, 40, GRID_PITCH) == 2);

	/* Scrolling: 24 tiles in 2 columns are 12 rows, 7 beyond the five. */
	assert(grid_rows(&more, 24) == 12);
	assert(grid_max_scroll(&more, 24) == 7 * GRID_PITCH);
	assert(grid_snap(&more, GRID_PITCH / 2, 24) == 1);
	assert(grid_snap(&more, 10000, 24) == 7);
	assert(grid_row_whole(&more, 4, 0) && !grid_row_whole(&more, 5, 0));
	/* The SP_1 margin above the first row: four pixels of scroll still
	 * leave it whole, the fifth cuts it under the tab bar. */
	assert(grid_row_whole(&more, 0, SP_1));
	assert(!grid_row_whole(&more, 0, SP_1 + 1));
	assert(grid_align(&more, 0, 11, 24) == 1);       /* row 5 into view */
	assert(grid_align(&more, 7 * GRID_PITCH, 0, 24) == 0);
	ui_rect thumb = grid_scroll_thumb(&more, 0, 24);
	ui_rect track = grid_scroll_track(&more);
	assert(thumb.y == track.y && thumb.h < track.h);
	thumb = grid_scroll_thumb(&more, grid_max_scroll(&more, 24), 24);
	assert(thumb.y + thumb.h == track.y + track.h);
	assert(grid_scroll_thumb(&six, 0, 6).h == grid_scroll_track(&six).h);
}

static void test_grid_focus(void)
{
	grid_geom g = { 2, 36, 5 };
	int dx, dy;
	/* Down keeps the column; into a shorter last row, its last tile. */
	assert(grid_move(&g, 5, 1, GRID_DOWN, &dx, &dy) == 3 && !dx && !dy);
	assert(grid_move(&g, 5, 3, GRID_DOWN, &dx, &dy) == 4);
	assert(grid_move(&g, 5, 4, GRID_DOWN, &dx, &dy) == 4 && dy == 1);
	assert(grid_move(&g, 5, 2, GRID_UP, &dx, &dy) == 0 && !dy);
	assert(grid_move(&g, 5, 1, GRID_UP, &dx, &dy) == 1 && dy == -1);
	/* Left/right within the row; no wrap at either side. */
	assert(grid_move(&g, 5, 0, GRID_RIGHT, &dx, &dy) == 1 && !dx);
	assert(grid_move(&g, 5, 1, GRID_RIGHT, &dx, &dy) == 1 && dx == 1);
	assert(grid_move(&g, 5, 2, GRID_LEFT, &dx, &dy) == 2 && dx == -1);
	assert(grid_move(&g, 5, 4, GRID_RIGHT, &dx, &dy) == 4 && dx == 1);
	/* One column: left/right jump a page. */
	grid_geom one = { 1, 36, 5 };
	assert(grid_move(&one, 12, 0, GRID_RIGHT, &dx, &dy) == 5 && !dy);
	assert(grid_move(&one, 12, 9, GRID_RIGHT, &dx, &dy) == 11);
	assert(grid_move(&one, 12, 11, GRID_RIGHT, &dx, &dy) == 11 && dy == 1);
	assert(grid_move(&one, 12, 3, GRID_LEFT, &dx, &dy) == 0 && !dy);
	/* Three columns, a row of one at the end. */
	grid_geom three = { 3, 36, 5 };
	assert(grid_move(&three, 7, 5, GRID_DOWN, &dx, &dy) == 6);
	assert(grid_move(&three, 7, 6, GRID_UP, &dx, &dy) == 3);
	assert(grid_move(&three, 0, 0, GRID_DOWN, &dx, &dy) == 0);
}

static void test_list_drag(void)
{
	/* Pixel geometry: one row per LIST_PITCH. */
	assert(LIST_PITCH == ROW_H + SP_1);
	assert(list_max_scroll(24) == 20 * LIST_PITCH);
	assert(list_max_scroll(4) == 0 && list_max_scroll(0) == 0);
	assert(list_row_at(0, 0).y == list_row_rect(0).y);
	assert(list_row_at(3, 0).y == list_row_rect(3).y);
	assert(list_row_at(5, 2 * LIST_PITCH).y == list_row_rect(3).y);
	assert(list_row_at(1, 10).y == list_row_rect(1).y - 10);

	/* Hit testing follows the scroll, rows only, inside the list area. */
	ui_rect r0 = list_row_rect(0);
	assert(list_hit_row(r0.x + 1, r0.y + 1, 7 * LIST_PITCH, 24) == 7);
	assert(list_hit_row(r0.x + 1, r0.y + 1, 10, 24) == 0);
	/* Scrolled 20 px: row 0's top part is under the tab bar. */
	assert(list_hit_row(r0.x + 1, list_top() - 1, 20, 24) == -1);
	assert(list_hit_row(r0.x + 1, list_top(), 20, 24) == 0);
	/* A fifth row peeking in at the bottom is hit; the bar is not. */
	assert(list_hit_row(r0.x + 1, SCREEN_H - BAR_H - 1, 20, 24) == 4);
	assert(list_hit_row(r0.x + 1, SCREEN_H - BAR_H, 20, 24) == -1);

	/* Snapping to the nearest whole row, within the ends. */
	assert(list_snap(0, 24) == 0);
	/* Halfway or more rounds to the next row, whatever the pitch's
	 * parity. */
	assert(list_snap((LIST_PITCH - 1) / 2, 24) == 0);
	assert(list_snap((LIST_PITCH + 1) / 2, 24) == 1);
	assert(list_snap(3 * LIST_PITCH + 30, 24) == 4);
	assert(list_snap(1000000, 24) == 20);
	assert(list_snap(-30, 24) == 0);
	assert(list_snap(100, 3) == 0);

	/* Whole rows on screen: 4 when resting on a row, 3 when cut at both
	 * ends. */
	int from, to;
	list_full_rows(0, 24, &from, &to);
	assert(from == 0 && to == 3);
	list_full_rows(5 * LIST_PITCH, 24, &from, &to);
	assert(from == 5 && to == 8);
	list_full_rows(20, 24, &from, &to);
	assert(from == 1 && to == 3);
	list_full_rows(SP_1, 24, &from, &to);       /* row 0 touches the tabs */
	assert(from == 0 && to == 3);
	list_full_rows(SP_1 + 1, 24, &from, &to);   /* now it is cut */
	assert(from == 1 && to == 3);
	list_full_rows(0, 2, &from, &to);
	assert(from == 0 && to == 1);
	/* Every whole row really is whole, and the next ones are not. */
	for (int s = 0; s <= list_max_scroll(24); s++) {
		list_full_rows(s, 24, &from, &to);
		assert(to - from + 1 >= RESULTS_PER_PAGE - 1);
		for (int i = from; i <= to; i++) {
			ui_rect r = list_row_at(i, s);
			assert(r.y >= list_top() && r.y + r.h <= SCREEN_H - BAR_H);
		}
		ui_rect before = list_row_at(from - 1, s);
		ui_rect after = list_row_at(to + 1, s);
		assert(from == 0 || before.y < list_top());
		assert(after.y + after.h > SCREEN_H - BAR_H);
	}

	/* Whether the focused row is whole: when it is not (cut or dragged
	 * out of view), the first button press only brings it back. */
	assert(list_row_whole(2, 20, 24));
	assert(!list_row_whole(0, 20, 24));          /* cut by the tabs */
	assert(!list_row_whole(4, 20, 24));          /* peeking in */
	assert(!list_row_whole(12, 5 * LIST_PITCH, 24));
	assert(list_row_whole(8, 5 * LIST_PITCH, 24));
	assert(list_row_whole(2, 0, 3));
	assert(!list_row_whole(0, 0, 0));
	/* Aligning on a hidden focus shows it whole. */
	for (int s = 0; s <= list_max_scroll(24); s += 7)
		for (int sel = 0; sel < 24; sel++)
			assert(list_row_whole(sel, list_align(s, sel, 24) * LIST_PITCH,
			                      24));

	/* Buttons bring a cut list back to whole rows, moving as little as
	 * possible while keeping the focus in view (6.6). */
	assert(list_align(20, 2, 24) == 0);
	assert(list_align(30, 2, 24) == 1);
	assert(list_align(30, 5, 24) == 2);
	assert(list_align(30, 0, 24) == 0);
	assert(list_align(5 * LIST_PITCH, 6, 24) == 5);
	assert(list_align(list_max_scroll(24) - 10, 23, 24) == 20);

	/* A touch becomes a drag after DRAG_START px, either way. */
	list_drag d;
	list_drag_begin(&d, 100, 0);
	assert(!list_drag_update(&d, 100 + DRAG_START - 1));
	assert(!list_drag_update(&d, 100 - DRAG_START + 1));
	assert(list_drag_update(&d, 100 - DRAG_START));
	/* Once dragging it stays a drag, even back at the start. */
	assert(list_drag_update(&d, 100));
	/* The list follows the stylus 1:1: moving up scrolls down. */
	list_drag_begin(&d, 150, 2 * LIST_PITCH);
	assert(list_drag_scroll(&d, 140, 24) == 2 * LIST_PITCH + 10);
	assert(list_drag_scroll(&d, 170, 24) == 2 * LIST_PITCH - 20);
	/* ... and stops at both ends. */
	assert(list_drag_scroll(&d, 150 + 1000, 24) == 0);
	assert(list_drag_scroll(&d, 150 - 5000, 24) == list_max_scroll(24));
	assert(list_drag_scroll(&d, 100, 3) == 0);
	/* Not touching: no drag. */
	d.down = false;
	d.dragging = false;
	assert(!list_drag_update(&d, 0));

	/* 5.11: the indicator against the right edge, between the bars. */
	ui_rect track = list_scroll_track();
	assert(track.x + track.w == SCREEN_BOTTOM_W && track.w == SCROLL_W);
	assert(track.y == list_top() && track.y + track.h == SCREEN_H - BAR_H);
	/* Drags only start inside the list area, never on the tab bar. */
	assert(!list_area_contains(10, TAB_H - 1));
	assert(list_area_contains(10, TAB_H));
	assert(!list_area_contains(10, SCREEN_H - BAR_H));
	/* Clear of the rows, pressed ones included. */
	assert(r0.x + r0.w + PRESS_SHIFT < track.x);
	/* Thumb: visible share of the list, top and bottom at the ends. */
	ui_rect top = list_scroll_thumb(0, 24);
	ui_rect bottom = list_scroll_thumb(list_max_scroll(24), 24);
	assert(top.y == track.y && top.h == track.h * 4 / 24);
	assert(bottom.y + bottom.h == track.y + track.h);
	ui_rect middle = list_scroll_thumb(list_max_scroll(24) / 2, 24);
	assert(middle.y > top.y && middle.y < bottom.y);
	/* Never shorter than SCROLL_MIN_H, never outside the track. */
	ui_rect tiny = list_scroll_thumb(list_max_scroll(1000), 1000);
	assert(tiny.h == SCROLL_MIN_H);
	assert(tiny.y + tiny.h == track.y + track.h);
	assert(list_scroll_thumb(-50, 24).y == track.y);
	assert(list_scroll_thumb(1000000, 24).y == bottom.y);
}

static void test_tabs(void)
{
	/* 5.12: four tabs across the top, at least the minimum touch target
	 * high, together exactly as wide as the screen. */
	ui_rect bar = tabs_bar_rect();
	assert(bar.x == 0 && bar.y == 0 && bar.w == SCREEN_BOTTOM_W);
	assert(bar.h == TAB_H && TAB_H >= TOUCH_MIN);
	int x = 0;
	for (int t = 0; t < TAB_COUNT; t++) {
		ui_rect r = tabs_tab_rect(t);
		assert(r.x == x && r.y == 0 && r.h == TAB_H);
		assert(r.w >= TOUCH_MIN && r.w >= ICON_S + 2 * SP_3);
		x += r.w;
		/* Hit testing matches what is drawn, edges included. */
		assert(tabs_hit(r.x, r.y) == t);
		assert(tabs_hit(r.x + r.w - 1, r.y + r.h - 1) == t);
	}
	assert(x == SCREEN_BOTTOM_W);
	/* Outside the bar: nothing. */
	assert(tabs_hit(10, TAB_H) == -1 && tabs_hit(10, 230) == -1);
	assert(tabs_hit(-1, 5) == -1 && tabs_hit(SCREEN_BOTTOM_W, 5) == -1);
	/* Active and pressed: only the bottom edge rises, 2 and 4 px. */
	assert(TAB_LIFT_PRESSED > TAB_LIFT_ACTIVE && TAB_LIFT_ACTIVE > 0);
	for (int t = 0; t < TAB_COUNT; t++) {
		ui_rect full = tabs_tab_rect(t);
		ui_rect rest = tabs_tab_face(t, TAB_FACE_REST);
		ui_rect on = tabs_tab_face(t, TAB_FACE_ACTIVE);
		ui_rect down = tabs_tab_face(t, TAB_FACE_PRESSED);
		assert(rest.x == full.x && rest.y == full.y && rest.w == full.w &&
		       rest.h == full.h);
		assert(on.x == full.x && on.y == full.y && on.w == full.w);
		assert(down.x == full.x && down.y == full.y && down.w == full.w);
		assert(on.h == full.h - TAB_LIFT_ACTIVE);
		assert(down.h == full.h - TAB_LIFT_PRESSED);
		assert(down.h >= ICON_S + 2 * SP_1);
	}
	/* The last tab ends at the screen's edge. */
	assert(tabs_tab_rect(TAB_SETTINGS).x + tabs_tab_rect(TAB_SETTINGS).w ==
	       SCREEN_BOTTOM_W);

	/* Stepping stops at both ends (no wrap, like the lists). */
	assert(tabs_step(TAB_HOME, 1) == TAB_SEARCH);
	assert(tabs_step(TAB_SEARCH, -1) == TAB_HOME);
	assert(tabs_step(TAB_HOME, -1) == -1);
	assert(tabs_step(TAB_SETTINGS, 1) == -1);
	assert(tabs_step(TAB_PLAYER, 0) == -1);
	assert(tabs_step(-1, 1) == -1 && tabs_step(TAB_COUNT, -1) == -1);

	/* L/R and ZL/ZR always switch tabs; both ways at once cancel out. */
	assert(tabs_key_delta(false, true, false, false) == 1);
	assert(tabs_key_delta(true, false, false, false) == -1);
	assert(tabs_key_delta(false, false, false, true) == 1);
	assert(tabs_key_delta(false, false, true, false) == -1);
	assert(tabs_key_delta(true, true, false, false) == 0);
	assert(tabs_key_delta(true, false, false, true) == 0);
	assert(tabs_key_delta(false, false, false, false) == 0);
}

static void test_settings_layout(void)
{
	ui_rect hints = settings_hints_rect();
	assert(hints.y + hints.h == SCREEN_H && hints.h == BAR_H);
	int ring = FOCUS_GAP + FOCUS_W;
	int below = ring > SHADOW_OFF ? ring : SHADOW_OFF;
	for (int i = 0; i < SETTINGS_ITEM_COUNT; i++) {
		ui_rect r = settings_item_rect(i);
		/* Full content width, 32 px high (4.4, 5.2). */
		assert(r.x == SP_2 && r.w == SCREEN_BOTTOM_W - 2 * SP_2);
		assert(r.h == TOUCH_MIN);
		assert(r.x + r.w + PRESS_SHIFT <= SCREEN_BOTTOM_W);
		if (i == 0)
			assert(r.y == TAB_H + SP_3);
		else {
			ui_rect prev = settings_item_rect(i - 1);
			/* Focus borders and shadows stay SP_1 apart at least. */
			assert((r.y - ring) - (prev.y + prev.h + below) >= SP_1);
		}
		assert(settings_hit_test(r.x, r.y) == (settings_item)i);
		assert(settings_hit_test(r.x + r.w - 1, r.y + r.h - 1) ==
		       (settings_item)i);
	}
	ui_rect last = settings_item_rect(SETTINGS_ITEM_COUNT - 1);
	assert(last.y + last.h + below + PRESS_SHIFT <= hints.y);
	/* The tab bar, the gaps and the bars are not buttons. */
	assert(settings_hit_test(10, 10) == SETTINGS_ITEM_NONE);
	assert(settings_hit_test(10, TAB_H + SP_3 - 1) == SETTINGS_ITEM_NONE);
	assert(settings_hit_test(10, 230) == SETTINGS_ITEM_NONE);

	/* Four buttons, the downloads second; the last clear of the bar. */
	assert(SETTINGS_ITEM_COUNT == 4 && SETTINGS_ITEM_DOWNLOADS == 1);
	assert(settings_item_rect(SETTINGS_ITEM_DOWNLOADS).y == 88);
	assert(settings_item_rect(SETTINGS_ITEM_PAIR).y == 176);
	/* Focus stops at the ends; anything else falls back to the first. */
	assert(settings_focus_move(SETTINGS_ITEM_LANGUAGE, 1) ==
	       SETTINGS_ITEM_DOWNLOADS);
	assert(settings_focus_move(SETTINGS_ITEM_DOWNLOADS, 1) ==
	       SETTINGS_ITEM_RECONNECT);
	assert(settings_focus_move(SETTINGS_ITEM_PAIR, 1) == SETTINGS_ITEM_PAIR);
	assert(settings_focus_move(SETTINGS_ITEM_LANGUAGE, -1) ==
	       SETTINGS_ITEM_LANGUAGE);
	assert(settings_focus_move(SETTINGS_ITEM_NONE, 1) ==
	       SETTINGS_ITEM_DOWNLOADS);
}

static void test_player_layout(void)
{
	/* 5.2: a 48 px primary between two 40 px secondaries, centred,
	 * SP_4 apart, under the tab bar and over the bars. */
	ui_rect prev = player_control_rect(PLAYER_CONTROL_PREV);
	ui_rect play = player_control_rect(PLAYER_CONTROL_PLAY);
	ui_rect next = player_control_rect(PLAYER_CONTROL_NEXT);
	assert(play.w == BTN_PLAY && play.h == BTN_PLAY);
	assert(prev.w == BTN_SKIP && next.h == BTN_SKIP);
	assert(play.x * 2 + play.w == SCREEN_BOTTOM_W);
	assert(play.x - (prev.x + prev.w) == SP_4);
	assert(next.x - (play.x + play.w) == SP_4);
	assert(prev.y + prev.h / 2 == play.y + play.h / 2);
	assert(play.y > TAB_H + SP_4);
	assert(player_hit_test(play.x, play.y) == PLAYER_CONTROL_PLAY);
	assert(player_hit_test(prev.x + 1, prev.y + 1) == PLAYER_CONTROL_PREV);
	assert(player_hit_test(next.x + next.w - 1, next.y + 1) ==
	       PLAYER_CONTROL_NEXT);
	assert(player_hit_test(prev.x + prev.w + 1, prev.y + 1) ==
	       PLAYER_CONTROL_NONE);
	assert(player_hit_test(10, 10) == PLAYER_CONTROL_NONE);

	/* The links (plan 4.4): touch zones of TOUCH_MIN, the track across the
	 * content width, then the artist and the album side by side, SP_1
	 * apart, all under the tab bar and above the controls. */
	ui_rect link_track = player_link_rect(PLAYER_LINK_TRACK);
	ui_rect link_artist = player_link_rect(PLAYER_LINK_ARTIST);
	ui_rect link_album = player_link_rect(PLAYER_LINK_ALBUM);
	assert(PLAYER_CONTROLS_TOP == 108);
	assert(link_track.x == SP_2 && link_track.y == 36 &&
	       link_track.w == SCREEN_BOTTOM_W - 2 * SP_2 &&
	       link_track.h == TOUCH_MIN);
	/* The second row leaves its right end to the download icon. */
	assert(link_artist.x == 8 && link_artist.y == 72 &&
	       link_artist.w == 132 && link_artist.h == TOUCH_MIN);
	assert(link_album.x == 144 && link_album.y == 72 &&
	       link_album.w == 132 && link_album.h == TOUCH_MIN);
	assert(link_album.x + link_album.w + SP_1 + TOUCH_MIN ==
	       SCREEN_BOTTOM_W - SP_2);
	assert(link_track.y >= TAB_H);
	assert(link_artist.y - (link_track.y + link_track.h) == SP_1);
	/* The links end above the controls, with room to spare. */
	assert(link_artist.y + link_artist.h < prev.y);
	assert(link_artist.y + link_artist.h < play.y);

	bool links_all[PLAYER_LINK_COUNT] = { true, true, true };
	bool links_no_album[PLAYER_LINK_COUNT] = { true, true, false };
	assert(player_link_hit(link_track.x + 1, link_track.y + 1, links_all) ==
	       PLAYER_LINK_TRACK);
	assert(player_link_hit(link_artist.x + 1, link_artist.y + 1, links_all) ==
	       PLAYER_LINK_ARTIST);
	assert(player_link_hit(link_album.x + 1, link_album.y + 1, links_all) ==
	       PLAYER_LINK_ALBUM);
	/* Between the two halves, and on a disabled link: nothing. */
	assert(player_link_hit(link_album.x - 2, link_album.y + 1, links_all) ==
	       PLAYER_LINK_NONE);
	assert(player_link_hit(link_album.x + 1, link_album.y + 1,
	                       links_no_album) == PLAYER_LINK_NONE);
	assert(player_link_hit(play.x + 1, play.y + 1, links_all) ==
	       PLAYER_LINK_NONE);
	/* The phases that show a track have links; idle and failed do not. */
	assert(!player_links_enabled(PLAYER_PHASE_IDLE));
	assert(player_links_enabled(PLAYER_PHASE_LOADING));
	assert(player_links_enabled(PLAYER_PHASE_PLAYING));
	assert(player_links_enabled(PLAYER_PHASE_FINISHED));
	assert(!player_links_enabled(PLAYER_PHASE_FAILED));

	/* 5.4, 5.5: the bars, below the controls and their shadow, clear of
	 * each other and of the button bar, the handle included. */
	ui_rect progress = player_progress_rect();
	ui_rect volume = player_volume_rect(40);
	assert(progress.h == PROGRESS_H && volume.h == VOLUME_H);
	assert(progress.x == SP_2 && progress.w == SCREEN_BOTTOM_W - 2 * SP_2);
	assert(play.y + play.h + SHADOW_OFF < player_times_cy() - SP_2);
	int handle_top = progress.y + progress.h / 2 - HANDLE_H / 2;
	assert(handle_top > player_times_cy());
	assert(volume.y > handle_top + HANDLE_H);
	assert(volume.x == SP_2 + 40 + SP_2);
	assert(volume.x + volume.w == SCREEN_BOTTOM_W - SP_2);
	assert(volume.y + volume.h + SP_2 <= player_hints_rect().y);
	ui_rect notice = player_notice_rect();
	assert(notice.y == TAB_H + SP_4);
	assert(notice.y + notice.h + SHADOW_OFF < player_hints_rect().y);

	/* Fractions. */
	assert(player_permille(0, 0) == 0);
	assert(player_permille(30, 60) == 500);
	assert(player_permille(90, 60) == 1000);
	assert(player_permille(4000000000u, 4000000000u) == 1000);
	assert(player_permille(1, 3) == 333);
	assert(player_fill_px(300, 500) == 150);
	assert(player_fill_px(300, 0) == 0 && player_fill_px(300, -5) == 0);
	assert(player_fill_px(300, 1000) == 300 && player_fill_px(300, 2000) == 300);
	assert(player_fill_px(0, 500) == 0);

	/* The bars' touch zones: each holds its bar, they meet without
	 * overlapping, below the controls and above the button bar. */
	ui_rect seek = player_seek_zone(40);
	ui_rect vol = player_volume_zone(40);
	assert(ui_rect_contains(seek, progress.x, progress.y));
	assert(ui_rect_contains(seek, progress.x + progress.w - 1,
	                        progress.y + progress.h - 1));
	assert(ui_rect_contains(vol, volume.x, volume.y));
	assert(ui_rect_contains(vol, volume.x + volume.w - 1,
	                        volume.y + volume.h - 1));
	assert(seek.y + seek.h == vol.y);
	assert(seek.y > play.y + play.h);
	assert(vol.y + vol.h == player_hints_rect().y);
	assert(!ui_rect_contains(vol, SP_2, volume.y));   /* the "Vol." label */
	/* A touch along a bar, its ends clamped. */
	assert(player_bar_permille(progress, progress.x - 5) == 0);
	assert(player_bar_permille(progress, progress.x) == 0);
	assert(player_bar_permille(progress, progress.x + progress.w / 2) == 500);
	assert(player_bar_permille(progress, progress.x + progress.w) == 1000);
	assert(player_bar_permille(progress, SCREEN_BOTTOM_W) == 1000);
	ui_rect empty = { 0, 0, 0, 0 };
	assert(player_bar_permille(empty, 10) == 0);
}

static void test_stream_chunks(void)
{
	enum { LEN = 3 * STREAM_CHUNK + 100 };   /* 4 chunks, the last short */
	uint8_t done[4] = {0};
	assert(stream_chunk_count(LEN) == 4);
	assert(stream_chunk_count(0) == 0);
	assert(stream_chunk_floor(STREAM_CHUNK + 5) == STREAM_CHUNK);

	/* Nothing yet: nothing readable; the live span from 0 is. */
	assert(stream_available(done, LEN, 0, 10) == 10);
	assert(stream_available(done, LEN, 500, 10) == 500);
	/* The live span marks what it finishes, the short last chunk too. */
	size_t next = 0;
	stream_mark(done, LEN, STREAM_CHUNK - 1, &next);
	assert(next == 0 && !done[0]);
	stream_mark(done, LEN, STREAM_CHUNK + 10, &next);
	assert(next == 1 && done[0] && !done[1]);
	assert(stream_available(done, LEN, STREAM_CHUNK + 10, 10) ==
	       STREAM_CHUNK + 10);

	/* A seek to chunk 3: the span moves there and finishes the file. */
	next = 3;
	stream_mark(done, LEN, LEN, &next);
	assert(next == 4 && done[3]);
	/* From chunk 0 the bytes end where chunk 1 is missing, whatever the
	 * live span is doing elsewhere. */
	assert(stream_available(done, LEN, LEN, 10) == STREAM_CHUNK);
	/* A stale watermark from an older span is not trusted: the live span
	 * must be in the very next chunk. */
	assert(stream_available(done, LEN, 3 * STREAM_CHUNK, 10) == STREAM_CHUNK);
	assert(stream_available(done, LEN, LEN, 3 * STREAM_CHUNK + 5) == LEN);
	assert(stream_available(done, LEN, 0, LEN) == LEN);
	/* Inside the hole: nothing. */
	assert(stream_available(done, LEN, LEN, 2 * STREAM_CHUNK + 1) ==
	       2 * STREAM_CHUNK + 1);

	/* The next missing chunk from where the decoder reads, past the end
	 * back to the start. */
	assert(stream_next_missing(done, LEN, 0) == STREAM_CHUNK);
	assert(stream_next_missing(done, LEN, 2 * STREAM_CHUNK + 7) ==
	       2 * STREAM_CHUNK);
	assert(stream_next_missing(done, LEN, 3 * STREAM_CHUNK) == STREAM_CHUNK);
	assert(stream_next_missing(done, LEN, LEN + 50) == STREAM_CHUNK);
	done[1] = done[2] = 1;
	assert(stream_next_missing(done, LEN, 0) == LEN);
	assert(stream_next_missing(done, 0, 0) == 0);
}

static void test_player_shared_helpers(void)
{
	char text[16];
	player_format_time(0, text, sizeof(text));
	assert(strcmp(text, "0:00") == 0);
	player_format_time(84, text, sizeof(text));
	assert(strcmp(text, "1:24") == 0);
	player_format_time(3601, text, sizeof(text));
	assert(strcmp(text, "60:01") == 0);

	assert(player_progress_permille(true, 0, 0) == 1000);
	assert(player_progress_permille(false, 30, 60) == 500);
	assert(player_progress_permille(false, 5, 0) == 0);

	/* The download follows every tick, else once a second. */
	player_download_sample sample = { 0, 0 };
	player_sample_download(&sample, true, 10);
	assert(sample.shown == 10);
	for (int i = 1; i < PLAYER_DOWNLOAD_FRAMES; i++) {
		player_sample_download(&sample, false, 20 + i);
		assert(sample.shown == 10);
	}
	player_sample_download(&sample, false, 99);
	assert(sample.shown == 99);
}

static bool overlaps(ui_rect a, ui_rect b)
{
	return a.x < b.x + b.w && b.x < a.x + a.w &&
	       a.y < b.y + b.h && b.y < a.y + a.h;
}

static bool on_top_screen(ui_rect r, int margin)
{
	return r.x >= margin && r.y >= margin &&
	       r.x + r.w <= SCREEN_TOP_W - margin &&
	       r.y + r.h <= SCREEN_H - margin;
}

static void test_now_layout(void)
{
	/* The system font's line feed is 30: t_titulo 23, t_cuerpo 15,
	 * t_meta 14 (ui_line_height). Also try a taller font. */
	static const int heights[][3] = { { 23, 15, 14 }, { 27, 18, 16 } };
	for (size_t k = 0; k < sizeof(heights) / sizeof(heights[0]); k++) {
		int title_h = heights[k][0], body_h = heights[k][1];
		int meta_h = heights[k][2];

		/* 7.5: 160x160 cover, e3 from the top and left edges, its 3 px
		 * shadow included on screen. */
		ui_rect art = now_art_rect();
		assert(art.x == SP_3 && art.y == SP_3);
		assert(art.w == ART_SIZE && art.h == ART_SIZE);
		ui_rect shadow = ui_rect_offset(art, ART_SHADOW_OFF,
		                                ART_SHADOW_OFF);
		assert(on_top_screen(art, SP_3) && on_top_screen(shadow, SP_3));

		/* The text column: right of the cover and its shadow, up to
		 * e3 from the right edge. */
		ui_rect text = { now_text_x(), SP_3, now_text_w(),
		                 now_album_cy(title_h, body_h) + body_h / 2 + 1 -
		                 SP_3 };
		assert(text.x == SP_3 + ART_SIZE + SP_4);
		assert(text.x + text.w == SCREEN_TOP_W - SP_3);
		assert(!overlaps(text, shadow) && on_top_screen(text, SP_3));
		int title_bottom = now_title_cy(title_h) + (title_h + 1) / 2;
		int artist_top = now_artist_cy(title_h, body_h) - body_h / 2;
		assert(artist_top - title_bottom >= SP_1 - 1);
		assert(now_album_cy(title_h, body_h) -
		       now_artist_cy(title_h, body_h) == body_h);
		assert(text.y + text.h <= art.y + art.h);
		/* The detail card's lines follow on, one t_cuerpo line each, and
		 * all NOW_LINES of them still end beside the cover. */
		assert(now_line_cy(title_h, body_h, 0) ==
		       now_artist_cy(title_h, body_h));
		assert(now_line_cy(title_h, body_h, 1) ==
		       now_album_cy(title_h, body_h));
		for (int n = 1; n < NOW_LINES; n++)
			assert(now_line_cy(title_h, body_h, n) -
			       now_line_cy(title_h, body_h, n - 1) == body_h);
		assert(now_line_cy(title_h, body_h, NOW_LINES - 1) + body_h / 2 + 1 <=
		       art.y + art.h);

		/* The detail card: the cover centred on the screen's height,
		 * shadow included on screen, and the text block centred on it. */
		ui_rect dart = now_detail_art_rect();
		assert(dart.x == art.x && dart.w == art.w && dart.h == art.h);
		assert(dart.y == SCREEN_H - dart.y - dart.h);
		assert(on_top_screen(ui_rect_offset(dart, ART_SHADOW_OFF,
		                                    ART_SHADOW_OFF), SP_3));
		for (int count = 0; count <= NOW_LINES; count++) {
			int dy = now_detail_text_dy(title_h, body_h, count);
			int top = now_title_cy(title_h) + dy - title_h / 2;
			int bottom = count ? now_line_cy(title_h, body_h, count - 1) +
			                     dy + body_h / 2
			                   : top + title_h;
			int above = top - dart.y, below = dart.y + dart.h - bottom;
			assert(above >= 0 && below >= 0);
			assert(above - below <= 2 && below - above <= 2);
		}

		/* Times e4 under the cover, the bar below with its handle
		 * clear of their line, everything inside e3. */
		int times_top = now_times_cy(meta_h) - meta_h / 2;
		assert(times_top == art.y + art.h + SP_4);
		assert(times_top > shadow.y + shadow.h);
		ui_rect bar = now_progress_rect(meta_h);
		assert(bar.h == PROGRESS_H);
		assert(bar.x == SP_3 && bar.x + bar.w == SCREEN_TOP_W - SP_3);
		ui_rect handle = { bar.x, bar.y + bar.h / 2 - HANDLE_H / 2,
		                   HANDLE_W, HANDLE_H };
		assert(handle.y >= times_top + meta_h);
		assert(on_top_screen(handle, SP_3) && on_top_screen(bar, SP_3));
	}

	/* The big cover: centred, e3 above and below. */
	ui_rect big = now_big_art_rect();
	assert(big.w == ART_BIG && big.h == ART_BIG);
	assert(big.x == 92 && big.y == SP_3);
	assert(big.x * 2 + big.w == SCREEN_TOP_W);
	assert(big.y + big.h == SCREEN_H - SP_3);
	ui_rect big_shadow = ui_rect_offset(big, ART_SHADOW_OFF, ART_SHADOW_OFF);
	assert(on_top_screen(big_shadow, 0));
}

static void test_list_bubbles(void)
{
	/* 5.9: centred SP_2 above the button bar; a second bubble stacks
	 * above, clear of the first one's shadow and under the tab bar. */
	ui_rect first = list_bubble_rect(100.0f);
	assert(first.h == BUBBLE_H && first.w == 100 + 2 * SP_3);
	assert(first.y + first.h + SP_2 == SCREEN_H - BAR_H);
	assert(first.x * 2 + first.w == SCREEN_BOTTOM_W);
	ui_rect same = list_bubble_slot_rect(100.0f, 0);
	assert(same.x == first.x && same.y == first.y && same.w == first.w);
	ui_rect second = list_bubble_slot_rect(60.0f, 1);
	assert(second.y + second.h + SHADOW_OFF + SP_2 == first.y);
	assert(!overlaps(ui_rect_offset(second, SHADOW_OFF, SHADOW_OFF), first));
	assert(second.y > TAB_H);
}

static void test_spinner(void)
{
	/* A 44 px ring of 8 blocks centred on the top screen: whole pixels,
	 * inside the screen, no two blocks touching, the centre empty. */
	int cx = SCREEN_TOP_W / 2, cy = SCREEN_H / 2;
	assert(SPINNER_SIZE == 44);
	ui_rect box = { cx - SPINNER_SIZE / 2, cy - SPINNER_SIZE / 2,
	                SPINNER_SIZE, SPINNER_SIZE };
	assert(box.x * 2 + box.w == SCREEN_TOP_W);
	assert(box.y * 2 + box.h == SCREEN_H);
	ui_rect centre = { box.x + SPINNER_CELL + SPINNER_GAP,
	                   box.y + SPINNER_CELL + SPINNER_GAP, SPINNER_CELL,
	                   SPINNER_CELL };
	for (int i = 0; i < SPINNER_CELLS; i++) {
		ui_rect a = spinner_cell_rect(cx, cy, i);
		assert(a.w == SPINNER_CELL && a.h == SPINNER_CELL);
		assert(a.x >= box.x && a.y >= box.y);
		assert(a.x + a.w <= box.x + box.w && a.y + a.h <= box.y + box.h);
		assert(!overlaps(a, centre));
		for (int j = i + 1; j < SPINNER_CELLS; j++)
			assert(!overlaps(a, spinner_cell_rect(cx, cy, j)));
		/* Clockwise: each block is a grid step from the previous. */
		ui_rect b = spinner_cell_rect(cx, cy, i + 1);
		int dx = b.x - a.x, dy = b.y - a.y;
		int pitch = SPINNER_CELL + SPINNER_GAP;
		assert((dx == 0) != (dy == 0));
		assert(dx == 0 || dx == pitch || dx == -pitch);
		assert(dy == 0 || dy == pitch || dy == -pitch);
	}
	/* The backdrop: SPINNER_PAD px around the ring, centred, translucent. */
	ui_rect back = spinner_backdrop_rect(cx, cy);
	assert(back.w == SPINNER_SIZE + 2 * SPINNER_PAD && back.h == back.w);
	assert(back.x == box.x - SPINNER_PAD && back.y == box.y - SPINNER_PAD);
	assert(back.x * 2 + back.w == SCREEN_TOP_W);
	assert(back.y * 2 + back.h == SCREEN_H);
	assert((SCRIM_COLOR >> 24) == SCRIM_ALPHA);
	assert(SCRIM_ALPHA > 0 && SCRIM_ALPHA < 0xFF);
	assert((SCRIM_COLOR & 0x00FFFFFFu) == (SCRIM_RGB & 0x00FFFFFFu));
	/* The top-left block, then right along the top. */
	assert(spinner_cell_rect(cx, cy, 0).x == box.x);
	assert(spinner_cell_rect(cx, cy, 1).y == box.y);
	assert(spinner_lit(0) == 0 && spinner_lit(9) == 1 &&
	       spinner_lit(-1) == SPINNER_CELLS - 1);

	/* Only while a track loads or plays with nothing downloaded. */
	assert(top_shows_spinner(PLAYER_PHASE_LOADING, 0));
	assert(top_shows_spinner(PLAYER_PHASE_PLAYING, 0));
	assert(!top_shows_spinner(PLAYER_PHASE_LOADING, 1));
	assert(!top_shows_spinner(PLAYER_PHASE_PLAYING, 100));
	assert(!top_shows_spinner(PLAYER_PHASE_IDLE, 0));
	assert(!top_shows_spinner(PLAYER_PHASE_FINISHED, 0));
	assert(!top_shows_spinner(PLAYER_PHASE_FAILED, 0));
}

static void test_top_mode(void)
{
	static const player_phase phases[] = {
		PLAYER_PHASE_IDLE, PLAYER_PHASE_LOADING, PLAYER_PHASE_PLAYING,
		PLAYER_PHASE_FINISHED, PLAYER_PHASE_FAILED
	};
	for (int tab = 0; tab < TAB_COUNT; tab++)
		for (size_t i = 0; i < sizeof(phases) / sizeof(phases[0]); i++) {
			player_phase phase = phases[i];
			bool music = phase == PLAYER_PHASE_LOADING ||
			             phase == PLAYER_PHASE_PLAYING ||
			             phase == PLAYER_PHASE_FINISHED;
			assert(top_music_on(phase) == music);
			/* A detail on the tab takes the top screen, music or not. */
			assert(top_mode_for((app_tab)tab, phase, true) == TOP_DETAIL);
			top_mode mode = top_mode_for((app_tab)tab, phase, false);
			if (tab == TAB_SETTINGS)
				assert(mode == TOP_ABOUT);
			else if (!music)
				assert(mode == TOP_LOGO);
			else if (tab == TAB_PLAYER)
				assert(mode == TOP_ART_BIG);
			else
				assert(mode == TOP_NOW_CARD);
		}
}

static void test_http_redirects(void)
{
	const char *from = "https://audio4-fa.scdn.co/audio/abc?sig=1";
	char out[128];

	assert(http_status_is_redirect(301));
	assert(http_status_is_redirect(302));
	assert(http_status_is_redirect(307));
	assert(http_status_is_redirect(308));
	assert(!http_status_is_redirect(200));
	assert(!http_status_is_redirect(206));
	assert(!http_status_is_redirect(304));
	assert(!http_status_is_redirect(403));

	assert(http_redirect_url(from, " https://edge.example/a?b=1\r\nVia: x",
	                         out, sizeof(out)));
	assert(strcmp(out, "https://edge.example/a?b=1") == 0);
	assert(http_redirect_url(from, "//edge.example/a  \r\n", out,
	                         sizeof(out)));
	assert(strcmp(out, "https://edge.example/a") == 0);
	assert(http_redirect_url(from, "/audio/def?sig=2", out, sizeof(out)));
	assert(strcmp(out, "https://audio4-fa.scdn.co/audio/def?sig=2") == 0);
	assert(http_redirect_url("https://host.example", "/p", out,
	                         sizeof(out)));
	assert(strcmp(out, "https://host.example/p") == 0);

	assert(!http_redirect_url(from, "http://edge.example/a", out,
	                          sizeof(out)));
	assert(!http_redirect_url(from, "relative/path", out, sizeof(out)));
	assert(!http_redirect_url(from, "  \r\n", out, sizeof(out)));
	assert(!http_redirect_url(from, "https://edge.example/a", out, 10));
}

static void test_power_policy(void)
{
	assert(power_keep_awake(PLAYER_PHASE_LOADING, false, true));
	assert(power_keep_awake(PLAYER_PHASE_PLAYING, false, true));
	assert(!power_keep_awake(PLAYER_PHASE_PLAYING, true, true));
	assert(!power_keep_awake(PLAYER_PHASE_IDLE, false, true));
	assert(!power_keep_awake(PLAYER_PHASE_FINISHED, false, true));
	assert(!power_keep_awake(PLAYER_PHASE_FAILED, false, true));
	/* Speakers go mute under a shut lid: no headphones, normal sleep. */
	assert(!power_keep_awake(PLAYER_PHASE_PLAYING, false, false));
	assert(!power_keep_awake(PLAYER_PHASE_LOADING, false, false));

	assert(power_screens_lit(true, true));
	assert(power_screens_lit(true, false));
	assert(!power_screens_lit(false, true));
	/* Music over with the lid shut: lit again before sleeping. */
	assert(power_screens_lit(false, false));
}

static void test_download_retry_delay(void)
{
	assert(download_retry_delay_ms(1) == 1000);
	assert(download_retry_delay_ms(2) == 2000);
	assert(download_retry_delay_ms(3) == 4000);
	assert(download_retry_delay_ms(4) == 8000);
	assert(download_retry_delay_ms(DOWNLOAD_MAX_FAILED_PASSES) == 8000);
	unsigned total = 0;
	for (int i = 1; i < DOWNLOAD_MAX_FAILED_PASSES; i++)
		total += download_retry_delay_ms(i);
	/* Long enough to ride out waking from sleep, short of hanging. */
	assert(total >= 30000 && total <= 90000);
}

static void test_login_qr_url(void)
{
	char url[64];
	/* Spotify's own link wins, as it is. */
	assert(login_qr_url("https://spotify.com/pair",
	                    "https://spotify.com/pair?code=JDDHV3", "JDDHV3",
	                    url, sizeof(url)));
	assert(strcmp(url, "https://spotify.com/pair?code=JDDHV3") == 0);
	/* Without it, the base URL plus the user code. */
	assert(login_qr_url("https://spotify.com/pair", "", "JDDHV3", url,
	                    sizeof(url)));
	assert(strcmp(url, "https://spotify.com/pair?code=JDDHV3") == 0);
	assert(login_qr_url("https://x.example/pair?lang=es", NULL, "AB-12",
	                    url, sizeof(url)));
	assert(strcmp(url, "https://x.example/pair?lang=es&code=AB-12") == 0);
	/* A code that would need escaping, or nothing at all: no QR. */
	assert(!login_qr_url("https://spotify.com/pair", "", "a&b", url,
	                     sizeof(url)) && url[0] == '\0');
	assert(!login_qr_url("https://spotify.com/pair", "", "", url,
	                     sizeof(url)));
	assert(!login_qr_url("", "", "JDDHV3", url, sizeof(url)));
	assert(!login_qr_url(NULL, NULL, NULL, url, sizeof(url)));
	/* Exactly full fits; one byte short is refused, never cut. */
	const char *full = "https://spotify.com/pair?code=JDDHV3";
	char exact[37];
	assert(strlen(full) + 1 == sizeof(exact));
	assert(login_qr_url(NULL, full, NULL, exact, sizeof(exact)));
	assert(strcmp(exact, full) == 0);
	assert(!login_qr_url(NULL, full, NULL, exact, sizeof(exact) - 1));
	assert(exact[0] == '\0');
	assert(!login_qr_url("https://spotify.com/pair", "", "JDDHV3", exact,
	                     sizeof(exact) - 1));
	assert(!login_qr_url(NULL, full, NULL, url, 0));

	/* The URL to type: no scheme, no www, no trailing slash. */
	assert(login_short_url("https://spotify.com/pair", url, sizeof(url)));
	assert(strcmp(url, "spotify.com/pair") == 0);
	assert(login_short_url("http://www.spotify.com/pair/", url,
	                       sizeof(url)));
	assert(strcmp(url, "spotify.com/pair") == 0);
	assert(login_short_url("spotify.com/pair", url, sizeof(url)));
	assert(strcmp(url, "spotify.com/pair") == 0);
	assert(!login_short_url("https://", url, sizeof(url)) && !url[0]);
	assert(!login_short_url(NULL, url, sizeof(url)));
	char small[16];
	assert(!login_short_url("https://spotify.com/pair", small,
	                        sizeof(small)));
	char fits[17];
	assert(login_short_url("https://spotify.com/pair", fits, sizeof(fits)));
}

static void test_login_qr_fits(void)
{
	/* 1 px per module and a 4 module margin, in whole pixels. */
	assert(login_qr_modules(1) == 21 && login_qr_modules(3) == 29);
	assert(login_qr_scale(29, LOGIN_QR_BOX) == 4);
	assert(login_qr_scale(25, LOGIN_QR_BOX) == 4);
	assert(login_qr_scale(33, LOGIN_QR_BOX) == 3);
	assert(login_qr_scale(0, LOGIN_QR_BOX) == 0);
	assert(login_qr_scale(200, LOGIN_QR_BOX) == 0);
	int max_v = login_qr_max_version(LOGIN_QR_BOX, LOGIN_QR_MIN_SCALE);
	assert(max_v >= 3);
	assert(login_qr_scale(login_qr_modules(max_v), LOGIN_QR_BOX) >=
	       LOGIN_QR_MIN_SCALE);
	assert(max_v == 40 ||
	       login_qr_scale(login_qr_modules(max_v + 1), LOGIN_QR_BOX) <
	       LOGIN_QR_MIN_SCALE);

	/* The real link (phase 0: 36 bytes) is a version 3 code at 4 px. */
	static uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
	static uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
	assert(qrcodegen_encodeText("https://spotify.com/pair?code=JDDHV3",
	                            temp, qr, qrcodegen_Ecc_LOW,
	                            qrcodegen_VERSION_MIN, max_v,
	                            qrcodegen_Mask_AUTO, true));
	int modules = qrcodegen_getSize(qr);
	assert(modules == login_qr_modules(3));
	int scale = login_qr_scale(modules, LOGIN_QR_BOX);
	assert(scale >= 4);
	/* Centred in the box, with at least the quiet zone around it. */
	ui_rect box = login_qr_rect();
	int ox = login_qr_origin(box.x, modules, scale);
	int oy = login_qr_origin(box.y, modules, scale);
	assert(ox - box.x >= LOGIN_QR_QUIET * scale);
	assert(oy - box.y >= LOGIN_QR_QUIET * scale);
	assert((box.x + box.w) - (ox + modules * scale) >=
	       LOGIN_QR_QUIET * scale);
	assert((box.y + box.h) - (oy + modules * scale) >=
	       LOGIN_QR_QUIET * scale);
	/* A link too long for the minimum scale is refused, not shrunk. */
	char huge[400];
	memset(huge, 'a', sizeof(huge) - 1);
	huge[sizeof(huge) - 1] = '\0';
	assert(!qrcodegen_encodeText(huge, temp, qr, qrcodegen_Ecc_LOW,
	                             qrcodegen_VERSION_MIN, max_v,
	                             qrcodegen_Mask_AUTO, true));
}

static bool inside_screen(ui_rect r, int width)
{
	return r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 &&
	       r.x + r.w <= width && r.y + r.h <= SCREEN_H;
}

static void test_login_layout(void)
{
	/* Top: the logo above the spinner, both centred, never touching. */
	ui_rect logo = login_logo_rect(), status = login_status_rect();
	assert(inside_screen(logo, SCREEN_TOP_W));
	assert(inside_screen(status, SCREEN_TOP_W));
	assert(logo.x * 2 + logo.w == SCREEN_TOP_W);
	assert(status.x * 2 + status.w == SCREEN_TOP_W);
	assert(logo.y + logo.h + SP_2 <= status.y);
	assert(spinner_cell_rect(SCREEN_TOP_W / 2, LOGIN_STATUS_CY, 0).y ==
	       status.y);
	/* Pairing: heading, then the panel to the bottom margin. */
	ui_rect heading = login_pair_heading_rect();
	ui_rect panel = login_pair_panel_rect();
	assert(inside_screen(heading, SCREEN_TOP_W));
	assert(inside_screen(panel, SCREEN_TOP_W));
	assert(heading.x == SP_3 && panel.x == SP_3);
	assert(panel.x * 2 + panel.w == SCREEN_TOP_W);
	assert(!overlaps(heading, panel));
	assert(panel.y + panel.h == SCREEN_H - SP_3);

	/* Bottom: nothing reaches the button bar, shadows included. */
	ui_rect hints = login_hints_rect();
	assert(hints.y + hints.h == SCREEN_H && hints.h == BAR_H);
	assert(hints.w == SCREEN_BOTTOM_W);
	ui_rect content = login_content_rect();
	assert(inside_screen(content, SCREEN_BOTTOM_W));
	assert(content.y + content.h <= hints.y);
	ui_rect qr = login_qr_rect();
	assert(qr.w == LOGIN_QR_BOX && qr.h == LOGIN_QR_BOX);
	assert(qr.x == SP_2);
	/* Its 1 px outline and hard shadow on screen and off the bar. */
	assert(qr.x - OUTLINE_W >= 0 && qr.y - OUTLINE_W >= 0);
	assert(qr.y + qr.h + OUTLINE_W + SHADOW_OFF + SP_1 <= hints.y);
	/* Centred in the content height (feature_plan.md: y 34). */
	assert(qr.y == 34 && qr.y * 2 + qr.h == hints.y);
	ui_rect side = login_pair_side_rect();
	assert(inside_screen(side, SCREEN_BOTTOM_W));
	assert(side.x + side.w == SCREEN_BOTTOM_W - SP_2);
	assert(side.x - (qr.x + qr.w + OUTLINE_W + SHADOW_OFF) >= SP_1);
	assert(side.w == 144);
	assert(side.y + side.h <= hints.y);

	/* Error: two 32 px buttons, SP_4 apart, focus rings and shadows
	 * clear of each other, of the notice and of the bar (4.4, 6.2). */
	int ring = FOCUS_GAP + FOCUS_W;
	int below = ring > SHADOW_OFF ? ring : SHADOW_OFF;
	for (int i = 0; i < LOGIN_BUTTON_COUNT; i++) {
		ui_rect b = login_button_rect(i);
		assert(b.h >= TOUCH_MIN && b.w >= TOUCH_MIN);
		assert(inside_screen(b, SCREEN_BOTTOM_W));
		assert(b.x - ring >= 0);
		assert(b.x + b.w + below + PRESS_SHIFT <= SCREEN_BOTTOM_W);
		assert(b.y + b.h + below + PRESS_SHIFT <= hints.y);
		assert(login_hit_test(b.x, b.y) == (login_button)i);
		assert(login_hit_test(b.x + b.w - 1, b.y + b.h - 1) ==
		       (login_button)i);
	}
	ui_rect retry = login_button_rect(LOGIN_BUTTON_RETRY);
	ui_rect pair = login_button_rect(LOGIN_BUTTON_PAIR);
	assert(retry.y == pair.y && retry.w == pair.w);
	assert(retry.x == SP_2 && pair.x + pair.w == SCREEN_BOTTOM_W - SP_2);
	assert((pair.x - ring) - (retry.x + retry.w + below) >= SP_1);
	for (int with = 0; with <= 1; with++) {
		ui_rect notice = login_notice_rect(with);
		assert(inside_screen(notice, SCREEN_BOTTOM_W));
		assert(notice.x == SP_2 && notice.w == SCREEN_BOTTOM_W - 2 * SP_2);
		int notice_bottom = notice.y + notice.h + SHADOW_OFF;
		assert(notice_bottom + SP_1 <= hints.y);
		if (with)
			assert(notice_bottom + SP_1 <= retry.y - ring);
		/* Room for the title and three lines of body at least. */
		assert(notice.h >= 64);
	}
	assert(login_notice_rect(false).h > login_notice_rect(true).h);
	/* The gap between them, the notice and the bar are not buttons. */
	assert(login_hit_test(retry.x + retry.w + 1, retry.y) ==
	       LOGIN_BUTTON_NONE);
	assert(login_hit_test(10, 10) == LOGIN_BUTTON_NONE);
	assert(login_hit_test(10, 230) == LOGIN_BUTTON_NONE);
	/* Focus stops at the ends; anything else falls back to Retry. */
	assert(login_focus_move(LOGIN_BUTTON_RETRY, 1) == LOGIN_BUTTON_PAIR);
	assert(login_focus_move(LOGIN_BUTTON_PAIR, 1) == LOGIN_BUTTON_PAIR);
	assert(login_focus_move(LOGIN_BUTTON_RETRY, -1) == LOGIN_BUTTON_RETRY);
	assert(login_focus_move(LOGIN_BUTTON_NONE, 0) == LOGIN_BUTTON_RETRY);
}

static void test_login_steps(void)
{
	/* Renewing the stored credential: five steps. */
	assert(login_step_total(false) == 5);
	assert(login_step(STR_CREDENTIAL_RENEWING, false) == 1);
	assert(login_step(STR_AP_SEARCHING, false) == 2);
	assert(login_step(STR_AP_AUTHENTICATING, false) == 3);
	assert(login_step(STR_LIBRARY_OPENING, false) == 4);
	assert(login_step(STR_SERVER_RESOLVING, false) == 5);
	/* Pairing adds one: the user linking the account. */
	assert(login_step_total(true) == 6);
	assert(login_step(STR_PAIR_REQUESTING, true) == 1);
	assert(login_step(STR_AP_SEARCHING, true) == 3);
	assert(login_step(STR_SERVER_RESOLVING, true) ==
	       login_step_total(true));
	/* Not a step: no counter. */
	assert(login_step(STR_LOGIN_CONNECTING, false) == 0);
}

static void test_login_notice_and_hints(void)
{
	/* One line of body; then one more than the division; capped. */
	int base = login_notice_height(0, 280, 15, 1000);
	assert(base == 2 * OUTLINE_W + 2 * SP_3 + ICON_S + SP_2 + 15);
	assert(login_notice_height(100, 280, 15, 1000) == base + 15);
	assert(login_notice_height(560, 280, 15, 1000) == base + 2 * 15);
	assert(login_notice_height(561, 280, 15, 1000) == base + 3 * 15);
	int cap = login_notice_rect(true).h;
	assert(login_notice_height(100000, 280, 15, cap) == cap);
	assert(login_notice_height(100, 0, 15, 1000) == base);

	/* Everything fits: every label shows. */
	int glyph[4] = { 14, 14, 40, 46 }, label[4] = { 30, 30, 20, 40 };
	bool show[4];
	assert(login_hints_fit(glyph, label, 4, SCREEN_BOTTOM_W, show));
	for (int i = 0; i < 4; i++)
		assert(show[i]);
	/* Too wide: labels go from the last one back, glyphs stay. With
	 * these: 8 + (14+4+60+12) + (14+4+54+12) + (40+4+30+12)
	 * + (46+4+42) = 360 with every label, 314 without SELECT's, 280
	 * without START's too, 222 with only A's; against width - SP_2. */
	int wide[4] = { 60, 54, 30, 42 };
	assert(login_hints_fit(glyph, wide, 4, 330, show));
	assert(show[0] && show[1] && show[2] && !show[3]);
	assert(login_hints_fit(glyph, wide, 4, SCREEN_BOTTOM_W, show));
	assert(show[0] && show[1] && !show[2] && !show[3]);
	assert(login_hints_fit(glyph, wide, 4, 270, show));
	assert(show[0] && !show[1] && !show[2] && !show[3]);
	/* Not even the glyphs: false. */
	assert(!login_hints_fit(glyph, wide, 4, 100, show));
	assert(login_hints_fit(glyph, wide, 0, 100, show));
}

static void test_recent_searches(void)
{
	recent_list r = { .count = 0 };
	char out[RECENT_SERIAL_SIZE];

	/* Blank queries are not remembered. */
	assert(!recent_push(&r, "   "));
	assert(!recent_push(&r, ""));
	assert(r.count == 0);
	assert(recent_serialize(&r, out, sizeof(out)) && out[0] == '\0');

	/* Newest first; the oldest falls off past RECENT_MAX. */
	assert(recent_push(&r, "one"));
	assert(recent_push(&r, " two "));
	assert(recent_push(&r, "three"));
	assert(recent_push(&r, "four"));
	assert(r.count == RECENT_MAX);
	assert(strcmp(r.items[0], "four") == 0);
	assert(strcmp(r.items[1], "three") == 0);
	assert(strcmp(r.items[2], "two") == 0);

	/* A repeat moves up, whatever its case, and shows as typed last. */
	assert(recent_push(&r, "TWO"));
	assert(r.count == RECENT_MAX);
	assert(strcmp(r.items[0], "TWO") == 0);
	assert(strcmp(r.items[1], "four") == 0);
	assert(strcmp(r.items[2], "three") == 0);
	assert(recent_push(&r, "TWO"));
	assert(strcmp(r.items[0], "TWO") == 0 && r.count == RECENT_MAX);

	/* Control characters could break the config line or the separator. */
	recent_list c = { .count = 0 };
	assert(recent_push(&c, "a\x1f" "b\nc"));
	assert(strcmp(c.items[0], "a b c") == 0);

	/* Round trip through the config value. */
	assert(recent_serialize(&r, out, sizeof(out)));
	assert(strcmp(out, "TWO\x1f" "four\x1f" "three") == 0);
	recent_list back;
	recent_parse(&back, out);
	assert(back.count == RECENT_MAX);
	for (int i = 0; i < RECENT_MAX; i++)
		assert(strcmp(back.items[i], r.items[i]) == 0);
	assert(!recent_serialize(&r, out, 6));

	/* A value edited by hand: blanks, repeats and extras are dropped. */
	recent_parse(&back, "x\x1f\x1fX\x1fy\x1fz");
	assert(back.count == 1 && strcmp(back.items[0], "x") == 0);
	recent_parse(&back, NULL);
	assert(back.count == 0);
	recent_parse(&back, "solo");
	assert(back.count == 1 && strcmp(back.items[0], "solo") == 0);

	/* Full-length queries survive the round trip. */
	recent_list l = { .count = 0 };
	char longq[RECENT_QUERY_SIZE];
	for (int i = 0; i < RECENT_MAX; i++) {
		memset(longq, 'a' + i, sizeof(longq) - 1);
		longq[sizeof(longq) - 1] = '\0';
		assert(recent_push(&l, longq));
	}
	assert(recent_serialize(&l, out, sizeof(out)));
	recent_parse(&back, out);
	assert(back.count == RECENT_MAX);
	for (int i = 0; i < RECENT_MAX; i++)
		assert(strcmp(back.items[i], l.items[i]) == 0);

	/* The idle layout: button, label and rows between the bars. */
	ui_rect button = list_idle_button_rect(100);
	assert(button.y >= list_top() + SP_1);
	assert(button.x * 2 + button.w == SCREEN_BOTTOM_W);
	assert(list_recent_label_cy() > button.y + button.h);
	ui_rect first = list_recent_rect(0);
	assert(first.y > list_recent_label_cy());
	assert(first.h >= TOUCH_MIN);
	for (int i = 1; i < RECENT_MAX; i++)
		assert(list_recent_rect(i).y >= list_recent_rect(i - 1).y +
		       list_recent_rect(i - 1).h);
	ui_rect last = list_recent_rect(RECENT_MAX - 1);
	assert(last.y + last.h + SHADOW_OFF + SP_1 <= list_hints_rect().y);
}

static void test_art_cache(void)
{
	/* A free slot first, else the least recently used. */
	bool used[4] = { true, false, true, true };
	unsigned stamps[4] = { 5, 0, 2, 9 };
	assert(art_cache_victim(used, stamps, 4) == 1);
	/* The oldest in use, for the byte budget, skips the free slot. */
	assert(art_cache_oldest(used, stamps, 4) == 2);
	bool none[2] = { false, false };
	assert(art_cache_oldest(none, stamps, 2) == -1);
	used[1] = true;
	stamps[1] = 7;
	assert(art_cache_victim(used, stamps, 4) == 2);
	/* Stamps that wrapped round still order by use. */
	unsigned wrapped[2] = { 0xFFFFFFFEu, 1u };
	bool both[2] = { true, true };
	assert(art_cache_victim(both, wrapped, 2) == 0);
	assert(art_cache_victim(both, wrapped, 0) == -1);
}

static void test_text_compose(void)
{
	char out[64];

	/* Plain text, precomposed accents and other scripts pass unchanged. */
	assert(text_compose("Hello", out, sizeof(out)) == 5);
	assert(strcmp(out, "Hello") == 0);
	text_compose("Canci\xc3\xb3n espa\xc3\xb1ola", out, sizeof(out));
	assert(strcmp(out, "Canci\xc3\xb3n espa\xc3\xb1ola") == 0);
	text_compose("\xec\x82\xac\xeb\x9e\x91 \xe7\x88\xb1", out, sizeof(out));
	assert(strcmp(out, "\xec\x82\xac\xeb\x9e\x91 \xe7\x88\xb1") == 0);

	/* Decomposed accents become the one character: e + U+0301 -> é,
	 * n + U+0303 -> ñ, U + U+0308 -> Ü. */
	text_compose("Cafe\xcc\x81 man\xcc\x83" "ana U\xcc\x88", out, sizeof(out));
	assert(strcmp(out, "Caf\xc3\xa9 ma\xc3\xb1" "ana \xc3\x9c") == 0);

	/* A mark with nothing to compose with is dropped, as are variation
	 * selectors; a second mark after a composed letter too. */
	text_compose("x\xcc\x81y", out, sizeof(out));
	assert(strcmp(out, "xy") == 0);
	text_compose("\xcc\x81" "a", out, sizeof(out));
	assert(strcmp(out, "a") == 0);
	text_compose("e\xcc\x81\xcc\x88", out, sizeof(out));
	assert(strcmp(out, "\xc3\xa9") == 0);
	text_compose("\xe2\x9d\xa4\xef\xb8\x8f", out, sizeof(out));
	assert(strcmp(out, "\xe2\x9d\xa4") == 0);

	/* Korean jamo: U+1112 U+1161 U+11AB -> 한 (U+D55C), and without
	 * the final consonant U+1112 U+1161 -> 하 (U+D558). */
	text_compose("\xe1\x84\x92\xe1\x85\xa1\xe1\x86\xab", out, sizeof(out));
	assert(strcmp(out, "\xed\x95\x9c") == 0);
	text_compose("\xe1\x84\x92\xe1\x85\xa1", out, sizeof(out));
	assert(strcmp(out, "\xed\x95\x98") == 0);

	/* Malformed bytes read as U+FFFD, one per byte. */
	text_compose("a\xff" "b", out, sizeof(out));
	assert(strcmp(out, "a\xef\xbf\xbd" "b") == 0);
	text_compose("\xc3", out, sizeof(out));
	assert(strcmp(out, "\xef\xbf\xbd") == 0);

	/* Cut on a character boundary when it does not fit. */
	assert(text_compose("ab\xc3\xa9", out, 4) == 2);
	assert(strcmp(out, "ab") == 0);
	assert(text_compose("abc", out, 1) == 0 && out[0] == '\0');

	/* The round trip of the UTF-8 helpers. */
	uint32_t cps[] = { 0x41, 0xE9, 0x20AC, 0xD55C, 0x1F600 };
	for (size_t i = 0; i < sizeof(cps) / sizeof(cps[0]); i++) {
		char bytes[5] = {0};
		size_t n = text_utf8_encode(cps[i], bytes);
		uint32_t back;
		assert(text_utf8_decode(bytes, &back) == n && back == cps[i]);
	}
	uint32_t cp;
	assert(text_utf8_decode("\xc0\xaf", &cp) == 1 && cp == 0xFFFD);
	assert(text_utf8_decode("\xed\xa0\x80", &cp) == 1 && cp == 0xFFFD);
}

static void test_about_layout(void)
{
	/* The card: inside the margins, the picture inside it. */
	ui_rect card = about_card_rect();
	ui_rect avatar = about_avatar_rect();
	assert(card.x == ABOUT_MARGIN && card.y == ABOUT_MARGIN);
	assert(card.x * 2 + card.w == SCREEN_TOP_W);
	assert(avatar.x > card.x && avatar.y > card.y);
	assert(avatar.x + avatar.w < card.x + card.w);
	assert(avatar.y + avatar.h < card.y + card.h);
	assert(about_card_text_x() > avatar.x + avatar.w);
	assert(about_card_text_x() + about_card_text_w() < card.x + card.w);

	/* The credits: under the card, evenly spaced, in two columns that
	 * end at the right margin. */
	assert(about_row_cy(0) - ABOUT_ROW_PITCH / 2 >= card.y + card.h + SP_2);
	for (int i = 1; i < ABOUT_ROWS; i++)
		assert(about_row_cy(i) - about_row_cy(i - 1) == ABOUT_ROW_PITCH);
	assert(about_value_x() > about_label_x());
	assert(about_value_x() + about_value_w() ==
	       SCREEN_TOP_W - ABOUT_MARGIN);

	/* The rule under the last row, and room for three lines of notice
	 * (t_meta, 14 px) above the bottom edge. */
	ui_rect rule = about_rule_rect();
	assert(rule.y > about_row_cy(ABOUT_ROWS - 1) + ABOUT_ROW_PITCH / 2 - 1);
	assert(about_notice_y() > rule.y);
	assert(about_notice_y() + 3 * 14 <= SCREEN_H);
	assert(about_notice_w() == card.w);
}

/* The downloads index is big: one, static, for every test below. */
static dl_index dl_test_idx, dl_test_back;

static dl_track dl_test_track(const char *uri, const char *name)
{
	dl_track t;
	memset(&t, 0, sizeof(t));
	snprintf(t.uri, sizeof(t.uri), "%s", uri);
	snprintf(t.name, sizeof(t.name), "%s", name);
	return t;
}

static dl_collection dl_test_coll(const char *uri, dl_coll_kind kind)
{
	dl_collection c;
	memset(&c, 0, sizeof(c));
	snprintf(c.uri, sizeof(c.uri), "%s", uri);
	c.kind = kind;
	return c;
}

/* Serializes `idx` the way dl_store.c does, one line after another. */
static size_t dl_test_serialize(const dl_index *idx, char *out, size_t cap)
{
	char line[DL_LINE_MAX];
	int cursor = 0;
	size_t n = 0;
	while (dl_index_line(idx, &cursor, line, sizeof(line))) {
		size_t len = strlen(line);
		assert(n + len + 1 < cap);
		memcpy(out + n, line, len);
		n += len;
		out[n++] = '\n';
	}
	out[n] = '\0';
	return n;
}

static void test_downloads_index_round_trip(void)
{
	static char text[64 * 1024], again[64 * 1024];
	dl_index *idx = &dl_test_idx, *back = &dl_test_back;
	dl_index_clear(idx);
	snprintf(idx->owner, sizeof(idx->owner), "someone");
	idx->has_salt = true;
	for (int i = 0; i < DL_SALT_LEN; i++)
		idx->salt[i] = (uint8_t)(i * 17);

	dl_track full = dl_test_track("spotify:track:4uLU6hMCjMI75M1A2tKUQC",
	                              "Song");
	snprintf(full.artist, sizeof(full.artist), "Artist");
	snprintf(full.album, sizeof(full.album), "Album");
	snprintf(full.art_small, sizeof(full.art_small), "https://i.scdn.co/a");
	snprintf(full.art_large, sizeof(full.art_large), "https://i.scdn.co/b");
	snprintf(full.album_uri, sizeof(full.album_uri), "spotify:album:x");
	snprintf(full.artist_uri, sizeof(full.artist_uri), "spotify:artist:y");
	full.duration_ms = 215000;
	full.track_number = 3;
	full.disc_number = 1;
	full.year = 2004;
	full.is_explicit = true;
	full.format = 1;
	full.size = 4321000;
	for (int i = 0; i < 20; i++)
		full.file_id[i] = (uint8_t)(0xA0 + i);
	int a = dl_add_track(idx, &full, true, 100);
	assert(a == 0);
	idx->tracks[a].has_key = true;
	for (int i = 0; i < DL_WRAPPED_LEN; i++)
		idx->tracks[a].wrapped_key[i] = (uint8_t)(255 - i);
	idx->tracks[a].state = DL_DONE;

	/* Empty fields, and a name carrying the separator and a newline: they
	 * come back as spaces, never as extra fields or lines. */
	dl_track bare = dl_test_track("spotify:track:0000000000000000000002",
	                              "Bad\x1fname\nhere");
	int b = dl_add_track(idx, &bare, false, 200);
	idx->tracks[b].state = DL_FAILED;
	snprintf(idx->tracks[b].error, sizeof(idx->tracks[b].error), "No key");

	dl_collection pl = dl_test_coll("spotify:playlist:p1", DL_COLL_PLAYLIST);
	snprintf(pl.name, sizeof(pl.name), "Mix");
	pl.total = 40;
	int c = dl_add_collection(idx, &pl, 300);
	assert(dl_add_ref(idx, c, b) && dl_add_ref(idx, c, a));

	size_t len = dl_test_serialize(idx, text, sizeof(text));
	assert(strncmp(text, "3dspotify-dl\x1f" "1\n", 15) == 0);
	assert(text[len - 2] == 'E' && text[len - 3] == '\n');

	bool whole = false;
	assert(dl_index_parse(text, len, back, &whole) && whole);
	assert(strcmp(back->owner, "someone") == 0);
	assert(back->has_salt && memcmp(back->salt, idx->salt, DL_SALT_LEN) == 0);
	assert(back->track_count == 2 && back->coll_count == 1 &&
	       back->ref_count == 2);
	const dl_track *t = &back->tracks[0];
	assert(strcmp(t->uri, full.uri) == 0 && strcmp(t->name, "Song") == 0);
	assert(strcmp(t->artist, "Artist") == 0 && strcmp(t->album, "Album") == 0);
	assert(strcmp(t->art_large, "https://i.scdn.co/b") == 0);
	assert(strcmp(t->artist_uri, "spotify:artist:y") == 0);
	assert(t->duration_ms == 215000 && t->track_number == 3 &&
	       t->disc_number == 1 && t->year == 2004 && t->is_explicit);
	assert(t->format == 1 && t->size == 4321000);
	assert(memcmp(t->file_id, full.file_id, 20) == 0);
	assert(t->has_key &&
	       memcmp(t->wrapped_key, idx->tracks[0].wrapped_key,
	              DL_WRAPPED_LEN) == 0);
	assert(t->state == DL_DONE && t->standalone && t->added_at == 100);
	assert(strcmp(back->tracks[1].name, "Bad name here") == 0);
	assert(back->tracks[1].state == DL_FAILED && !back->tracks[1].has_key);
	assert(strcmp(back->tracks[1].error, "No key") == 0);
	assert(back->tracks[1].artist[0] == '\0');
	assert(back->colls[0].kind == DL_COLL_PLAYLIST &&
	       strcmp(back->colls[0].name, "Mix") == 0 &&
	       back->colls[0].total == 40 && back->colls[0].added_at == 300);
	/* The refs keep the collection's order. */
	int order[4];
	assert(dl_collection_tracks(back, 0, order, 4) == 2);
	assert(order[0] == 1 && order[1] == 0);
	assert(back->next_seq == idx->next_seq);

	/* Serializing what was read gives the same bytes. */
	size_t len2 = dl_test_serialize(back, again, sizeof(again));
	assert(len2 == len && memcmp(text, again, len) == 0);
}

static void test_downloads_index_damage(void)
{
	dl_index *idx = &dl_test_idx;
	bool whole = true;

	/* Another version, or not this file at all: refused whole. */
	const char *v2 = "3dspotify-dl\x1f" "2\nT\x1fspotify:track:a\nE\n";
	assert(!dl_index_parse(v2, strlen(v2), idx, &whole));
	assert(idx->track_count == 0 && !whole);
	const char *junk = "hello\nE\n";
	assert(!dl_index_parse(junk, strlen(junk), idx, &whole));
	assert(!dl_index_parse("", 0, idx, &whole));

	/* Lines not understood are skipped; the rest stays. A line without the
	 * separator, an unknown kind, a track with no URI, a ref to nothing, a
	 * repeated track, and a bad hex key (the track stays, keyless). */
	const char *text =
		"3dspotify-dl\x1f" "1\r\n"
		"garbage\n"
		"X\x1f" "what\n"
		"T\x1f\x1f" "No uri\n"
		"T\x1fspotify:track:a\x1f" "A\n"
		"T\x1fspotify:track:a\x1f" "Again\n"
		"T\x1fspotify:track:b\x1f" "B\x1f\x1f\x1f\x1f\x1f\x1f\x1f"
		"1\x1f" "2\x1f" "3\x1f" "4\x1f" "0\x1f" "1\x1f" "99\x1f"
		"zz\x1fnothex\x1f" "3\x1f" "0\x1f" "5\x1f" "7\x1f\n"
		"R\x1fspotify:album:none\x1fspotify:track:a\n"
		"C\x1fspotify:album:c\x1f" "a\x1f" "Album\n"
		"R\x1fspotify:album:c\x1fspotify:track:b\n"
		"R\x1fspotify:album:c\x1fspotify:track:missing\n";
	assert(dl_index_parse(text, strlen(text), idx, &whole));
	assert(!whole);              /* no E line: written only in part */
	assert(idx->track_count == 2 && idx->coll_count == 1 &&
	       idx->ref_count == 1);
	assert(strcmp(idx->tracks[0].name, "A") == 0);
	assert(idx->tracks[0].state == DL_QUEUED);
	/* A done track without its key cannot play: back to the queue. */
	assert(!idx->tracks[1].has_key && idx->tracks[1].state == DL_QUEUED);
	assert(idx->tracks[1].size == 99 && idx->tracks[1].seq == 7);
	assert(idx->next_seq == 8);
	assert(idx->colls[0].kind == DL_COLL_ALBUM);

	/* A download that was running when the app stopped is queued again;
	 * nothing after the end marker is read. */
	const char *active =
		"3dspotify-dl\x1f" "1\n"
		"T\x1fspotify:track:a\x1f\x1f\x1f\x1f\x1f\x1f\x1f\x1f\x1f\x1f\x1f"
		"\x1f\x1f\x1f\x1f\x1f\x1f" "1\n"
		"E\n"
		"T\x1fspotify:track:after-end\n";
	assert(dl_index_parse(active, strlen(active), idx, &whole) && whole);
	assert(idx->track_count == 1 && idx->tracks[0].state == DL_QUEUED);
}

static void test_downloads_refs(void)
{
	dl_index *idx = &dl_test_idx;
	dl_index_clear(idx);
	dl_track t1 = dl_test_track("spotify:track:1", "1");
	dl_track t2 = dl_test_track("spotify:track:2", "2");
	dl_track t3 = dl_test_track("spotify:track:3", "3");
	dl_track t4 = dl_test_track("spotify:track:4", "4");
	dl_collection album = dl_test_coll("spotify:album:a", DL_COLL_ALBUM);
	dl_collection list = dl_test_coll("spotify:playlist:p", DL_COLL_PLAYLIST);
	int ca = dl_add_collection(idx, &album, 1);
	int cp = dl_add_collection(idx, &list, 2);
	/* 1 only in the album; 2 in both; 3 in the album and on its own; 4
	 * only in the playlist. */
	int a1 = dl_add_track(idx, &t1, false, 1);
	int a2 = dl_add_track(idx, &t2, false, 1);
	int a3 = dl_add_track(idx, &t3, false, 1);
	int a4 = dl_add_track(idx, &t4, false, 1);
	assert(dl_add_track(idx, &t3, true, 9) == a3);    /* already there */
	assert(idx->tracks[a3].standalone && idx->tracks[a3].added_at == 1);
	assert(dl_add_ref(idx, ca, a1) && dl_add_ref(idx, ca, a2) &&
	       dl_add_ref(idx, ca, a3));
	assert(dl_add_ref(idx, cp, a2) && dl_add_ref(idx, cp, a4));
	assert(dl_add_ref(idx, cp, a4) && idx->ref_count == 5);   /* once */
	assert(dl_track_refs(idx, a2) == 2 && dl_track_refs(idx, a3) == 1);

	idx->tracks[a1].state = DL_DONE;
	idx->tracks[a2].state = DL_DONE;
	int done, count;
	dl_collection_progress(idx, ca, &done, &count);
	assert(done == 2 && count == 3);

	/* Removing the album drops track 1 only: 2 is in the playlist, 3 was
	 * downloaded on its own. */
	char orphans[4][SP_URI_MAX];
	assert(dl_remove_collection(idx, ca, orphans, 4) == 1);
	assert(strcmp(orphans[0], "spotify:track:1") == 0);
	assert(idx->coll_count == 1 && idx->track_count == 3);
	assert(dl_find_track(idx, "spotify:track:1") < 0);
	/* The refs still point at the right tracks after the shift. */
	cp = dl_find_collection(idx, "spotify:playlist:p");
	assert(cp == 0);
	int order[4];
	assert(dl_collection_tracks(idx, cp, order, 4) == 2);
	assert(strcmp(idx->tracks[order[0]].uri, "spotify:track:2") == 0);
	assert(strcmp(idx->tracks[order[1]].uri, "spotify:track:4") == 0);

	/* Then the playlist: 2 and 4 go, 3 stays on its own. */
	assert(dl_remove_collection(idx, cp, orphans, 4) == 2);
	assert(idx->coll_count == 0 && idx->ref_count == 0);
	assert(idx->track_count == 1 &&
	       strcmp(idx->tracks[0].uri, "spotify:track:3") == 0);
	assert(dl_remove_collection(idx, 5, orphans, 4) == 0);
}

static void test_downloads_queue_order(void)
{
	dl_index *idx = &dl_test_idx;
	dl_index_clear(idx);
	assert(dl_next_queued(idx) == -1);
	dl_track a = dl_test_track("spotify:track:a", "a");
	dl_track b = dl_test_track("spotify:track:b", "b");
	dl_track c = dl_test_track("spotify:track:c", "c");
	int ia = dl_add_track(idx, &a, true, 1);
	int ib = dl_add_track(idx, &b, true, 1);
	int ic = dl_add_track(idx, &c, true, 1);
	assert(idx->tracks[ia].state == DL_QUEUED);
	assert(dl_next_queued(idx) == ia);

	/* Active and failed are not picked; the next in order is. */
	idx->tracks[ia].state = DL_ACTIVE;
	assert(dl_next_queued(idx) == ib);
	idx->tracks[ib].state = DL_FAILED;
	assert(dl_next_queued(idx) == ic);

	/* A retry goes to the end of the queue. */
	dl_requeue(idx, ib);
	assert(idx->tracks[ib].state == DL_QUEUED);
	assert(dl_next_queued(idx) == ic);
	idx->tracks[ic].state = DL_DONE;
	assert(dl_next_queued(idx) == ib);
	idx->tracks[ib].state = DL_DONE;
	idx->tracks[ia].state = DL_DONE;
	assert(dl_next_queued(idx) == -1);
}

static void test_downloads_owner(void)
{
	assert(dl_owner_matches("", "me"));
	assert(dl_owner_matches(NULL, NULL));
	assert(dl_owner_matches("me", "me"));
	assert(!dl_owner_matches("me", "someone"));
	assert(!dl_owner_matches("me", ""));
	assert(!dl_owner_matches("me", NULL));
}

static void test_downloads_engine_states(void)
{
	dl_index *idx = &dl_test_idx;
	dl_index_clear(idx);
	dl_track a = dl_test_track("spotify:track:a", "a");
	dl_track b = dl_test_track("spotify:track:b", "b");
	int ia = dl_add_track(idx, &a, true, 1);
	int ib = dl_add_track(idx, &b, true, 1);

	/* queued -> active -> done */
	dl_mark_active(idx, ia);
	assert(idx->tracks[ia].state == DL_ACTIVE);
	assert(dl_next_queued(idx) == ib);
	/* A real download is done with its key, or it would be fetched again
	 * on the next load. */
	idx->tracks[ia].has_key = true;
	dl_mark_done(idx, ia, 4000000);
	assert(idx->tracks[ia].state == DL_DONE && idx->tracks[ia].size == 4000000);

	/* active -> stopped: queued again in its place, ahead of later ones. */
	dl_track c = dl_test_track("spotify:track:c", "c");
	int ic = dl_add_track(idx, &c, true, 1);
	dl_mark_active(idx, ib);
	dl_mark_paused(idx, ib);
	assert(idx->tracks[ib].state == DL_QUEUED);
	assert(dl_next_queued(idx) == ib);
	/* Pausing leaves anything not running alone. */
	dl_mark_paused(idx, ia);
	assert(idx->tracks[ia].state == DL_DONE);

	/* The key is kept as soon as it comes, before the file is whole. */
	uint8_t wrapped[DL_WRAPPED_LEN], file_id[20];
	memset(wrapped, 0x5A, sizeof(wrapped));
	memset(file_id, 0x11, sizeof(file_id));
	dl_mark_active(idx, ib);
	dl_set_key(idx, ib, wrapped, file_id, 1);
	dl_mark_paused(idx, ib);
	assert(idx->tracks[ib].has_key && idx->tracks[ib].format == 1);
	assert(memcmp(idx->tracks[ib].file_id, file_id, 20) == 0);

	/* active -> failed, then a retry sends it to the end of the queue. */
	dl_mark_active(idx, ib);
	dl_mark_failed(idx, ib, "Network");
	assert(idx->tracks[ib].state == DL_FAILED &&
	       strcmp(idx->tracks[ib].error, "Network") == 0);
	assert(dl_next_queued(idx) == ic);
	dl_requeue(idx, ib);
	assert(idx->tracks[ib].error[0] == '\0');
	dl_mark_active(idx, ic);
	idx->tracks[ic].has_key = true;
	dl_mark_done(idx, ic, 1);
	assert(dl_next_queued(idx) == ib);

	/* A restart in the middle: active is queued again, with its key. */
	static char text[16 * 1024];
	dl_mark_active(idx, ib);
	dl_test_serialize(idx, text, sizeof(text));
	bool whole = false;
	assert(dl_index_parse(text, strlen(text), &dl_test_back, &whole) && whole);
	int rb = dl_find_track(&dl_test_back, "spotify:track:b");
	assert(dl_test_back.tracks[rb].state == DL_QUEUED);
	assert(dl_test_back.tracks[rb].has_key);
	assert(dl_next_queued(&dl_test_back) == rb);

	/* Cancelling: a track only wanted on its own leaves; one a collection
	 * holds stays for it. */
	dl_collection al = dl_test_coll("spotify:album:x", DL_COLL_ALBUM);
	int cx = dl_add_collection(idx, &al, 1);
	assert(dl_add_ref(idx, cx, ic));
	assert(!dl_unwant_track(idx, ic));
	assert(!idx->tracks[ic].standalone && dl_find_track(idx, c.uri) == ic);
	assert(dl_unwant_track(idx, ia));
	assert(dl_find_track(idx, "spotify:track:a") < 0);
	/* The positions moved down; the ref still names c. */
	int order[2];
	assert(dl_collection_tracks(idx, 0, order, 2) == 1);
	assert(strcmp(idx->tracks[order[0]].uri, "spotify:track:c") == 0);
	assert(!dl_unwant_track(idx, -1) && !dl_unwant_track(idx, 99));
}

static void test_downloads_key_bucket(void)
{
	dl_bucket b;
	uint64_t t = 1000;
	dl_bucket_init(&b, t);
	assert(b.tokens == DL_KEY_BUCKET);

	/* The engine takes down to the reserve, then waits. */
	int taken = 0;
	while (dl_bucket_engine_may_take(&b, t)) {
		dl_bucket_take(&b, t);
		taken++;
	}
	assert(taken == DL_KEY_BUCKET - DL_KEY_RESERVE);
	assert(b.tokens == DL_KEY_RESERVE);
	/* The player still has the reserve; its keys count too. */
	dl_bucket_take(&b, t);
	assert(b.tokens == DL_KEY_RESERVE - 1);

	/* One comes back every DL_KEY_REFILL_MS, whole ones only. */
	dl_bucket_refill(&b, t + DL_KEY_REFILL_MS - 1);
	assert(b.tokens == DL_KEY_RESERVE - 1);
	dl_bucket_refill(&b, t + DL_KEY_REFILL_MS);
	assert(b.tokens == DL_KEY_RESERVE);
	assert(!dl_bucket_engine_may_take(&b, t + DL_KEY_REFILL_MS));
	/* The remainder carries: 1.5 periods later, one more at 2. */
	dl_bucket_refill(&b, t + DL_KEY_REFILL_MS + DL_KEY_REFILL_MS / 2);
	assert(b.tokens == DL_KEY_RESERVE);
	dl_bucket_refill(&b, t + 2 * DL_KEY_REFILL_MS);
	assert(b.tokens == DL_KEY_RESERVE + 1);
	assert(dl_bucket_engine_may_take(&b, t + 2 * DL_KEY_REFILL_MS));
	/* Never above the capacity, however long it rests. */
	dl_bucket_refill(&b, t + 1000ull * DL_KEY_REFILL_MS);
	assert(b.tokens == DL_KEY_BUCKET);

	/* A 0x0002 empties it and blocks the engine for the backoff, even once
	 * the model would have refilled. */
	uint64_t r = t + 2000ull * DL_KEY_REFILL_MS;
	dl_bucket_refused(&b, r);
	assert(b.tokens == 0);
	assert(!dl_bucket_engine_may_take(&b, r + DL_KEY_BACKOFF_MS - 1));
	uint64_t later = r + (uint64_t)(DL_KEY_RESERVE + 1) * DL_KEY_REFILL_MS;
	assert(later >= r + DL_KEY_BACKOFF_MS);
	assert(dl_bucket_engine_may_take(&b, later));
	/* Taking from an empty one stays at zero. */
	dl_bucket_refused(&b, later);
	dl_bucket_take(&b, later);
	assert(b.tokens == 0);
}

static void test_downloads_retry_and_busy(void)
{
	assert(dl_net_retry_ms(0) == 0);
	assert(dl_net_retry_ms(1) == 5000);
	assert(dl_net_retry_ms(2) == 15000);
	assert(dl_net_retry_ms(3) == 60000);
	assert(dl_net_retry_ms(4) == 0);       /* given up: DL_FAILED */

	/* Preparing a track, or downloading the one that plays: busy. */
	assert(dl_player_busy(true, false, false));
	assert(dl_player_busy(true, true, true));
	assert(dl_player_busy(false, true, false));
	/* Its file whole, or nothing playing: the engine may go on. */
	assert(!dl_player_busy(false, true, true));
	assert(!dl_player_busy(false, false, false));
}

static void test_playback_next_playable(void)
{
	/* Five tracks played in a shuffled order; only 1 and 3 downloaded. */
	const int order[5] = { 4, 1, 0, 3, 2 };
	const bool playable[5] = { false, true, false, true, false };
	/* From the start: position 1 (track 1), then 3 (track 3), then none. */
	assert(playback_next_playable(-1, 5, 1, order, playable) == 1);
	assert(playback_next_playable(1, 5, 1, order, playable) == 3);
	assert(playback_next_playable(3, 5, 1, order, playable) == -1);
	/* Backwards the same, and no wrapping at the start. */
	assert(playback_next_playable(4, 5, -1, order, playable) == 3);
	assert(playback_next_playable(3, 5, -1, order, playable) == 1);
	assert(playback_next_playable(1, 5, -1, order, playable) == -1);
	assert(playback_next_playable(0, 5, -1, order, playable) == -1);
	/* From past the end backwards: the last playable one. */
	assert(playback_next_playable(5, 5, -1, order, playable) == 3);
	/* Nothing playable, or nonsense in: none. */
	const bool none[5] = { false };
	assert(playback_next_playable(-1, 5, 1, order, none) == -1);
	assert(playback_next_playable(0, 0, 1, order, playable) == -1);
	assert(playback_next_playable(0, 5, 0, order, playable) == -1);
	assert(playback_next_playable(9, 5, -1, order, playable) == -1);
}

int main(void)
{
	test_login_qr_url();
	test_login_qr_fits();
	test_login_layout();
	test_login_steps();
	test_login_notice_and_hints();
	test_tabs();
	test_settings_layout();
	test_player_layout();
	test_stream_chunks();
	test_player_shared_helpers();
	test_now_layout();
	test_list_bubbles();
	test_recent_searches();
	test_art_cache();
	test_text_compose();
	test_about_layout();
	test_spinner();
	test_top_mode();
	test_list_drag();
	test_grid_layout();
	test_grid_focus();
	test_art_url();
	test_thumb_queue();
	test_list_logic();
	test_layout_rects();
	test_press_timing();
	test_touch_tracking();
	test_theme_colors();
	test_theme_contrast();
	test_text_fit();
	test_marquee();
	test_home_layout();
	test_home_focus();
	test_image_fit();
	test_image_morton_offset();
	test_image_to_texture();
	test_image_pow2();
	test_image_copy_to_texture();
	test_search_path_encoding();
	test_spotify_id();
	test_meta_parse_track();
	test_meta_parse_album();
	test_meta_parse_artist();
	test_meta_parse_batch();
	test_meta_parse_rootlist();
	test_meta_parse_playlist();
	test_meta_parse_rootlist_live();
	test_list_window();
	test_detail_paging();
	test_thumb_reuse();
	test_meta_parse_recent();
	test_home_merge();
	test_detail_stack();
	test_detail_pool();
	test_detail_buttons();
	test_list_frames();
	test_http_redirects();
	test_download_retry_delay();
	test_power_policy();
	test_queue_boundaries();
	test_queue_order();
	test_queue_drop_known();
	test_language_tables();
	test_language_switching();
	test_downloads_index_round_trip();
	test_downloads_index_damage();
	test_downloads_refs();
	test_downloads_queue_order();
	test_downloads_owner();
	test_downloads_engine_states();
	test_downloads_key_bucket();
	test_downloads_retry_and_busy();
	test_download_buttons();
	test_downloads_manager_list();
	test_offline_rows();
	test_playback_next_playable();
	return 0;
}
