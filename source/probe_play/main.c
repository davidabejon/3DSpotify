/* Phase 5a complete: from a Spotify URI to sound from the speakers.
 *
 * Signs in, resolves one track, asks the access point for its audio key,
 * downloads it from the CDN, decrypts it and decodes the Vorbis through NDSP.
 * Every step is timed, because this is the first time the whole chain runs on
 * the console and the interesting question is what it costs.
 *
 * Step 4 came later, for tracks the app refuses with "no Vorbis file this
 * console can play": it lists every audio file the Track and its
 * alternatives carry, uncapped, and says which case the track is in. The
 * track comes from URI_FILE when there is one.
 *
 * The access point session is held open for the key and only for that: it is
 * the one piece no other service will hand over.
 */
#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ap3ds.h"
#include "cfg3ds.h"
#include "http3ds.h"
#include "login5.h"
#include "meta_parse.h"
#include "pair3ds.h"
#include "proto3ds.h"
#include "spclient.h"
#include "spotify_id.h"
#include "track.h"
#include "tls3ds.h"

#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.h"

#define SOC_BUFFER      0x100000
#define WAVE_BUFFERS    4
#define FRAMES_PER_BUF  4096
/* A four minute track at 160 kbps is about 5 MB; 12 leaves room for longer
 * ones without pretending the whole catalogue fits in RAM. */
#define TRACK_LIMIT     (12 * 1024 * 1024)

#define SAMPLE_TRACK "spotify:track:3n3Ppam7vgaVa1iaRUc9Lp"

/* A track to try instead of the sample: one line holding a spotify:track URI
 * or an open.spotify.com/track link, next to the .3dsx or at the SD root.
 * It lets a track that fails in the app be looked at without a rebuild. */
#define URI_FILE "spotify-play-uri.txt"

static FILE *logf;

static void say(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fputs(line, stdout);
	fputs(line, stderr);
	if (logf) {
		fputs(line, logf);
		fflush(logf);
	}
}

static void open_log(const char *argv0)
{
	if (argv0 && argv0[0]) {
		char p[320];
		snprintf(p, sizeof(p), "%s", argv0);
		char *slash = strrchr(p, '/');
		if (slash) {
			slash[1] = '\0';
			strncat(p, "spotify-play.log", sizeof(p) - strlen(p) - 1);
			logf = fopen(p, "w");
		}
	}
	if (!logf)
		logf = fopen("sdmc:/spotify-play.log", "w");
}

static void frame(void)
{
	gfxFlushBuffers();
	gfxSwapBuffers();
	gspWaitForVBlank();
}

static void track_step(const char *step)
{
	say("   ... %s\n", step);
}

/* The track id in `line`: the 22 characters after "track:" or "track/",
 * which covers both a URI and a share link (whose "?si=" tail is ignored). */
static bool uri_from_line(const char *line, char *uri, size_t cap)
{
	const char *p = strstr(line, "track");
	if (!p || (p[5] != ':' && p[5] != '/'))
		return false;
	char id[SP_ID_LEN + 1];
	memcpy(id, p + 6, SP_ID_LEN);
	id[SP_ID_LEN] = '\0';
	if (strlen(p + 6) < SP_ID_LEN)
		return false;
	uint8_t gid[16];
	if (!sp_gid_from_id(id, gid))
		return false;
	snprintf(uri, cap, "spotify:track:%s", id);
	return true;
}

static void load_uri(const char *argv0, char *uri, size_t cap)
{
	snprintf(uri, cap, "%s", SAMPLE_TRACK);
	char paths[2][320];
	int np = 0;
	if (argv0 && argv0[0]) {
		snprintf(paths[np], sizeof(paths[np]), "%s", argv0);
		char *slash = strrchr(paths[np], '/');
		if (slash) {
			slash[1] = '\0';
			strncat(paths[np], URI_FILE,
			        sizeof(paths[np]) - strlen(paths[np]) - 1);
			np++;
		}
	}
	snprintf(paths[np++], sizeof(paths[0]), "sdmc:/%s", URI_FILE);

	for (int i = 0; i < np; i++) {
		FILE *f = fopen(paths[i], "r");
		if (!f)
			continue;
		char line[256] = "";
		bool got = fgets(line, sizeof(line), f) != NULL;
		fclose(f);
		if (got && uri_from_line(line, uri, cap)) {
			say("track from %s\n", paths[i]);
			return;
		}
		say("%s holds no valid track; using the sample\n", paths[i]);
		return;
	}
}

static void to_hex(const uint8_t *p, size_t n, char *out)
{
	static const char HEX[] = "0123456789abcdef";
	for (size_t i = 0; i < n; i++) {
		out[i * 2] = HEX[p[i] >> 4];
		out[i * 2 + 1] = HEX[p[i] & 15];
	}
	out[n * 2] = '\0';
}

/* metadata.proto's AudioFile.Format, by number. The wire carries only the
 * number, so that is printed too: a name here can be out of date, the
 * number cannot. */
static const char *format_label(uint64_t f)
{
	static const char *const NAMES[] = {
		"OGG_VORBIS_96", "OGG_VORBIS_160", "OGG_VORBIS_320",
		"MP3_256", "MP3_320", "MP3_160", "MP3_96", "MP3_160_ENC",
		"AAC_24", "AAC_48", "MP4_128", "MP4_256", "MP4_128_DUAL",
		"MP4_256_DUAL", "MP4_128_CBCS", "MP4_256_CBCS", "FLAC_FLAC",
		"MP4_FLAC", "XHE_AAC_24", "XHE_AAC_16", "XHE_AAC_12",
		"HE_AAC_64", "FLAC_FLAC_24BIT",
	};
	return f < sizeof(NAMES) / sizeof(NAMES[0]) ? NAMES[f] : "?";
}

static bool read_varint(const uint8_t *p, size_t len, size_t *at, uint64_t *out)
{
	uint64_t v = 0;
	for (unsigned shift = 0; *at < len && shift < 64; shift += 7) {
		uint8_t b = p[(*at)++];
		v |= (uint64_t)(b & 0x7F) << shift;
		if (!(b & 0x80)) {
			*out = v;
			return true;
		}
	}
	return false;
}

/* Which top-level fields a message carries, repeats folded into a count:
 * a field the client does not read yet shows up here and nowhere else. */
static void field_summary(const char *what, const pbv *msg)
{
	char line[256];
	int n = snprintf(line, sizeof(line), "   %s:", what);
	size_t at = 0;
	uint32_t last = 0;
	int count = 0;
	while (at < msg->len) {
		uint64_t key, v = 0;
		if (!read_varint(msg->p, msg->len, &at, &key))
			break;
		uint32_t f = (uint32_t)(key >> 3);
		unsigned wire = (unsigned)(key & 7);
		if (wire == 0) {
			if (!read_varint(msg->p, msg->len, &at, &v))
				break;
		} else if (wire == 2) {
			if (!read_varint(msg->p, msg->len, &at, &v) || at + v > msg->len)
				break;
			at += (size_t)v;
		} else if (wire == 1) {
			at += 8;
		} else if (wire == 5) {
			at += 4;
		} else {
			break;
		}
		if (f == last) {
			count++;
			continue;
		}
		if (last && n < (int)sizeof(line))
			n += snprintf(line + n, sizeof(line) - n,
			              count > 1 ? " f%u x%d" : " f%u", (unsigned)last,
			              count);
		last = f;
		count = 1;
	}
	if (last && n < (int)sizeof(line))
		snprintf(line + n, sizeof(line) - n,
		         count > 1 ? " f%u x%d" : " f%u", (unsigned)last, count);
	say("%s\n", line);
}

/* Every AudioFile in `msg`'s repeated `field` -- all of them, unlike the
 * parser, which keeps SP_MAX_FILES -- each with what storage-resolve says
 * of it, which is whether the CDN would serve that format at all. Counts
 * the Vorbis ones into *vorbis. */
static int list_files(const char *host, const char *token, const pbv *msg,
                      uint32_t field, const char *indent, int *vorbis)
{
	size_t at = 0;
	pbv file;
	int n = 0;
	while (pb_next_bytes(msg, field, &at, &file)) {
		pbv fid;
		uint64_t fmt = 0;
		bool has_fmt = pb_get_varint(file.p, file.len, 2, &fmt);
		if (!pb_get_bytes(file.p, file.len, 1, &fid) || fid.len != 20) {
			say("%s#%-2d no file_id (%u bytes)\n", indent, n,
			    (unsigned)file.len);
			n++;
			continue;
		}
		char hex[41];
		to_hex(fid.p, 20, hex);
		if (has_fmt)
			say("%s#%-2d %2llu %-15s %.8s\n", indent, n,
			    (unsigned long long)fmt, format_label(fmt), hex);
		else
			say("%s#%-2d no format        %.8s\n", indent, n, hex);
		if (has_fmt && fmt <= SP_FMT_OGG_320)
			(*vorbis)++;

		sp_storage st;
		char err[160];
		if (sp_storage_resolve(host, token, fid.p, &st, err, sizeof(err)) < 0)
			say("%s    resolve: %s\n", indent, err);
		else
			say("%s    resolve: result %d, %d urls\n", indent, st.result,
			    st.count);
		n++;
	}
	return n;
}

/* Restriction { catalogue 1, countries_allowed 2, countries_forbidden 3,
 * type 4, catalogue_str 5 }: the countries are two-letter codes run
 * together, so their length over two is how many. */
static void list_restrictions(const pbv *track)
{
	size_t at = 0;
	pbv rs;
	int n = 0;
	while (pb_next_bytes(track, 11, &at, &rs)) {
		pbv v;
		uint64_t type = 0;
		unsigned allowed = 0, forbidden = 0;
		if (pb_get_bytes(rs.p, rs.len, 2, &v))
			allowed = (unsigned)v.len / 2;
		if (pb_get_bytes(rs.p, rs.len, 3, &v))
			forbidden = (unsigned)v.len / 2;
		pb_get_varint(rs.p, rs.len, 4, &type);
		char cats[96] = "";
		size_t cat = 0;
		pbv c;
		while (pb_next_bytes(&rs, 5, &cat, &c)) {
			size_t used = strlen(cats);
			if (used + c.len + 2 >= sizeof(cats))
				break;
			if (used)
				strcat(cats, ",");
			strncat(cats, (const char *)c.p, c.len);
		}
		say("   restriction %d: type %llu, %u allowed, %u forbidden, %s\n",
		    n, (unsigned long long)type, allowed, forbidden,
		    cats[0] ? cats : "-");
		n++;
	}
	if (!n)
		say("   no restrictions\n");
}

/* Why a track has, or seems to have, no Vorbis file. Reads the raw Track
 * and lists everything in it the parser might drop, then compares with
 * what sp_parse_track and sp_pick_file make of the same bytes. */
static void probe_files(const char *host, const char *token, const char *uri)
{
	uint8_t q_store[16], er_store[192], req_store[256];
	pb q, er, req;
	pb_init(&q, q_store, sizeof(q_store));
	pb_varint(&q, 1, EXT_KIND_TRACK_V4);
	pb_init(&er, er_store, sizeof(er_store));
	pb_str(&er, 1, uri);
	pb_sub(&er, 2, &q);
	pb_init(&req, req_store, sizeof(req_store));
	pb_sub(&req, 2, &er);

	http_reply r;
	if (http_request(host, "/extended-metadata/v0/extended-metadata", "POST",
	                 token, "application/x-protobuf", "application/x-protobuf",
	                 req.p, req.len, 0, &r) != 0) {
		say("   metadata: %s\n", r.err);
		return;
	}
	pbv t;
	if (r.status != 200 ||
	    !sp_parse_entity((const uint8_t *)r.body, r.len, &t)) {
		say("   metadata: HTTP %d, %u bytes, no Track\n", r.status,
		    (unsigned)r.len);
		http_reply_free(&r);
		return;
	}
	say("   Track of %u bytes\n", (unsigned)t.len);
	field_summary("fields", &t);

	int vorbis = 0;
	say("   Track.file:\n");
	int files = list_files(host, token, &t, 12, "     ", &vorbis);
	if (!files)
		say("     (none)\n");

	int alt_vorbis = 0, nalt = 0;
	size_t at = 0;
	pbv alt;
	while (pb_next_bytes(&t, 13, &at, &alt)) {
		pbv gid;
		char alt_uri[SP_URI_MAX] = "?";
		if (pb_get_bytes(alt.p, alt.len, 1, &gid) && gid.len == 16)
			sp_uri_from_gid(SP_KIND_TRACK, gid.p, alt_uri, sizeof(alt_uri));
		say("   Track.alternative[%d] %s\n", nalt, alt_uri);
		field_summary("  fields", &alt);
		if (!list_files(host, token, &alt, 12, "       ", &alt_vorbis))
			say("       (no files)\n");
		nalt++;
	}
	list_restrictions(&t);

	static sp_track parsed;
	memset(&parsed, 0, sizeof(parsed));
	sp_parse_track(&t, &parsed);
	const sp_file *pick = sp_pick_file(&parsed);
	http_reply_free(&r);

	say("   parser: %d of %d files (cap %d), picks %s\n",
	    parsed.file_count, files, SP_MAX_FILES,
	    pick ? sp_format_name(pick->format) : "none");
	if (pick)
		say("   VERDICT: there is Vorbis and the parser finds it.\n");
	else if (vorbis)
		say("   VERDICT: %d Vorbis in Track.file, but the parser "
		    "drops them (SP_MAX_FILES cap).\n", vorbis);
	else if (alt_vorbis)
		say("   VERDICT: Vorbis only in Track.alternative (%d).\n",
		    alt_vorbis);
	else if (files || nalt)
		say("   VERDICT: no Vorbis; only the formats above.\n");
	else
		say("   VERDICT: the track carries no files (unavailable?).\n");
}

int main(int argc, char **argv)
{
	(void)argc;
	gfxInitDefault();
	consoleInit(GFX_TOP, NULL);
	const char *argv0 = (argv && argv[0]) ? argv[0] : NULL;
	open_log(argv0);
	cfg_set_path(argv0);

	say("3DS Spotify -- native playback (phase 5a)\n");
	say("built %s\n", BUILD_STAMP);
	char track_uri[64];
	load_uri(argv0, track_uri, sizeof(track_uri));
	say("%s\n\n", track_uri);

	u32 *soc = memalign(0x1000, SOC_BUFFER);
	if (!soc || R_FAILED(socInit(soc, SOC_BUFFER))) {
		say("socInit failed; is wifi on?\n");
		goto hold;
	}
	if (R_FAILED(psInit())) {
		say("psInit failed.\n");
		goto hold;
	}
	int rc = tls_global_init();
	if (rc) {
		char reason[128];
		tls_strerror(rc, reason, sizeof(reason));
		say("tls_global_init: %s\n", reason);
		goto hold;
	}

	char err[256];
	char stored[512];
	if (!cfg_load_refresh_token(stored, sizeof(stored))) {
		say("Not paired. Run probe_pair first.\n");
		goto hold;
	}

	spotify_tokens tok;
	say("1. OAuth token...\n");
	if (token_refresh(stored, &tok, err, sizeof(err)) != 0) {
		say("   %s\n", err);
		goto hold;
	}
	if (tok.refresh_token[0] && strcmp(tok.refresh_token, stored) != 0)
		cfg_save_refresh_token(tok.refresh_token, tok.scope);

	char device_id[64];
	ap_device_id(device_id, sizeof(device_id));

	say("2. access point...\n");
	ap_candidate cands[AP_MAX_CANDIDATES];
	int ncands = ap_resolve_list(cands, AP_MAX_CANDIDATES, err, sizeof(err));
	if (ncands < 1) {
		say("   %s\n", err);
		goto hold;
	}
	ap_info info;
	int tried = 0;
	ap_session *ap = ap_login_any(cands, ncands, tok.access_token, device_id,
	                              &info, &tried, err, sizeof(err));
	if (!ap) {
		say("   %s\n", err);
		goto hold;
	}
	say("   %s como %s\n", info.ap, info.username);

	say("3. library token...\n");
	login5_token l5;
	if (login5_from_credentials(info.username, info.reusable, info.reusable_len,
	                            device_id, &l5, err, sizeof(err)) != 0) {
		say("   %s\n", err);
		ap_close(ap);
		goto hold;
	}
	char sp_host[128], ignored[128];
	sp_resolve(sp_host, sizeof(sp_host), ignored, sizeof(ignored));

	say("4. audio files...\n");
	probe_files(sp_host, l5.access_token, track_uri);

	say("5. track, key and download...\n");
	track_audio tr;
	/* One megabyte is about 50 seconds at 160 kbps: enough to prove the chain
	 * and hear it, without waiting on the whole file. */
	rc = track_fetch(sp_host, l5.access_token, ap, track_uri,
	                 1024 * 1024, TRACK_LIMIT, track_step, &tr,
	                 err, sizeof(err));
	/* The key was the only thing the session was for. */
	ap_close(ap);
	if (rc != 0) {
		say("   %s\n", err);
		if (rc == AP_KEY_REFUSED)
			say("   (AP refusal; the code alone does not tell the cause)\n");
		goto hold;
	}

	say("   %s - %s\n", tr.meta.artist, tr.meta.name);
	say("   %s, %u bytes\n", sp_format_name(tr.format), (unsigned)tr.len);
	say("   resolve %.0f  key %.0f  download %.0f  decrypt %.0f ms\n",
	    tr.resolve_ms, tr.key_ms, tr.download_ms, tr.decrypt_ms);

	say("6. decode and play...\n");
	int verr = 0;
	stb_vorbis *v = stb_vorbis_open_memory(track_vorbis(&tr),
	                                       (int)track_vorbis_len(&tr),
	                                       &verr, NULL);
	if (!v) {
		say("   stb_vorbis could not open it (error %d)\n", verr);
		track_free(&tr);
		goto hold;
	}
	stb_vorbis_info vi = stb_vorbis_get_info(v);
	say("   %lu Hz, %d channels\n", (unsigned long)vi.sample_rate, vi.channels);

	if (R_FAILED(ndspInit())) {
		say("   ndspInit failed (emulator without DSP firmware)\n");
		stb_vorbis_close(v);
		track_free(&tr);
		goto hold;
	}
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
	ndspChnSetRate(0, (float)vi.sample_rate);
	ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
	float mix[12] = {0};
	mix[0] = mix[1] = 1.0f;
	ndspChnSetMix(0, mix);

	const u32 buf_bytes = FRAMES_PER_BUF * 4;
	u8 *audio = linearAlloc(buf_bytes * WAVE_BUFFERS);
	static ndspWaveBuf waves[WAVE_BUFFERS];
	if (!audio) {
		say("   out of linear memory\n");
		goto hold;
	}
	memset(waves, 0, sizeof(waves));
	for (int i = 0; i < WAVE_BUFFERS; i++) {
		waves[i].data_vaddr = audio + i * buf_bytes;
		waves[i].nsamples = FRAMES_PER_BUF;
		waves[i].status = NDSP_WBUF_DONE;
	}

	say("   playing. START stops.\n\n");
	unsigned frames = 0;
	int underruns = 0, stalled = 0;
	double decode_ms = 0;
	bool eof = false;
	TickCounter t;

	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;

		bool filled = false;
		for (int i = 0; i < WAVE_BUFFERS && !eof; i++) {
			if (waves[i].status != NDSP_WBUF_DONE &&
			    waves[i].status != NDSP_WBUF_FREE)
				continue;
			s16 *dst = (s16 *)waves[i].data_vaddr;
			osTickCounterStart(&t);
			int got = stb_vorbis_get_samples_short_interleaved(
			              v, vi.channels, dst, FRAMES_PER_BUF * 2);
			osTickCounterUpdate(&t);
			decode_ms += osTickCounterRead(&t);
			if (got <= 0) {
				eof = true;
				break;
			}
			if (got < FRAMES_PER_BUF)
				memset(dst + got * 2, 0, (FRAMES_PER_BUF - got) * 4);
			frames += (unsigned)got;
			waves[i].nsamples = got;
			DSP_FlushDataCache(dst, buf_bytes);
			ndspChnWaveBufAdd(0, &waves[i]);
			filled = true;
		}

		if (!eof && !filled) {
			int idle = 0;
			for (int i = 0; i < WAVE_BUFFERS; i++)
				if (waves[i].status == NDSP_WBUF_DONE ||
				    waves[i].status == NDSP_WBUF_FREE)
					idle++;
			if (idle == WAVE_BUFFERS)
				underruns++;
			if (++stalled > 1800)
				break;
		} else {
			stalled = 0;
		}

		if (eof) {
			bool playing = false;
			for (int i = 0; i < WAVE_BUFFERS; i++)
				if (waves[i].status == NDSP_WBUF_QUEUED ||
				    waves[i].status == NDSP_WBUF_PLAYING)
					playing = true;
			if (!playing)
				break;
		}
		frame();
	}

	double audio_s = (double)frames / vi.sample_rate;
	say("\n   %.1f s played, %d underruns\n", audio_s, underruns);
	if (decode_ms > 0)
		say("   decoder at %.2fx real time\n",
		    audio_s * 1000.0 / decode_ms);

	stb_vorbis_close(v);
	track_free(&tr);
	ndspExit();

hold:
	say("\nSTART exits.\n");
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
		frame();
	}
	if (logf)
		fclose(logf);
	tls_global_exit();
	psExit();
	socExit();
	gfxExit();
	return 0;
}
