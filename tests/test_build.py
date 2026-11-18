import hashlib
import importlib.util
import io
import os
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "native_build", Path(__file__).resolve().parents[1] / "source" / "build.py"
)
assert SPEC is not None and SPEC.loader is not None
build = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build)


def read_png_rgba(data: bytes) -> tuple[int, int, bytes]:
    """Decodes an 8-bit, non-interlaced RGBA PNG with the standard library.
    Fails on anything else: the logo has to be exactly that."""
    import struct
    import zlib
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise AssertionError("not a PNG")
    pos, idat, header = 8, bytearray(), None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    if header is None:
        raise AssertionError("PNG without IHDR")
    width, height, depth, colour, _, _, interlace = header
    if (depth, colour, interlace) != (8, 6, 0):
        raise AssertionError("logo must be 8-bit RGBA, not interlaced")
    # Bail out on the size before decoding a huge image.
    if width > 256 or height > 128:
        return width, height, b""
    raw = zlib.decompress(bytes(idat))
    stride = width * 4
    out = bytearray(height * stride)
    prev = bytearray(stride)
    for y in range(height):
        filter_type = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if filter_type == 1:
                line[i] = (line[i] + a) & 0xFF
            elif filter_type == 2:
                line[i] = (line[i] + b) & 0xFF
            elif filter_type == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif filter_type == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
            elif filter_type != 0:
                raise AssertionError("bad PNG filter")
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return width, height, bytes(out)


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_explicit_sdk_takes_precedence_over_environment(self):
        (self.root / "devkitARM").mkdir()
        with patch.dict(os.environ, {"DEVKITPRO": "not-the-selected-sdk"}):
            self.assertEqual(build.find_devkitpro(self.root), self.root.resolve())

    def test_invalid_explicit_sdk_does_not_fall_back(self):
        with self.assertRaisesRegex(SystemExit, "devkitPro not found"):
            build.find_devkitpro(self.root / "missing")

    def test_sdk_from_environment(self):
        (self.root / "devkitARM").mkdir()
        with patch.dict(os.environ, {"DEVKITPRO": str(self.root)}):
            self.assertEqual(build.find_devkitpro(None), self.root.resolve())

    def test_require_tool_uses_host_suffix(self):
        name = "arm-none-eabi-gcc" + (".exe" if os.name == "nt" else "")
        expected = self.root / name
        expected.touch()
        self.assertEqual(build.require_tool(self.root, "arm-none-eabi-gcc"), expected)
        with self.assertRaisesRegex(SystemExit, "Missing tool"):
            build.require_tool(self.root, "missing")

    def test_missing_stb_vorbis_offline_has_actionable_error(self):
        with patch.object(build, "ROOT", self.root):
            with self.assertRaisesRegex(SystemExit, "--fetch-deps"):
                build.stb_vorbis_source(False)

    def test_native_target_has_root_entry_point_and_no_proxy_client(self):
        source_dirs, _, mbedtls, extra_sources = build.APPS["3dspotify"]
        source_names = {
            path.name
            for directory in source_dirs
            for path in (build.ROOT / directory).glob("*.c")
        }
        source_names.update(Path(path).name for path in extra_sources)
        self.assertEqual(source_dirs, ["ui", "lang", "platform3ds"])
        self.assertTrue((build.ROOT / "main.c").is_file())
        self.assertFalse((build.ROOT / "ui" / "main.c").exists())
        self.assertIn("main.c", extra_sources)
        self.assertFalse((build.ROOT / "image_display.c").exists())
        self.assertFalse((build.ROOT / "parse.c").exists())
        self.assertFalse((build.ROOT / "fetch.c").exists())
        self.assertFalse((build.ROOT.parent / "server").exists())
        self.assertNotIn("stb_image_impl.c", source_names)
        self.assertIn("ui", source_dirs)
        self.assertNotIn("probe_play", source_dirs)
        self.assertEqual(mbedtls, "release")
        self.assertIn("deps/stb_vorbis.c", extra_sources)

    def test_native_target_name_and_distribution_path(self):
        self.assertIn("3dspotify", build.APPS)
        self.assertNotIn("spotify3ds", build.APPS)
        self.assertEqual(build.APP_TITLE, "3DSpotify")
        source = (build.ROOT / "build.py").read_text(encoding="utf-8")
        self.assertIn('default="3dspotify"', source)
        self.assertIn(
            'output_name = APP_TITLE if app == "3dspotify" else app',
            source,
        )
        self.assertIn(
            'ROOT / "dist" / "3ds" / output_name / f"{output_name}.3dsx"',
            source,
        )
        self.assertIn('dest.with_suffix(".cia")', source)
        self.assertIn("launch {dest.name}", source)

    def test_native_target_uses_only_direct_artwork_pipeline(self):
        source_dirs, _, _, _ = build.APPS["3dspotify"]
        ui_sources = list((build.ROOT / "ui").glob("*.c"))
        implementations = [
            source for source in ui_sources
            if "STB_IMAGE_IMPLEMENTATION" in source.read_text(encoding="utf-8")
        ]
        self.assertIn("ui", source_dirs)
        self.assertEqual([source.name for source in implementations],
                         ["artwork.c"])
        self.assertTrue((build.ROOT / "ui" / "artwork.h").is_file())

    def test_citro2d_owns_both_screens(self):
        import re
        ui_dir = build.ROOT / "ui"
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")
        artwork = (ui_dir / "artwork.c").read_text(encoding="utf-8")
        for name in ("ui.c", "ui.h", "image_util.h", "worker_priority.h"):
            self.assertTrue((ui_dir / name).is_file(), name)
        # The 40-column text screen is gone: every screen is a view.
        for name in ("textscreen.c", "textscreen.h"):
            self.assertFalse((ui_dir / name).exists(), name)
        self.assertNotIn("textscreen", app)
        self.assertNotIn("VIEW_TEXT", app)
        # No libctru console and no direct framebuffer writes: a console and
        # a citro3d target cannot share a screen.
        for source in (app, artwork):
            self.assertNotIn("consoleInit", source)
            self.assertNotIn("consoleClear", source)
            self.assertNotIn("gfxGetFramebuffer", source)
            self.assertNotIn("gfxSwapBuffers", source)
            self.assertIsNone(re.search(r"(?<![A-Za-z_])printf\(", source))
        self.assertNotIn("frames_drawn", artwork)
        self.assertIn("ui_init()", app)
        self.assertLess(app.index("artwork_close(&cover_art);"),
                        app.index("ui_exit();", app.index("done:")))
        # Render on demand: a GPU frame only when something changed.
        frame = app[app.index("static void frame(void)"):
                    app.index("/* --- Search ---")]
        self.assertIn("view_login_dirty()", frame)
        self.assertIn("gspWaitForVBlank();", frame)
        # Workers that can overlap playback stay below the decoder.
        self.assertIn("background_priority()", artwork)
        self.assertNotIn("0x18", artwork)
        self.assertNotIn("static s32 background_priority", app)

    def test_home_view_replaces_session_text_screen(self):
        ui_dir = build.ROOT / "ui"
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")
        for name in ("view_home.c", "view_home.h", "home_logic.h",
                     "text_fit.h"):
            self.assertTrue((ui_dir / name).is_file(), name)
        self.assertNotIn("show_session_ready", app)
        self.assertIn("view_home_init()", app)
        self.assertLess(app.index("view_home_exit();", app.index("done:")),
                        app.index("ui_exit();", app.index("done:")))
        # Pure logic stays free of 3DS headers for the host tests.
        for name in ("home_logic.h", "grid_logic.h", "home_feed_logic.h",
                     "text_fit.h", "theme.h", "layout.h", "press_logic.h"):
            source = (ui_dir / name).read_text(encoding="utf-8")
            self.assertNotIn("#include <3ds", source)
            self.assertNotIn("#include <citro", source)
        # The home view joins the on-demand render: a frame only when dirty.
        frame = app[app.index("static void frame(void)"):
                    app.index("/* --- Search ---")]
        self.assertIn("view_home_dirty()", frame)
        self.assertIn("view_home_draw_bottom()", frame)
        # The playlists load in the background from the start of the tabs,
        # are reaped every frame and stopped before the thumbnails' wait;
        # Retry loads them again. The old last-search button is gone.
        loop = app[app.index("static tabs_exit run_tabs("):
                   app.index("static void stop_tabs(")]
        self.assertLess(loop.index("home_feed_start("), loop.index("for (;;)"))
        self.assertIn("home_feed_poll();", loop)
        self.assertIn("case HOME_ACTION_RETRY:", loop)
        self.assertNotIn("HOME_ACTION_REPEAT", app)
        stop = app[app.index("static void stop_tabs("):]
        stop = stop[:stop.index("\n}\n")]
        self.assertLess(stop.index("home_feed_stop();"),
                        stop.index("thumbs_wait();"))
        # A cancelled keyboard does not wipe the last query.
        ask = app[app.index("static bool ask_search_query("):
                  app.index("static void keyboard_search(")]
        self.assertIn("if (confirmed)", ask)
        # Reconnecting and pairing moved to the Settings tab; Home only
        # offers to reconnect while it runs without a connection.
        home = (ui_dir / "view_home.c").read_text(encoding="utf-8")
        self.assertEqual(home.count("HOME_ACTION_RECONNECT"), 1)
        self.assertIn("if (offline)\n\t\t\tqueue(HOME_ACTION_RECONNECT", home)
        self.assertNotIn("HOME_ACTION_PAIR", home)

    def test_home_playlists_follow_style_guide_and_docs(self):
        import re
        ui_dir = build.ROOT / "ui"
        lang_dir = build.ROOT / "lang"
        docs = build.ROOT.parent / "docs"
        for name in ("tile_grid.c", "tile_grid.h", "grid_logic.h",
                     "home_feed.c", "home_feed.h", "home_feed_logic.h"):
            self.assertTrue((ui_dir / name).is_file(), name)
        # No loose colours or text scales in the new drawing code.
        for name in ("tile_grid.c", "view_home.c"):
            source = (ui_dir / name).read_text(encoding="utf-8")
            self.assertIsNone(re.search(r"0x[0-9A-Fa-f]{6}", source), name)
            self.assertIsNone(re.search(r"\b0\.(?!0f)\d+f\b", source), name)
            self.assertNotIn("C2D_Color32", source, name)
        # Rows and tiles share one frame for their states (5.3, 5.16).
        self.assertIn("ui_item_frame(", (ui_dir / "track_list.c").read_text(
            encoding="utf-8"))
        self.assertIn("ui_item_frame(", (ui_dir / "tile_grid.c").read_text(
            encoding="utf-8"))
        theme = (ui_dir / "theme.h").read_text(encoding="utf-8")
        for token in ("TILE_H = TOUCH_MIN", "TILE_COVER", "GRID_GAP"):
            self.assertIn(token, theme)
        # The loader keeps credentials only while it runs.
        feed = (ui_dir / "home_feed.c").read_text(encoding="utf-8")
        for function in ("bool home_feed_poll(", "void home_feed_stop("):
            body = feed[feed.index(function):]
            body = body[:body.index("\n}\n")]
            self.assertIn("secure_clear(job.token", body)
            self.assertIn("secure_clear(job.username", body)
        self.assertIn("background_priority()", feed)
        self.assertNotIn("api.spotify.com", feed)
        # Every new string in both languages (Spanish with its accents and
        # ñ since ef1e295: the native tests check the text is valid UTF-8).
        keys = ("STR_HOME_RECENT", "STR_HOME_YOUR_PLAYLISTS",
                "STR_HOME_SHOW_MORE", "STR_HOME_EMPTY",
                "STR_HOME_FAILED_TITLE", "STR_HINT_OPEN", "STR_PLAYLIST_TYPE",
                "STR_LIKED_SONGS", "STR_RESUME_FMT")
        for name in ("lang_en.c", "lang_es.c"):
            table = (lang_dir / name).read_text(encoding="utf-8")
            for key in keys:
                match = re.search(r"\[" + key + r"\] = \"([^\"]+)\"", table)
                self.assertIsNotNone(match, (name, key))
        # The focused tile previews on the top screen through the detail
        # card, and Liked Songs leads the tiles with its heart.
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")
        sync = app[app.index("static const detail_doc *top_detail_sync("):]
        self.assertIn("view_home_preview()", sync[:sync.index("\n}\n")])
        self.assertIn("SP_LIKED_URI", feed)
        self.assertIn("ICON_HEART", (ui_dir / "tile_grid.c").read_text(
            encoding="utf-8"))
        self.assertIn('"HEART"', (ui_dir / "gen_icons.py").read_text(
            encoding="utf-8"))
        self.assertNotIn("STR_HOME_LAST_SEARCH",
                         (lang_dir / "lang.h").read_text(encoding="utf-8"))
        # The style guide and both READMEs describe it.
        styles = (docs / "STYLES.md").read_text(encoding="utf-8")
        self.assertIn("### 5.16 Tesela", styles)
        home = styles[styles.index("### 7.7 Inicio"):
                      styles.index("### 7.8 Ajustes")]
        for text in ("Mostrar más", "Recientes", "Tus playlists", "[Y] Buscar"):
            self.assertIn(text, home)
        self.assertIn("Playlist:", styles[styles.index("### 7.12"):])
        self.assertIn("TILE_H = TOUCH_MIN", styles)
        readme = (build.ROOT.parent / "README.md").read_text(encoding="utf-8")
        readme_es = (build.ROOT.parent / "README_es.md").read_text(
            encoding="utf-8")
        self.assertIn("Show more", readme)
        self.assertIn("**Playlist:**", readme)
        self.assertIn("Mostrar", readme_es)
        self.assertIn("**Playlist:**", readme_es)

    def test_ui_follows_style_guide(self):
        import re
        ui_dir = build.ROOT / "ui"
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")
        for name in ("theme.h", "draw.c", "draw.h", "layout.h",
                     "press_logic.h", "icons.c", "icons.h",
                     "icons_data.c", "icons_data.h", "gen_icons.py"):
            self.assertTrue((ui_dir / name).is_file(), name)
        # Every colour is opaque: no alpha in the theme (2, 2.7).
        theme = (ui_dir / "theme.h").read_text(encoding="utf-8")
        self.assertNotIn("THEME_RGBA", theme)
        for line in theme.splitlines():
            if line.startswith("#define COL_"):
                self.assertIn("THEME_RGB(", line)
        # Every colour comes from theme.h (docs/STYLES.md 1.5).
        for path in [build.ROOT / "main.c", *ui_dir.glob("*.c")]:
            source = path.read_text(encoding="utf-8")
            self.assertNotIn("C2D_Color32", source, path.name)
            self.assertNotIn("UI_COLOR_", source, path.name)
        # The redesigned views: no loose colours, sizes or text scales.
        for name in ("view_home.c", "view_results.c", "track_list.c",
                     "draw.c"):
            source = (ui_dir / name).read_text(encoding="utf-8")
            self.assertIsNone(re.search(r"0x[0-9A-Fa-f]{6}", source), name)
            # 0.0f is the depth argument of the C2D calls.
            self.assertIsNone(re.search(r"\b0\.(?!0f)\d+f\b", source), name)
        # Flat and square (1.2, 4.3): solid rectangles only, through the
        # draw.c helpers; no circles, triangles, gradients or blending.
        draw = (ui_dir / "draw.c").read_text(encoding="utf-8")
        self.assertIn("ui_rect ui_bevel_rect(", draw)
        self.assertIn("void ui_panel(", draw)
        self.assertIn("void ui_button(", draw)
        home = (ui_dir / "view_home.c").read_text(encoding="utf-8")
        results = (ui_dir / "view_results.c").read_text(encoding="utf-8")
        rows = (ui_dir / "track_list.c").read_text(encoding="utf-8")
        for source, name in ((draw, "draw.c"), (home, "view_home.c"),
                             (results, "view_results.c"),
                             (rows, "track_list.c")):
            for call in ("C2D_DrawCircle", "C2D_DrawTriangle",
                         "C2D_DrawRectangle(", "C2D_DrawEllipse",
                         "color_fade", "color_mix"):
                self.assertNotIn(call, source, name)
        self.assertNotIn("C2D_DrawRect", home)
        self.assertNotIn("C2D_DrawRect", results)
        self.assertNotIn("C2D_DrawRect", rows)
        # Icons: crisp at whole pixels (5.10).
        icons = (ui_dir / "icons.c").read_text(encoding="utf-8")
        self.assertIn("GPU_NEAREST, GPU_NEAREST", icons)
        # Both screens share the background gradient (7.1).
        ui = (ui_dir / "ui.c").read_text(encoding="utf-8")
        self.assertIn("COL_FONDO_SUP, COL_FONDO_SUP", ui)
        # Touch acts on release; physical buttons show the pressed state.
        self.assertIn("TOUCH_ACTIVATE", home)
        self.assertIn("press_shown(pending_press)", home)
        # Icons are generated C arrays that match their generator.
        spec = importlib.util.spec_from_file_location(
            "gen_icons", ui_dir / "gen_icons.py")
        gen_icons = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gen_icons)
        self.assertTrue(gen_icons.is_current(),
                        "run source/ui/gen_icons.py")
        self.assertIn("icons_init();", app)
        self.assertLess(app.index("icons_exit();", app.index("done:")),
                        app.index("ui_exit();", app.index("done:")))

    def test_results_view_replaces_search_text_screens(self):
        ui_dir = build.ROOT / "ui"
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")
        for name in ("view_results.c", "view_results.h", "list_logic.h"):
            self.assertTrue((ui_dir / name).is_file(), name)
        for gone in ("show_search_progress", "show_search_error",
                     "show_results(", "RESULTS_PER_PAGE 7"):
            self.assertNotIn(gone, app)
        logic = (ui_dir / "list_logic.h").read_text(encoding="utf-8")
        self.assertNotIn("#include <3ds", logic)
        self.assertNotIn("#include <citro", logic)
        # 7.4: four rows per screen, left/right jump as many; L/R are
        # the tab bar's.
        self.assertIn("#define RESULTS_PER_PAGE 4", logic)
        results = (ui_dir / "view_results.c").read_text(encoding="utf-8")
        # The rows, their focus, scrolling and touch are the list widget's,
        # shared with the album and artist details.
        rows = (ui_dir / "track_list.c").read_text(encoding="utf-8")
        self.assertIn('#include "track_list.h"', results)
        self.assertIn("track_list_update_touch(&list", results)
        self.assertIn("track_list_navigate(&list, down)", results)
        # Left and right jump a page: the rows of the list's frame
        # (RESULTS_PER_PAGE, or fewer under an album's download button).
        self.assertIn("move_focus(l, l->frame.rows)", rows)
        self.assertIn("else if (down & KEY_RIGHT)\n\t\tmove_focus(l, "
                      "l->frame.rows);", rows)
        self.assertIn("l->frame = list_frame_full();", rows)
        for source in (results, rows):
            self.assertNotIn("KEY_L)", source)
            self.assertNotIn("KEY_R)", source)
        # Rows are cut in pixels, never the duration (5.3, 3.3).
        self.assertIn("ui_parse_fitted(&l->title[i]", rows)
        # The thumbnail and its note placeholder are the shared ui_art.
        self.assertIn("ui_art(list_thumb_rect(row)", rows)
        draw = (ui_dir / "draw.c").read_text(encoding="utf-8")
        self.assertIn("ICON_NOTE", draw)
        # On-demand render and view switching.
        frame = app[app.index("static void frame(void)"):
                    app.index("/* --- Search ---")]
        self.assertIn("view_results_dirty()", frame)
        self.assertIn("view_results_draw_bottom()", frame)
        self.assertIn("view_results_init()", app)
        self.assertLess(app.index("view_results_exit();", app.index("done:")),
                        app.index("ui_exit();", app.index("done:")))
        # Searching is a state of the view, in the background: B cancels
        # from its tab, START closes the app.
        search = app[app.index("static void search_start("):
                     app.index("static bool ask_search_query(")]
        self.assertIn("view_results_searching();", search)
        self.assertIn("view_results_show(search.tracks", search)
        loop = app[app.index("static tabs_exit run_tabs("):
                   app.index("static void stop_tabs(")]
        self.assertIn("view_results_cancelling()", loop)
        self.assertIn("(down & KEY_B)", loop)
        self.assertIn("if (down & KEY_START)", loop)
        # A result stands alone: a radio from it is the queue, and errors
        # can be retried.
        self.assertIn("queue_play_radio(session, &search.tracks[index]);", app)
        self.assertIn("case RESULTS_ACTION_RETRY:", app)
        self.assertIn("search_error_advice(search.error)", app)

    def test_result_thumbnails_follow_style_guide(self):
        ui_dir = build.ROOT / "ui"
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")
        for name in ("thumbs.c", "thumbs.h", "thumb_queue.h", "art_url.h"):
            self.assertTrue((ui_dir / name).is_file(), name)
        for name in ("thumb_queue.h", "art_url.h"):
            source = (ui_dir / name).read_text(encoding="utf-8")
            self.assertNotIn("#include <3ds", source)
            self.assertNotIn("#include <citro", source)
        thumbs = (ui_dir / "thumbs.c").read_text(encoding="utf-8")
        artwork = (ui_dir / "artwork.c").read_text(encoding="utf-8")
        # One allow-list for every cover download.
        self.assertIn("art_url_allowed(", artwork)
        self.assertNotIn("static bool artwork_url_allowed", artwork)
        self.assertIn("art_url_path(url)", thumbs)
        # STYLES.md 9: one keep-alive session, below the decoder's priority.
        self.assertIn("http_session_open(ART_HOST)", thumbs)
        self.assertIn("http_session_request(", thumbs)
        self.assertNotIn("http_get_url(", thumbs)
        self.assertIn("background_priority()", thumbs)
        # A fixed pool made once, filled on the main thread; nothing
        # allocated per thumbnail; no citro3d in the worker.
        self.assertNotIn("malloc", thumbs)
        self.assertNotIn("calloc", thumbs)
        worker = thumbs[thumbs.index("static bool fetch("):
                        thumbs.index("/* --- Main thread")]
        for call in ("C3D_", "C2D_", "linearAlloc"):
            self.assertNotIn(call, worker)
        self.assertIn("C3D_TexInit(", thumbs[:thumbs.index("static bool fetch(")])
        # stb_image is still implemented once, in artwork.c.
        self.assertNotIn("STB_IMAGE_IMPLEMENTATION", thumbs)
        # Cancelled and joined before playback and while the audio
        # downloads; stopped with the session.
        self.assertIn("threadJoin(worker", thumbs)
        play = app[app.index("static void play_from_results("):
                   app.index("static void switch_tab(app_tab tab)\n{")]
        self.assertLess(play.index("view_results_allow_thumbs(false);"),
                        play.index("queue_play_radio(session, "))
        loop = app[app.index("static tabs_exit run_tabs("):
                   app.index("static void stop_tabs(")]
        # Paused while the track prepares, not for the whole download.
        self.assertIn("view_results_allow_thumbs(!player_preparing());",
                      loop)
        # Joining a worker blocks the decoder: the track pauses meanwhile.
        stop_search = app[app.index("static void search_stop(void)"):
                          app.index("static void search_start(")]
        self.assertLess(stop_search.index("player_hold(true);"),
                        stop_search.index("search_join();"))
        stop = app[app.index("static void stop_tabs("):]
        self.assertIn("thumbs_wait();", stop[:stop.index("\n}\n")])
        results = (ui_dir / "view_results.c").read_text(encoding="utf-8")
        rows = (ui_dir / "track_list.c").read_text(encoding="utf-8")
        detail = (ui_dir / "view_detail.c").read_text(encoding="utf-8")
        # Allowing and stopping is one switch for every list.
        allow = results[results.index("void view_results_allow_thumbs("):]
        self.assertIn("track_list_allow_thumbs(allow);",
                      allow[:allow.index("\n}\n")])
        allow = rows[rows.index("void track_list_allow_thumbs("):]
        self.assertIn("thumbs_allow(allow);", allow[:allow.index("\n}\n")])
        allow = thumbs[thumbs.index("void thumbs_allow("):]
        self.assertIn("thumbs_stop();", allow[:allow.index("\n}\n")])
        self.assertIn("thumbs_init();", app)
        self.assertLess(app.index("thumbs_exit();", app.index("done:")),
                        app.index("ui_exit();", app.index("done:")))
        # thumbs.c serves one client (a list or a grid): the one on screen
        # claims it from its view's update, where the uploads happen too; a
        # list ready before it is shown only takes it when no other has it.
        frame = thumbs[thumbs.index("void thumbs_client_frame("):]
        frame = frame[:frame.index("\n}\n")]
        self.assertIn("if (owner != c)", frame)
        self.assertIn("thumbs_poll()", frame)
        offer = thumbs[thumbs.index("void thumbs_client_offer("):]
        self.assertIn("if (owner)", offer[:offer.index("\n}\n")])
        self.assertIn("thumbs_client_frame(&l->thumbs);", rows)
        self.assertIn("thumbs_client_offer(&l->thumbs);", rows)
        self.assertIn("track_list_thumbs_frame(&list);", results)
        self.assertIn("track_list_thumbs_frame(&list);", detail)
        self.assertIn("track_list_offer_thumbs(&list);", results)
        # A fixed-size slot either way. A long list keeps a window of rows
        # in the pool, so rows are offset by its base; a moved window
        # keeps the pool and the covers that only changed place.
        self.assertIn("thumbs_client_image(&l->thumbs, index - l->base,", rows)
        self.assertIn("thumbs_client_window(&l->thumbs, l->first - l->base)",
                      rows)
        self.assertIn("thumbs_client_replace(&l->thumbs, urls, n);", rows)
        self.assertIn("thumb_reuse_plan(", thumbs)
        # Only the client thumbs.c serves follows the scroll with downloads.
        window = thumbs[thumbs.index("void thumbs_client_window("):]
        self.assertIn("if (owner == c && c->synced)\n"
                      "\t\tthumbs_set_window(first);",
                      window[:window.index("\n}\n")])
        # Nothing thumbs.c holds for another client is drawn.
        image = thumbs[thumbs.index("bool thumbs_client_image("):]
        self.assertIn("owner == c && c->synced &&",
                      image[:image.index("\n}\n")])

    def test_results_play_directly_and_scroll_by_touch(self):
        ui_dir = build.ROOT / "ui"
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")
        lang_dir = build.ROOT / "lang"
        # No selection screen: the list opens the player.
        self.assertNotIn("show_selection", app)
        self.assertNotIn("selection_flow", app)
        for table in ("lang.h", "lang_en.c", "lang_es.c"):
            source = (lang_dir / table).read_text(encoding="utf-8")
            self.assertNotIn("STR_SELECTION_TITLE", source)
            self.assertNotIn("STR_KEYS_SELECTION_", source)
        play = app[app.index("static void play_from_results("):
                   app.index("static void switch_tab(app_tab tab)\n{")]
        self.assertIn("queue_play_radio(session, ", play)
        self.assertIn("switch_tab(TAB_PLAYER);", play)
        self.assertNotIn("hidKeysDown", play)
        # Dragging (6.9) and the scroll indicator (5.11).
        results = (ui_dir / "view_results.c").read_text(encoding="utf-8")
        rows = (ui_dir / "track_list.c").read_text(encoding="utf-8")
        self.assertIn("list_drag_begin(&l->drag", rows)
        # A drag starts on the list, never on the tab bar.
        self.assertIn("list_area_contains_in(l->frame, pos.px, pos.py)",
                      rows)
        self.assertIn("!tabbar_touching()", rows)
        self.assertIn("list_drag_update(&l->drag", rows)
        self.assertIn("press_cancel(&l->press)", rows)
        # A drag may leave rows cut; the buttons align to whole rows.
        self.assertIn("list_align_in(l->frame, l->scroll, l->selected, "
                      "l->count)", rows)
        # The focus survives a drag (6.9), and the first button press with
        # it out of view only brings the list back to it (6.6).
        drag_end = rows[rows.index("static void drag_end(track_list *l)"):]
        drag_end = drag_end[:drag_end.index("\n}\n")]
        for change in ("list_snap", "scroll =", "selected ="):
            self.assertNotIn(change, drag_end)
        self.assertIn("list_row_whole_in(l->frame, l->selected, l->scroll, "
                      "l->count)", rows)
        self.assertIn("track_list_realign(&list, down, KEY_A | KEY_X)",
                      results)
        for source in (results, rows):
            self.assertNotIn("list_keep_visible", source)
        self.assertIn("list_scroll_thumb_in(l->frame, look->scroll, "
                      "l->count)", rows)
        self.assertIn("list_hit_row_in(l->frame, pos.px, pos.py, l->scroll,",
                      rows)
        style = (build.ROOT.parent / "docs" / "STYLES.md").read_text(
            encoding="utf-8")
        self.assertIn("### 5.11 Indicador de desplazamiento", style)
        self.assertIn("### 6.9 Arrastre en listas", style)

    def test_detail_views_are_wired(self):
        ui_dir = build.ROOT / "ui"
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")
        for name in ("detail_nav.h", "detail_logic.h"):
            source = (ui_dir / name).read_text(encoding="utf-8")
            self.assertNotIn("#include <3ds", source, name)
            self.assertNotIn("#include <citro", source, name)
        # X opens the track detail; A still plays with nothing in between
        # (STYLES.md 7.4).
        results = (ui_dir / "view_results.c").read_text(encoding="utf-8")
        self.assertIn("press_key(GLYPH_X, RESULTS_ACTION_DETAIL, NULL)",
                      results)
        self.assertIn("COL_BTN_X", results)
        loop = app[app.index("static tabs_exit run_tabs("):
                   app.index("static void stop_tabs(")]
        self.assertIn("case RESULTS_ACTION_DETAIL:", loop)
        self.assertIn("play_from_results(session, view_results_selected());",
                      loop)
        self.assertIn("details_open_track(TAB_SEARCH, search.tracks, "
                      "search.count,", app)
        # With a detail open, its view has the tab's input and screen.
        self.assertIn("if (details_active(active_tab)) {\n"
                      "\t\t\tdetail_input(session, down);", loop)
        frame = app[app.index("static void frame(void)"):
                    app.index("/* --- Search ---")]
        self.assertIn("view_detail_dirty(details_top(active_tab))", frame)
        self.assertIn("view_detail_draw_bottom(details_top(active_tab))",
                      frame)
        # The loader runs once per frame next to the search, a new search
        # replaces what the Search tab's details came from, and the loader
        # is gone before the thumbnails' wait and the session.
        self.assertLess(loop.index("search_poll();"),
                        loop.index("details_poll(active_tab,"))
        start = app[app.index("static void search_start("):
                    app.index("static void search_poll(")]
        self.assertIn("details_clear(TAB_SEARCH);", start)
        stop = app[app.index("static void stop_tabs("):]
        stop = stop[:stop.index("\n}\n")]
        self.assertLess(stop.index("tls_set_abort(true);"),
                        stop.index("details_stop();"))
        self.assertLess(stop.index("details_stop();"),
                        stop.index("thumbs_wait();"))
        # Playing from a detail queues its copy, like the results do.
        play = app[app.index("static void play_from_detail("):
                   app.index("static void open_result_detail(")]
        self.assertLess(play.index("view_results_allow_thumbs(false);"),
                        play.index("queue_play_list(session, doc);"))
        # A track detail opened from a single track starts a radio.
        self.assertIn("if (doc->kind == DETAIL_TRACK && doc->single)\n"
                      "\t\t\tqueue_play_radio(session, pick);", play)
        # The queue's loads run beside the player and stop with the
        # session, before the TLS abort is lifted.
        loop = app[app.index("static tabs_exit run_tabs("):
                   app.index("static void stop_tabs(")]
        self.assertLess(loop.index("player_pump();"),
                        loop.index("queue_feed_tick(session);"))
        stop = app[app.index("static void stop_tabs(void)"):
                   app.index("/* --- Session start-up")]
        self.assertLess(stop.index("queue_feed_stop();"),
                        stop.index("tls_set_abort(false);"))
        # Albums and artists: the track's buttons open them on the same
        # tab, X on their rows opens a track detail with the list, and a
        # failed load can be retried.
        detail_in = app[app.index("static void detail_input("):
                        app.index("/* --- Tabs ---")]
        for wired in ("details_open_artist(active_tab, track->artist_uri);",
                      "details_open_album(active_tab, track->album_uri);",
                      "details_open_track(active_tab, doc->tracks, "
                      "doc->count,",
                      "details_retry(active_tab);",
                      "details_back(active_tab);"):
            self.assertIn(wired, detail_in)
        view = (ui_dir / "view_detail.c").read_text(encoding="utf-8")
        # An album's rows share its cover: they show their position.
        self.assertIn("bool numbers = doc->kind == DETAIL_ALBUM ||", view)
        self.assertIn("list.style = numbers ? TRACK_LIST_NUMBERS", view)
        self.assertIn("track_list_scroll_to(&list, doc->first);", view)
        self.assertIn("doc->selected = list.selected;", view)
        # The Playing tab's title, artist and album open their details on
        # that tab, once the press has been seen; the track's with a copy
        # of the queue.
        player = (ui_dir / "view_player.c").read_text(encoding="utf-8")
        self.assertIn("player_link_hit(pos.px, pos.py, enabled)", player)
        self.assertIn("queue(link_action(control - LINK_CONTROL), press);",
                      player)
        self.assertIn("player_links_enabled(info.phase)", player)
        for case, kind in (("PLAYER_ACTION_OPEN_TRACK", "DETAIL_TRACK"),
                           ("PLAYER_ACTION_OPEN_ARTIST", "DETAIL_ARTIST"),
                           ("PLAYER_ACTION_OPEN_ALBUM", "DETAIL_ALBUM")):
            self.assertIn(f"case {case}:\n\t\t\t\topen_playing_detail({kind});",
                          loop)
        opening = app[app.index("static void open_playing_detail("):
                      app.index("static void detail_input(")]
        self.assertIn("details_open_track(TAB_PLAYER, playback_queue, "
                      "playback_queue_count,", opening)
        # The detail card has its own image: cover_art stays the playing
        # track's, which the player waits for before preparing it.
        self.assertEqual(app.count("artwork_request(&cover_art"), 1)
        self.assertIn("artwork_request(&cover_art, playback_queue[index]",
                      app)
        self.assertIn("artwork_request(&detail_art, detail_art_url);", app)
        self.assertIn("artwork_poll(&detail_art);", frame)
        self.assertIn("top_set_detail(top_detail_sync(), &detail_art);",
                      frame)
        keyboard = app[app.index("static bool ask_search_query("):
                       app.index("static void keyboard_search(")]
        self.assertIn("artwork_invalidate(&detail_art);", keyboard)
        self.assertLess(app.index("artwork_close(&detail_art);",
                                  app.index("done:")),
                        app.index("ui_exit();", app.index("done:")))
        top_logic = (ui_dir / "top_logic.h").read_text(encoding="utf-8")
        self.assertIn("if (detail_on)\n\t\treturn TOP_DETAIL;", top_logic)
        self.assertIn("view_detail_init()", app)
        self.assertLess(app.index("view_detail_exit();", app.index("done:")),
                        app.index("ui_exit();", app.index("done:")))

    def test_detail_views_follow_style_guide_and_docs(self):
        import re
        ui_dir = build.ROOT / "ui"
        # Pure logic, testable on the host.
        marquee = (ui_dir / "marquee_logic.h").read_text(encoding="utf-8")
        self.assertNotIn("#include <3ds", marquee)
        self.assertNotIn("#include <citro", marquee)
        # The detail views: no loose colours, text scales or primitives.
        for name in ("view_detail.c", "top_detail.c", "top_now.c"):
            source = (ui_dir / name).read_text(encoding="utf-8")
            self.assertIsNone(re.search(r"0x[0-9A-Fa-f]{6}", source), name)
            self.assertIsNone(re.search(r"\b0\.(?!0f)\d+f\b", source), name)
            for call in ("C2D_DrawCircle", "C2D_DrawTriangle",
                         "C2D_DrawRect", "C2D_DrawEllipse"):
                self.assertNotIn(call, source, name)
        # The detail card is centred and rolls what does not fit instead
        # of cutting it (STYLES.md 5.15, 7.12); the clipping lives in draw.c.
        card = (ui_dir / "top_detail.c").read_text(encoding="utf-8")
        self.assertIn("top_now_draw_header(art, true,", card)
        self.assertNotIn("ui_parse_fitted", card)
        self.assertIn("marquee_offset(", card)
        top_now = (ui_dir / "top_now.c").read_text(encoding="utf-8")
        self.assertIn("ui_text_marquee(", top_now)
        draw = (ui_dir / "draw.c").read_text(encoding="utf-8")
        self.assertIn("C3D_SetScissor(GPU_SCISSOR_DISABLE", draw)
        # The loader's own errors come from the language tables.
        details = (ui_dir / "details.c").read_text(encoding="utf-8")
        self.assertIn("tr(STR_ERR_DETAIL_CONNECT_FMT)", details)
        self.assertIn("tr(STR_ERR_DETAIL_THREAD)", details)
        self.assertIsNone(re.search(r'snprintf\([^;]*"[a-z][^"%]*[a-z]"',
                                    details))
        # Documented: the components, the screens and the controls.
        style = (build.ROOT.parent / "docs" / "STYLES.md").read_text(
            encoding="utf-8")
        for section in ("### 5.14 Enlace", "### 5.15 Texto que rueda",
                        "### 7.9 Detalles: navegación común",
                        "### 7.10 Detalle de canción",
                        "### 7.11 Detalle de álbum, de artista y de playlist",
                        "### 7.12 Ficha de detalle (pantalla superior)"):
            self.assertIn(section, style)
        self.assertIn("[A] Reproducir [X] Detalle [Y] Buscar [B] Volver",
                      style)
        for readme, heading in (("README.md", "### Details"),
                                ("README_es.md", "### Detalles")):
            text = (build.ROOT.parent / readme).read_text(encoding="utf-8")
            self.assertIn(heading, text, readme)
            self.assertIn("#3FA2FF", text, readme)

    def test_download_keys_are_only_stored_wrapped(self):
        import re
        store = (build.ROOT / "platform3ds" / "dl_store.c").read_text(
            encoding="utf-8")
        # Nothing in the store prints or logs: a key cannot leak through a
        # line that is not there.
        self.assertIsNone(re.search(r"(?<![A-Za-z_])(f|v|)printf\(", store))
        self.assertNotIn("fputs(", store.replace("fputs(line, f)", ""))
        self.assertNotIn("say(", store)
        self.assertNotIn("native_log", store)
        # The key goes in and out through AES-GCM, authenticated by the
        # track's URI, under a key derived from this console.
        self.assertIn("mbedtls_gcm_crypt_and_tag", store)
        self.assertIn("mbedtls_gcm_auth_decrypt", store)
        self.assertIn("mbedtls_hkdf(", store)
        self.assertIn("PS_GetLocalFriendCodeSeed", store)
        self.assertIn("PS_GetDeviceId", store)
        # The KEK is wiped after every use, on every path.
        for fn in ("bool dl_store_wrap_key(", "bool dl_store_unwrap_key("):
            body = store[store.index(fn):]
            body = body[:body.index("\n}\n") + 2]
            self.assertIn("mbedtls_platform_zeroize(kek, sizeof(kek));", body)
            self.assertLess(body.index("mbedtls_gcm_free(&gcm);"),
                            body.index("return ok;"))
        # The index keeps only the wrapped form.
        logic = (build.ROOT / "ui" / "downloads_logic.h").read_text(
            encoding="utf-8")
        self.assertIn("wrapped_key[DL_WRAPPED_LEN]", logic)
        self.assertIsNone(re.search(r"uint8_t\s+key\[", logic))

    def test_downloads_engine_keeps_out_of_the_players_way(self):
        import re
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")
        engine = (build.ROOT / "ui" / "downloads.c").read_text(
            encoding="utf-8")

        def body(source, signature):
            text = source[source.index(signature):]
            return text[:text.index("\n}\n")]

        # Leaving the tabs: the engine stops with the TLS abort on, before
        # the session, TLS and sockets go (thumbs_wait is the last join).
        stop = body(app, "static void stop_tabs(void)")
        self.assertLess(stop.index("tls_set_abort(true);"),
                        stop.index("downloads_stop();"))
        self.assertLess(stop.index("downloads_stop();"),
                        stop.index("thumbs_wait();"))
        # Each frame after the audio, told when the player is busy; and the
        # player waits for the engine before preparing a track.
        tabs = body(app, "static tabs_exit run_tabs(")
        self.assertLess(tabs.index("player_pump();"),
                        tabs.index("downloads_poll(dl_player_busy("))
        self.assertIn("downloads_busy()",
                      body(app, "static void player_start_prepare(void)"))
        # The player's keys count against the allowance the engine models.
        self.assertIn("downloads_note_player_key(",
                      body(app, "static void prepare_reap(void)"))
        # Two workers share the session: the lock covers the OAuth renewal
        # and the copy, never a login to the access point.
        ap = body(app, "static ap_session *open_playback_ap(")
        self.assertLess(ap.index("LightLock_Lock(&session_lock);"),
                        ap.index("ensure_oauth_token("))
        self.assertLess(ap.index("ensure_oauth_token("),
                        ap.index("LightLock_Unlock(&session_lock);"))
        self.assertLess(ap.index("LightLock_Unlock(&session_lock);"),
                        ap.index("ap_login_any("))
        self.assertNotIn("session->oauth_token, device_id", ap)
        # The engine's worker runs below the decoder and drops its token.
        self.assertIn("background_priority()", engine)
        self.assertIn("secure_clear(j->token, sizeof(j->token));",
                      body(engine, "static void job_worker(void *arg)"))
        self.assertIn("secure_clear(token, sizeof(token));",
                      body(engine, "void downloads_stop(void)"))
        # Its log names tracks and outcomes, never a key or a token.
        for call in re.findall(r"dlog\((.*?)\);", engine, re.S):
            args = re.sub(r'"[^"]*"', '""', call)   # the words, not the text
            for secret in (r"\btoken\b", r"\bkey\b", "wrapped", "salt",
                           "urls"):
                self.assertIsNone(re.search(secret, args), call)
        # The audio is written as it came: the save path never decrypts.
        track = (build.ROOT / "platform3ds" / "track.c").read_text(
            encoding="utf-8")
        save = body(track, "static bool save_cb(")
        self.assertNotIn("crypt", save)
        self.assertIn("fwrite(data, 1, n, c->f)", save)

    def test_downloads_play_from_the_card(self):
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")
        track = (build.ROOT / "platform3ds" / "track.c").read_text(
            encoding="utf-8")

        def body(source, signature):
            text = source[source.index(signature):]
            return text[:text.index("\n}\n")]

        # The network path is still the network's; a download takes the
        # card's, chosen by the stream itself.
        worker = body(app, "static void download_worker(void *arg)")
        self.assertIn("track_stream_download(&job->stream, &job->stop)",
                      worker)
        self.assertIn("track_stream_load_local(&job->stream, &job->stop)",
                      worker)
        # A downloaded track opens before any access point, with its key
        # unwrapped in the worker and wiped there; the network stays as the
        # fallback.
        prep = body(app, "static void prepare_worker(void *arg)")
        self.assertLess(prep.index("track_stream_open_local("),
                        prep.index("open_playback_ap("))
        self.assertIn("secure_clear(key, sizeof(key));", prep)
        self.assertIn("secure_clear(&job->local_src", prep)
        self.assertIn("downloads_local_track(prep.track.uri, &prep.local_src)",
                      body(app, "static void player_start_prepare(void)"))
        # The card is read through the download's own callback: one place
        # decrypts and publishes chunks, whatever the source.
        local = body(track, "int track_stream_load_local(")
        self.assertIn("stream_cb(ts, (const char *)buf, got);", local)
        self.assertIn("ts->dl_restart_at >= 0", local)
        self.assertNotIn("decrypt(", local)
        # Covers: the cache, then the card, then the network.
        for name, fetch, net in (
                ("artwork.c", "static void artwork_worker(", "http_get_url("),
                ("thumbs.c", "static bool fetch(", "http_session_request(")):
            source = (build.ROOT / "ui" / name).read_text(encoding="utf-8")
            text = body(source, fetch)
            self.assertLess(text.index("art_cache_get("),
                            text.index("dl_store_art_read("))
            self.assertLess(text.index("dl_store_art_read("),
                            text.index(net))

    def test_download_buttons_are_wired(self):
        import re
        root = build.ROOT
        app = (root / "main.c").read_text(encoding="utf-8")
        view = (root / "ui" / "view_detail.c").read_text(encoding="utf-8")
        player = (root / "ui" / "view_player.c").read_text(encoding="utf-8")
        header = (root / "lang" / "lang.h").read_text(encoding="utf-8")

        def body(source, signature):
            text = source[source.index(signature):]
            return text[:text.index("\n}\n")]

        # Every download text in both languages, the Spanish one accented
        # where the word is ("álbum"), and the formats as in English.
        ids = re.findall(r"\b(STR_DL_\w+|STR_HINT_DOWNLOAD)\b,", header)
        self.assertIn("STR_DL_REMOVE_CONFIRM", ids)
        for name in ("lang_en.c", "lang_es.c"):
            table = (root / "lang" / name).read_text(encoding="utf-8")
            for sid in ids:
                self.assertIn("[%s] = \"" % sid, table, (name, sid))
        es = (root / "lang" / "lang_es.c").read_text(encoding="utf-8")
        self.assertIn('[STR_DL_ALL_ALBUM] = "Descargar álbum"', es)
        # The detail's button does what its state says; removing needs the
        # second A that "Press A again" asks for.
        detail = body(app, "static void download_from_detail(")
        self.assertIn("dl_track_button(known, state, alone)", detail)
        self.assertIn("downloads_remove(t->uri);", detail)
        self.assertIn("download_track(t);", detail)
        confirm = body(view, "static detail_action download_action(void)")
        self.assertLess(confirm.index("remove_armed()"),
                        confirm.rindex("return DETAIL_ACTION_DOWNLOAD;"))
        self.assertIn("view_detail_notice(STR_DL_REMOVE_CONFIRM", confirm)
        # The button over an album or a playlist, never an artist's list.
        place = body(view, "static bool has_header(")
        self.assertIn("DETAIL_ALBUM", place)
        self.assertIn("DETAIL_PLAYLIST", place)
        self.assertIn("DETAIL_OFFLINE_COLLECTION", place)
        self.assertNotIn("DETAIL_ARTIST", place)
        self.assertNotIn("DETAIL_DOWNLOADS", place)
        self.assertIn("downloads_add_collection(",
                      body(app, "static void download_collection("))
        # The track that plays goes with the player's key, from X or from
        # the detail alike.
        self.assertIn("downloads_add_track_keyed(",
                      body(app, "static str_id download_track("))
        # The player's X shows its press before it acts (6.5).
        update = body(player, "player_action view_player_update(")
        self.assertIn("queue(PLAYER_ACTION_DOWNLOAD, &press_glyph[GLYPH_X]);",
                      update)
        # The share is read with the clock, not every frame (9).
        self.assertIn("DL_SAMPLE_FRAMES", view)

    def test_downloads_manager_is_wired(self):
        root = build.ROOT
        app = (root / "main.c").read_text(encoding="utf-8")
        view = (root / "ui" / "view_downloads.c").read_text(encoding="utf-8")
        top = (root / "ui" / "top.c").read_text(encoding="utf-8")

        def body(source, signature):
            text = source[source.index(signature):]
            return text[:text.index("\n}\n")]

        # Settings opens it, and B comes back to the Downloads button.
        self.assertIn("case SETTINGS_ACTION_DOWNLOADS:", app)
        self.assertIn("view_settings_focus(SETTINGS_ITEM_DOWNLOADS);", app)
        self.assertIn("view_settings_set_waiting(downloads_waiting());", app)
        self.assertIn("top_set_manager(manager_open);", app)
        self.assertIn("case TOP_DOWNLOADS:", top)
        # Cancelling and removing take a second press of the same button
        # on the same row.
        act = body(view, "static dlv_action activate(void)")
        self.assertLess(act.index("armed(KEY_A, uri)"),
                        act.index("downloads_cancel(uri);"))
        rem = body(view, "static void remove_focused(void)")
        self.assertLess(rem.index("armed(KEY_X, uri)"),
                        rem.index("downloads_remove(uri);"))
        # Rebuilt only when the downloads change; the share and the free
        # space on their own clocks, never every frame (9).
        self.assertIn("if (built != downloads_generation())", view)
        tick = body(view, "static void tick(void)")
        self.assertIn("share_frames >= SHARE_FRAMES", tick)
        self.assertIn("free_frames >= FREE_FRAMES", tick)
        self.assertIn("dl_store_free_bytes()", tick)
        # Its covers come through the thumbnail pool like any list's.
        self.assertIn("thumbs_client_frame(&thumbs);", view)

    def test_home_downloads_never_reach_the_service(self):
        root = build.ROOT
        logic = (root / "ui" / "offline_logic.h").read_text(encoding="utf-8")
        self.assertIn('#define DL_HOME_URI "3dspotify:downloads"', logic)
        # The pseudo-URI is the app's own: nothing that talks to Spotify
        # names it, and the Home tile opens the card's list instead.
        for name in ("spclient.c", "http3ds.c", "ap3ds.c"):
            text = (root / "platform3ds" / name).read_text(encoding="utf-8")
            self.assertNotIn("DL_HOME_URI", text, name)
            self.assertNotIn("3dspotify:downloads", text, name)
        app = (root / "main.c").read_text(encoding="utf-8")
        self.assertIn("if (strcmp(view_home_selected_uri(), DL_HOME_URI) == 0)\n"
                      "\t\t\t\t\tdetails_open_downloads(TAB_HOME);", app)
        # The offline levels never start a load: READY as they open.
        details = (root / "ui" / "details.c").read_text(encoding="utf-8")
        text = details[details.index("static void open_offline("):]
        text = text[:text.index("\n}\n")]
        self.assertIn("d->state = DETAIL_READY;", text)
        self.assertNotIn("DETAIL_LOADING", text)
        # Playing from the downloads queues only the tracks on the card.
        play = app[app.index("static void queue_play_offline("):]
        play = play[:play.index("\n}\n")]
        self.assertIn("r->kind != TRACK_ROW_TRACK || r->off", play)

    def test_offline_start_keeps_off_the_network(self):
        import re
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")

        def body(signature):
            text = app[app.index(signature):]
            return text[:text.index("\n}\n")]

        # The tabs start the feed only with a connection, and every search
        # they start is behind it.
        tabs = body("static tabs_exit run_tabs(")
        start = tabs.index("home_feed_start(")
        self.assertIn("if (net_online) {", tabs[start - 40:start])
        for m in re.finditer(r"(keyboard_search|search_start)\(session", tabs):
            before = tabs[max(0, m.start() - 160):m.start()]
            self.assertIn("net_online", before, tabs[m.start():m.start() + 60])
        self.assertIn("if (net_online)\n\t\t\tqueue_feed_tick(session);", tabs)
        # Offline, a track that is not on the card does not reach for an
        # access point: it fails at once.
        prep = body("static void prepare_worker(void *arg)")
        self.assertLess(prep.index("if (!job->session->online) {"),
                        prep.index("open_playback_ap("))
        # No profile request without a connection.
        self.assertIn("!session->online", body(
            "static void account_start(const spotify_session *session)\n{"))
        # A start-up that failed for the network, with a credential, goes
        # on with the downloads; one that failed for the account does not
        # (login_error_allows_offline decides), and pairing never does.
        loop = app[app.index("bool reconnecting = false;"):]
        loop = loop[:loop.index("failed:")]
        self.assertIn("login_error_allows_offline(error.category, "
                      "error.transport)", loop)
        self.assertIn("!force_pair && credential_stored()", loop)
        self.assertIn("wifi_down()", loop)
        # The username is kept for that start, and nothing else of the
        # session.
        self.assertIn('cfg_set_async("username", session.username);', loop)

    def test_losing_the_connection_goes_offline(self):
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")

        def body(signature):
            text = app[app.index(signature):]
            return text[:text.index("\n}\n")]

        # Online, the tabs watch the Wi-Fi and turn offline when it goes,
        # as after an offline start, without stopping what plays.
        self.assertIn("if (connection_lost())\n\t\t\tgo_offline(session);",
                      body("static tabs_exit run_tabs("))
        self.assertIn("wifi_down()", body("static bool connection_lost(void)"))
        lost = body("static void go_offline(spotify_session *session)")
        for call in ("session->online = false;", "tabbar_set_offline(true);",
                     "view_home_set_offline(true);",
                     "view_detail_set_offline(true);",
                     "view_results_error(STR_OFFLINE_SEARCH);",
                     "downloads_stop();"):
            self.assertIn(call, lost)
        self.assertNotIn("player_stop", lost)
        # Back online, a feed that failed meanwhile is fetched again.
        self.assertIn("home_feed_retry(",
                      body("static void go_online(spotify_session *session)"))

    def test_reconnecting_keeps_the_music(self):
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")

        def body(signature):
            text = app[app.index(signature):]
            return text[:text.index("\n}\n")]

        # Offline, retrying never leaves the tabs: no Wi-Fi says so at once,
        # with Wi-Fi the start-up runs in the background.
        retry = body("static void try_reconnect(void)")
        self.assertLess(retry.index("wifi_down()"),
                        retry.index("reconnect_start();"))
        self.assertNotIn("TABS_RECONNECT", retry)
        tabs = body("static tabs_exit run_tabs(")
        self.assertIn("case HOME_ACTION_RECONNECT:\n\t\t\t\ttry_reconnect();",
                      tabs)
        # A background start-up cannot pair: it has no screen for a code.
        self.assertIn("login.no_pair = true;",
                      body("static void reconnect_start(void)\n{"))
        self.assertIn('strstr(detail, "invalid_grant") && !login.no_pair', app)
        # The new session replaces the offline one only between two
        # preparations of the player, and under the session lock.
        poll = body("static reconnect_result reconnect_poll(spotify_session "
                    "*session)\n{")
        self.assertIn("prep.thread || player_preparing()", poll)
        self.assertLess(poll.index("LightLock_Lock(&session_lock);"),
                        poll.index("*session = reconnect_session;"))
        self.assertIn("secure_clear(&reconnect_session", poll)
        # Leaving the tabs cuts it short first.
        stop = body("static void stop_tabs(void)")
        self.assertLess(stop.index("reconnect_stop();"),
                        stop.index("player_stop();"))

    def test_native_search_encoding_and_cancellation_are_wired(self):
        source_root = build.ROOT
        app = (source_root / "main.c").read_text(
            encoding="utf-8"
        )
        spotify = (source_root / "platform3ds" / "spclient.c").read_text(
            encoding="utf-8"
        )
        search_path = (source_root / "platform3ds" / "search_path.c").read_text(
            encoding="utf-8"
        )
        self.assertIn("sp_search_path(query, path, sizeof(path))", spotify)
        self.assertIn("c == ' '", search_path)
        self.assertIn("encoded[n++] = '+'", search_path)
        self.assertIn("encoded[n++] = '%'", search_path)
        self.assertIn("encoded[n++] = hex[c >> 4]", search_path)
        self.assertIn("search.cancel_requested = true;", app)
        self.assertIn("(down & KEY_B)", app)
        # The search runs below the decoder: it can overlap playback.
        start = app[app.index("static void search_start("):
                    app.index("static void search_poll(")]
        self.assertIn("background_priority()", start)
        self.assertNotIn("0x18", start)
        # A cancel is looked at before the tracks are named, on both ways.
        fallback = app[app.index("static int search_fallback("):
                       app.index("static void search_worker(")]
        self.assertLess(fallback.index("if (job->cancel_requested)"),
                        fallback.index("search_name_tracks("))
        typed = app[app.index("static int search_typed_once("):
                    app.index("static int search_fallback(")]
        self.assertIn("&job->cancel_requested", typed)
        self.assertLess(typed.index("if (job->cancel_requested)"),
                        typed.index("if (job->count < 0)"))
        self.assertTrue(
            (build.ROOT / "ui" / "player_logic.h").is_file()
        )

    def test_typed_search_goes_through_pathfinder_with_a_fallback(self):
        import re
        root = build.ROOT
        app = (root / "main.c").read_text(encoding="utf-8")
        # Both ways stay reachable from the worker: pathfinder first, the
        # old search whenever it fails.
        searching = app[app.index("static int search_typed_once("):
                        app.index("/* What to suggest for a failed search")]
        self.assertIn("sp_search_typed(", searching)
        self.assertIn("sp_search_tracks(", searching)
        worker = searching[searching.index("static void search_worker("):]
        self.assertLess(worker.index("search_pathfinder("),
                        worker.index("search_fallback("))
        self.assertIn("pf_hash_refresh(", searching)
        # The browser User-Agent pathfinder needs is written once.
        holders = [f.name for f in sorted((root / "platform3ds").glob("*.[ch]"))
                   if "Mozilla/5.0" in f.read_text(encoding="utf-8")]
        self.assertEqual(holders, ["pathfinder.c"])
        pf_hash = (root / "platform3ds" / "pf_hash.h").read_text(
            encoding="utf-8")
        match = re.search(r'#define PF_HASH_DEFAULT\s*\\?\s*"([^"]*)"',
                          pf_hash)
        self.assertIsNotNone(match)
        self.assertRegex(match.group(1), r"^[0-9a-f]{64}$")
        # The trace exists in debug builds only, and never holds the query
        # or the token.
        self.assertIn('["-DAPP_DEBUG"] if args.debug',
                      (root / "build.py").read_text(encoding="utf-8"))
        trace = app[app.index("#ifdef APP_DEBUG\n/* One line per search"):
                    app.index("/* Names `n` track URIs")]
        self.assertIn("search_trace.log", trace)
        self.assertEqual(app.count("search_trace.log"),
                         trace.count("search_trace.log"))
        traced = re.findall(r"TRACE\(([^;]*)\);", app)
        self.assertTrue(traced)
        for call in traced:
            self.assertNotIn("query", call)
            self.assertNotIn("token", call)
        # The pure parts are in the host tests.
        native = (root.parent / "tests" / "test_native_logic.py").read_text(
            encoding="utf-8")
        self.assertIn('"search_results.c"', native)
        self.assertIn('"json3ds.c"', native)

    def test_native_queue_boundaries_and_cleanup_order_are_explicit(self):
        app = (build.ROOT / "main.c").read_text(
            encoding="utf-8"
        )
        skip = app[app.index("static void player_skip("):
                   app.index("static void player_hold(")]
        self.assertIn("queue_step(queue_pos, playback_queue_count", skip)
        logic = (build.ROOT / "ui" / "queue_logic.h").read_text(
            encoding="utf-8"
        )
        self.assertIn("next >= 0 && next < count", logic)
        # A finished track hands over to the next one by itself; a failed
        # one does not.
        pump = app[app.index("static void player_pump(void)"):
                   app.index("static void player_toggle(")]
        self.assertIn("player.phase == PLAYER_PHASE_FINISHED", pump)
        self.assertNotIn("PLAYER_PHASE_FAILED", pump)
        cleanup = app[app.index("static void player_release(void)"):
                      app.index("static void player_fail(")]
        # The download is stopped (or abandoned) before the decoder and the
        # DSP go; its buffer is freed when the worker is reaped.
        cleanup_steps = [
            "dl.stop = true;",
            "stb_vorbis_close(s->decoder);",
            "ndspChnWaveBufClear(0);",
        ]
        offsets = [cleanup.index(step) for step in cleanup_steps]
        self.assertEqual(offsets, sorted(offsets))
        reap = app[app.index("static void download_reap(void)"):
                   app.index("static void download_wait(void)")]
        self.assertLess(reap.index("threadJoin(dl.thread"),
                        reap.index("track_stream_free(&dl.stream);"))

    def test_playing_tab_keeps_audio_running_and_redraws_sparingly(self):
        ui_dir = build.ROOT / "ui"
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")
        for name in ("view_player.c", "view_player.h", "player_logic.h"):
            self.assertTrue((ui_dir / name).is_file(), name)
        # The text status line is gone: the tab draws bars (5.4, 5.5).
        self.assertNotIn("player_update_line", app)
        self.assertNotIn("STR_PLAYER_STATUS_FMT", app)
        self.assertNotIn("static playback_result playback(", app)
        player = (ui_dir / "view_player.c").read_text(encoding="utf-8")
        self.assertIn("ui_progress(player_progress_rect()", player)
        self.assertIn("ui_progress(volume, -1", player)
        # The audio is pumped every frame of the tab loop, whatever tab is
        # showing, before drawing.
        loop = app[app.index("static tabs_exit run_tabs("):
                   app.index("static void stop_tabs(")]
        self.assertLess(loop.index("player_pump();"), loop.index("frame();"))
        self.assertLess(loop.index("player_pump();"),
                        loop.index("switch (active_tab)"))
        # Pause holds the clock; the keyboard holds the track.
        elapsed = app[app.index("static unsigned player_elapsed("):
                      app.index("static player_info player_snapshot(")]
        self.assertIn("if (!player.paused && !player.held)", elapsed)
        self.assertIn("s->displayed_played_frames = played_frames", elapsed)
        self.assertIn("played_frames = s->displayed_played_frames", elapsed)
        ask = app[app.index("static bool ask_search_query("):
                  app.index("static void keyboard_search(")]
        self.assertLess(ask.index("player_hold(true);"),
                        ask.index("swkbdInputText("))
        self.assertLess(ask.index("swkbdInputText("),
                        ask.index("player_hold(false);"))
        # 9: the player only costs frames on its own tab, and the download
        # share follows the clock.
        frame = app[app.index("static void frame(void)"):
                    app.index("/* --- Search ---")]
        self.assertIn("changed |= view_player_dirty()", frame)
        self.assertIn("tabbar_draw();", frame)
        self.assertIn("player_sample_download(&download, tick,", player)
        logic = (build.ROOT / "ui" / "player_logic.h").read_text(
            encoding="utf-8")
        self.assertIn("PLAYER_DOWNLOAD_FRAMES = 60", logic)
        self.assertIn("++sample->frames >= PLAYER_DOWNLOAD_FRAMES", logic)

    def test_tabs_follow_style_guide(self):
        ui_dir = build.ROOT / "ui"
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")
        for name in ("tabbar.c", "tabbar.h", "tabs_logic.h", "view_settings.c",
                     "view_settings.h", "settings_logic.h"):
            self.assertTrue((ui_dir / name).is_file(), name)
        # Pure logic stays free of 3DS headers for the host tests.
        for name in ("tabs_logic.h", "settings_logic.h", "player_logic.h"):
            source = (ui_dir / name).read_text(encoding="utf-8")
            self.assertNotIn("#include <3ds", source)
            self.assertNotIn("#include <citro", source)
        # Icons, no labels: one per tab, generated like the others.
        tabbar = (ui_dir / "tabbar.c").read_text(encoding="utf-8")
        self.assertIn("ICON_HOME, ICON_SEARCH, ICON_PLAYING, ICON_SETTINGS",
                      tabbar)
        self.assertNotIn("C2D_Text", tabbar)
        # Touch acts on release (6.4); buttons press the new tab (6.5).
        self.assertIn("TOUCH_ACTIVATE", tabbar)
        self.assertIn("press_begin(&press[next]);", tabbar)
        # Pressed and active tabs shrink upwards instead of moving (5.12).
        self.assertIn("tabs_tab_face(tab, face)", tabbar)
        self.assertIn("pressed ? TAB_FACE_PRESSED", tabbar)
        theme = (ui_dir / "theme.h").read_text(encoding="utf-8")
        self.assertIn("TAB_LIFT_ACTIVE = 2", theme)
        self.assertIn("TAB_LIFT_PRESSED = 4", theme)
        self.assertNotIn("ui_rect_offset(r, PRESS_SHIFT, PRESS_SHIFT)", tabbar)
        # Flat and square, through the draw.c helpers.
        for name in ("tabbar.c", "view_player.c", "view_settings.c"):
            source = (ui_dir / name).read_text(encoding="utf-8")
            self.assertNotIn("C2D_DrawRect", source, name)
            self.assertNotIn("C2D_Color32", source, name)
        # L/R and ZL/ZR only ever switch tabs.
        self.assertNotIn("uses_lr", app)
        self.assertIn("KEY_L | KEY_R | KEY_ZL | KEY_ZR", tabbar)
        # Every view and the bar are made and freed with the renderer.
        for call in ("view_player_init()", "view_settings_init()"):
            self.assertIn(call, app)
        done = app.index("done:")
        for call in ("view_player_exit();", "view_settings_exit();"):
            self.assertLess(app.index(call, done), app.index("ui_exit();", done))
        # Settings holds what X, Y and SELECT did on the home view.
        settings = (ui_dir / "view_settings.c").read_text(encoding="utf-8")
        for action in ("SETTINGS_ACTION_LANGUAGE", "SETTINGS_ACTION_RECONNECT",
                       "SETTINGS_ACTION_PAIR"):
            self.assertIn(action, settings)
            self.assertIn("case " + action, app)
        style = (build.ROOT.parent / "docs" / "STYLES.md").read_text(
            encoding="utf-8")
        self.assertIn("### 5.12 Barra de pesta", style)

    def test_output_name_uses_app_title(self):
        self.assertEqual(build.output_name("spotify3ds"), "3DSpotify")
        self.assertEqual(build.output_name("probe_tls"), "probe_tls")

    def test_romfs_is_packed_into_3dsx_and_cia(self):
        romfs = self.root / "romfs"
        with self.assertRaisesRegex(SystemExit, "romfs directory is missing"):
            build.require_romfs(romfs)
        romfs.mkdir()
        self.assertEqual(build.require_romfs(romfs), romfs.resolve())

        pack = build.pack_command(Path("3dsxtool"), Path("app.elf"),
                                  Path("app.3dsx"), Path("icon.smdh"), romfs)
        self.assertIn(f"--smdh={Path('icon.smdh')}", pack)
        self.assertIn(f"--romfs={romfs}", pack)
        # Probes have neither an icon nor a romfs.
        probe = build.pack_command(Path("3dsxtool"), Path("p.elf"),
                                   Path("p.3dsx"), None, None)
        self.assertFalse(any(arg.startswith("--") for arg in probe))

        makerom = build.makerom_command(Path("makerom"), Path("app.elf"),
                                        Path("icon.smdh"), Path("b.bnr"),
                                        Path("app.cia"), romfs)
        self.assertIn(f"-DAPP_ROMFS={romfs.as_posix()}", makerom)
        self.assertTrue(Path(romfs.as_posix()).is_absolute())

        rsf = (build.ASSETS / "template.rsf").read_text(encoding="utf-8")
        self.assertRegex(rsf, r"(?m)^RomFs:\s*\n\s*(#.*\n\s*)*"
                              r"RootPath\s*:\s*\$\(APP_ROMFS\)")
        self.assertEqual(build.ROMFS, build.ROOT.parent / "romfs")
        source = (build.ROOT / "build.py").read_text(encoding="utf-8")
        self.assertIn("require_romfs(ROMFS) if app == \"3dspotify\"", source)

    def test_romfs_is_mounted_around_the_renderer(self):
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")
        self.assertIn('#include "logo.h"', app)
        self.assertLess(app.index("romfsInit()"), app.index("logo_load();"))
        self.assertLess(app.index("ui_init()"), app.index("logo_load();"))
        done = app.index("done:")
        self.assertLess(app.index("logo_free();", done),
                        app.index("ui_exit();", done))
        self.assertLess(app.index("ui_exit();", done),
                        app.index("romfsExit();", done))
        # A failed romfsInit is not fatal: the logo falls back to text.
        self.assertIn("bool romfs_ready = R_SUCCEEDED(romfsInit());", app)
        logo = (build.ROOT / "ui" / "logo.c").read_text(encoding="utf-8")
        self.assertIn("STR_APP_NAME", logo)
        self.assertIn("GPU_NEAREST", logo)
        self.assertIn("image_copy_to_texture(", logo)
        self.assertIn("stbi_info_from_memory(", logo)

    def test_top_now_reuses_the_bottom_player(self):
        ui_dir = build.ROOT / "ui"
        layout = (ui_dir / "now_layout.h").read_text(encoding="utf-8")
        self.assertNotIn("#include <3ds", layout)
        self.assertNotIn("#include <citro", layout)
        top = (ui_dir / "top_now.c").read_text(encoding="utf-8")
        # The same bar, times, fitting, sampling and texts as the player.
        for call in ("ui_progress(", "player_progress_permille(",
                     "player_format_time(", "ui_parse_fitted(",
                     "player_sample_download(", "STR_PLAYER_FINISHED_TITLE",
                     "STR_UNTITLED", "STR_UNKNOWN_ARTIST",
                     "artwork_draw_in(art, now_art_rect(), ART_SHADOW_OFF)",
                     "artwork_draw_in(art, now_big_art_rect(), "
                     "ART_SHADOW_OFF)"):
            self.assertIn(call, top)
        for copy in ("static float measure(", "%u:%02u", ">= 60"):
            self.assertNotIn(copy, top)
        player = (ui_dir / "view_player.c").read_text(encoding="utf-8")
        self.assertNotIn("static void format_time(", player)
        self.assertNotIn("static float measure(", player)
        artwork = (ui_dir / "artwork.c").read_text(encoding="utf-8")
        self.assertIn("ui_art_or(box,", artwork)
        theme = (ui_dir / "theme.h").read_text(encoding="utf-8")
        self.assertIn("ART_BIG = 216", theme)

    def test_session_start_up_runs_in_a_worker(self):
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")
        # One global job, never on a stack: the worker holds its address.
        self.assertIn("static session_job login;", app)
        self.assertIn("#define SESSION_STACK (64 * 1024)", app)
        self.assertIn("threadCreate(session_worker, NULL, SESSION_STACK,\n"
                      "\t                            background_priority()",
                      app)
        # The worker's large locals live off its stack.
        for name in ("static spotify_tokens login_tokens;",
                     "static pair_request login_request;",
                     "static ap_info login_ap;",
                     "static login5_token login_library;"):
            self.assertIn(name, app)
        # What runs in the worker never draws nor reads the buttons.
        worker = app[app.index("static bool pair_account("):
                     app.index("static void session_worker(")]
        for main_only in ("frame()", "hidScanInput", "hidKeysDown",
                          "textscreen_", "show_progress", "aptMainLoop"):
            self.assertNotIn(main_only, worker)
        self.assertIn("session_stage(STR_AP_AUTHENTICATING);", worker)
        # The wait between polls notices a cancel within a slice.
        pair = app[app.index("static bool pair_account("):
                   app.index("static bool resolve_and_login(")]
        self.assertIn("session_wait(interval)", pair)
        self.assertIn("secure_clear(req, sizeof(*req));", pair)
        # A cancel that races an approval saves nothing.
        self.assertLess(pair.index("if (login.cancel) {"),
                        pair.index("cfg_save_refresh_token("))
        # The device code is secret: it never reaches the screen's copy.
        shared = app[app.index("typedef struct {\n\tbool force_pair;"):
                     app.index("static session_job login;")]
        self.assertNotIn("device_code", shared)
        publish = app[app.index("static void pair_publish("):
                      app.index("static void pair_snapshot(")]
        self.assertNotIn("device_code", publish)
        self.assertIn("LightLock_Lock(&login.lock);", publish)
        # Cancelling is stop_tabs()'s pattern: abort, join, un-abort.
        cancel = app[app.index("static void session_cancel(void)"):
                     app.index("typedef enum {\n\tLOGIN_READY")]
        self.assertLess(cancel.index("tls_set_abort(true);"),
                        cancel.index("threadJoin(login.thread"))
        self.assertLess(cancel.index("threadFree(login.thread);"),
                        cancel.index("tls_set_abort(false);"))
        # The main thread keeps drawing; B cancels only a pairing, and
        # closing the app cancels whatever runs.
        loop = app[app.index("static login_result run_login("):
                   app.index("static login_action run_error(")]
        self.assertIn("while (!login.done)", loop)
        self.assertIn("frame();", loop)
        self.assertIn("action == LOGIN_ACTION_CANCEL && login.pairing", loop)
        self.assertLess(loop.index("if (!aptMainLoop()) {"),
                        loop.index("return LOGIN_QUIT;"))
        self.assertIn("session_cancel();", loop)
        self.assertIn("run_login(force_pair, &session, &error)", app)
        # The 90-frame "credential expired" screen is gone: the worker
        # goes straight to pairing and says why.
        self.assertNotIn("i < 90", app)
        self.assertIn("login.reason_expired = true;", app)

    def test_login_view_owns_both_screens_before_the_session(self):
        ui_dir = build.ROOT / "ui"
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")
        view = (ui_dir / "view_login.c").read_text(encoding="utf-8")
        for name in ("view_login.c", "view_login.h", "login_logic.h"):
            self.assertTrue((ui_dir / name).is_file(), name)
        # Pure logic stays free of 3DS headers for the host tests.
        logic = (ui_dir / "login_logic.h").read_text(encoding="utf-8")
        self.assertNotIn("#include <3ds", logic)
        self.assertNotIn("#include <citro", logic)
        # On-demand render: the view joins frame() for both screens.
        frame = app[app.index("static void frame(void)"):
                    app.index("/* --- Search ---")]
        self.assertIn("changed = view_login_dirty();", frame)
        self.assertIn("view_login_draw_top();", frame)
        self.assertIn("view_login_draw_bottom();", frame)
        self.assertIn("!view_login_init()", app)
        done = app.index("done:")
        self.assertLess(app.index("view_login_exit();", done),
                        app.index("ui_exit();", done))
        # Connecting, pairing and both errors are the view's, not text.
        self.assertIn("active_view = VIEW_LOGIN;", app)
        for gone in ("show_error(", "show_pair_screen(", "show_progress(",
                     "language_toggled("):
            self.assertNotIn(gone, app)
        self.assertIn("run_error(&error, true);", app)
        # Style guide 6: presses show, touch acts on lift, and actions wait
        # for their press; the spinner animates without a veil (7.2).
        self.assertIn("press_shown(pending_press)", view)
        self.assertIn("touch_track_update(", view)
        self.assertIn("ui_spinner_ring(", view)
        self.assertNotIn("ui_spinner(", view)
        self.assertIn("anim_frames / LOADER_STEP", view)
        # The QR: encoded once per URL, drawn in runs, `tinta` on `texto`;
        # the secret device code never reaches the view.
        self.assertIn("if (strcmp(qr_encoded, qr_url) == 0)", view)
        self.assertIn("qrcodegen_encodeText(", view)
        self.assertNotIn("qrcodegen_encodeText(", view[
            view.index("void view_login_draw_bottom(void)"):])
        self.assertIn("ui_fill(box, COL_TEXTO);", view)
        self.assertIn("ui_fill(run, COL_TINTA);", view)
        self.assertNotIn("device_code", view)
        self.assertNotIn("device_code",
                         (ui_dir / "view_login.h").read_text(encoding="utf-8"))
        # The URL in `lima` is only allowed on `capa` (2.6): inside the
        # panel, drawn after it.
        top = view[view.index("static void draw_pair_top(void)"):
                   view.index("void view_login_draw_top(void)")]
        self.assertLess(top.index("ui_panel(panel);"), top.index("COL_LIMA"))
        # One primary button per screen (5.2).
        self.assertEqual(view.count("UI_BUTTON_PRIMARY"), 1)
        # The fatal error only exits: no buttons to retry or pair.
        self.assertIn("case LOGIN_FATAL:", view)
        self.assertIn("if (!buttons)\n\t\treturn;", view)

    def test_top_screen_is_composed_by_top(self):
        ui_dir = build.ROOT / "ui"
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")
        frame = app[app.index("static void frame(void)"):
                    app.index("/* --- Search ---")]
        # One compositor for the tabs; before the session, the login view.
        self.assertIn("top_set(&info);", frame)
        self.assertIn("top_dirty(active_tab, &cover_art)", frame)
        self.assertIn("top_draw(active_tab, &cover_art);", frame)
        self.assertIn("view_login_draw_top();", frame)
        self.assertNotIn("artwork_draw(", app)
        self.assertNotIn("view_player_dirty() || artwork_dirty", frame)
        for gone in ("view_home_draw_top", "view_results_draw_top",
                     "view_settings_draw_top"):
            self.assertNotIn(gone, app)
            for path in ui_dir.glob("*.[ch]"):
                self.assertNotIn(gone, path.read_text(encoding="utf-8"),
                                 path.name)
        done = app.index("done:")
        self.assertLess(app.index("top_exit();", done),
                        app.index("ui_exit();", done))
        self.assertIn("!top_init()", app)
        logic = (ui_dir / "top_logic.h").read_text(encoding="utf-8")
        self.assertNotIn("#include <3ds", logic)
        self.assertNotIn("#include <citro", logic)
        top = (ui_dir / "top.c").read_text(encoding="utf-8")
        self.assertIn("logo_draw_centered(", top)
        self.assertNotIn("view_results", top)
        # The download spinner: centred, on any tab, animated by steps.
        self.assertIn("ui_spinner(SCREEN_TOP_W / 2, SCREEN_H / 2, "
                      "spinner_step())", top)
        self.assertIn("top_shows_spinner(info->phase, info->download_pct)",
                      top)
        # Its translucent backdrop is the only alpha in the UI (2.7).
        for path in [build.ROOT / "main.c", *ui_dir.glob("*.[ch]")]:
            if path.name not in ("theme.h", "draw.c"):
                self.assertNotIn("SCRIM_", path.read_text(encoding="utf-8"),
                                 path.name)
        draw = (ui_dir / "draw.c").read_text(encoding="utf-8")
        self.assertEqual(draw.count("SCRIM_COLOR"), 1)
        self.assertIn("ui_fill(spinner_backdrop_rect(cx, cy), SCRIM_COLOR);",
                      draw)
        spinner = (ui_dir / "spinner_logic.h").read_text(encoding="utf-8")
        self.assertNotIn("#include <3ds", spinner)
        self.assertNotIn("#include <citro", spinner)
        # The search bubble is on the bottom screen: 5.9 with the loading
        # indicator, and the language bubble stacked above it.
        results = (ui_dir / "view_results.c").read_text(encoding="utf-8")
        self.assertIn("ui_bubble_loading(list_bubble_rect(", results)
        self.assertIn("list_bubble_slot_rect(", results)
        for gone in ("query_title_text", "range_text", "detail_title",
                     "error_detail_text", "header_text"):
            self.assertNotIn(gone, results)

    def test_app_data_uses_3dspotify_directory_on_clean_install(self):
        cfg = (build.ROOT / "platform3ds" / "cfg3ds.c").read_text(
            encoding="utf-8"
        )
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")
        self.assertIn('#define APP_DATA_DIR "sdmc:/3ds/3DSpotify/"', cfg)
        self.assertNotIn("spotibacan", cfg)
        self.assertIn('mkdir("sdmc:/3ds", 0777);', cfg)
        self.assertIn('mkdir("sdmc:/3ds/3DSpotify", 0777);', cfg)
        self.assertIn("cfg_set_path(NULL);", app)

    def test_player_preserves_audio_key_refusal_diagnostic(self):
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")
        begin = app[app.index("static void player_begin_audio(void)"):
                    app.index("static int free_wave_index(")]
        self.assertLess(begin.index('"%s", prep.error'),
                        begin.index("prepare_reap();"))
        self.assertIn(
            'if (!error[0])\n\t\t\tsnprintf(error, sizeof(error), "%s",',
            begin,
        )
        self.assertNotIn("if (rc == AP_KEY_REFUSED)", begin)
        self.assertIn("player_fail(error);", begin)
        ap = (build.ROOT / "platform3ds" / "ap3ds.c").read_text(
            encoding="utf-8"
        )
        self.assertIn("Audio key refused (0x%04X)", ap)
        self.assertIn("Audio key refused (no server code)", ap)
        self.assertIn("packet=0x%02X len=%d\\nseq=%lu", ap)
        track = (build.ROOT / "platform3ds" / "track.c").read_text(
            encoding="utf-8"
        )
        self.assertEqual(track.count("out->key_refused_code = refused;"), 2)

    def test_audio_key_sessions_reauthenticate_after_oauth(self):
        source = (build.ROOT / "platform3ds" / "ap3ds.c").read_text(
            encoding="utf-8"
        )
        login = source[source.index("ap_session *ap_login("):
                       source.index("ap_session *ap_login_any(")]
        self.assertEqual(login.count("ap_login_credentials("), 2)
        self.assertIn("NULL, AUTH_SPOTIFY_TOKEN", login)
        self.assertLess(login.index("ap_close(s);"),
                        login.index("bootstrap.username"))
        self.assertIn("bootstrap.reusable_type, bootstrap.reusable", login)
        self.assertIn("bootstrap.reusable_len, device_id", login)
        self.assertIn("strcmp(nfo->username, bootstrap.username) != 0", login)
        self.assertIn("mbedtls_platform_zeroize(&bootstrap", login)
        self.assertIn("mbedtls_platform_zeroize(&local", login)
        self.assertIn("mbedtls_platform_zeroize(nfo->reusable", login)
        credentials = source[
            source.index("static ap_session *ap_login_credentials("):
            source.index("ap_session *ap_login(")
        ]
        self.assertIn("pb_str(&creds, 0x0a, username)", credentials)
        self.assertIn("pb_varint(&creds, 0x14, (uint64_t)auth_type)", credentials)
        self.assertIn("pb_bytes(&creds, 0x1e, auth_data, auth_len)", credentials)
        self.assertIn("pb_get_varint(buf, (size_t)n, 0x1e, &reusable_type)",
                      credentials)
        self.assertIn("reusable_type != AUTH_STORED_SPOTIFY", credentials)
        self.assertIn("reusable_type != AUTH_STORED_FACEBOOK", credentials)
        self.assertIn("!reusable.len", credentials)
        self.assertIn("mbedtls_platform_zeroize(login_store", credentials)
        self.assertIn("mbedtls_platform_zeroize(buf, AP_MAX_PACKET)",
                      credentials)

    def test_loading_a_track_never_blocks_the_main_thread(self):
        ui_dir = build.ROOT / "ui"
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")
        # The network steps run in the preparation thread, polled per frame.
        worker = app[app.index("static void prepare_worker("):
                     app.index("static void prepare_reap(")]
        self.assertIn("open_playback_ap(job->session", worker)
        self.assertIn("track_stream_begin_cached(", worker)
        # The definition, the preparation and the downloads' callback, which
        # only their worker calls (downloads.h, downloads_ap_fn).
        self.assertEqual(app.count("open_playback_ap("), 3)
        callback = app[app.index("static ap_session *downloads_open_ap("):]
        self.assertIn("open_playback_ap(downloads_session",
                      callback[:callback.index("\n}\n")])
        # The definition, and the engine's start: with the tabs, and when
        # they come back online.
        self.assertEqual(app.count("downloads_open_ap"), 3)
        self.assertEqual(app.count("track_stream_begin_cached("), 1)
        self.assertIn("threadCreate(prepare_worker, &prep, PREPARE_STACK,", app)
        pump = app[app.index("static void player_pump(void)"):
                   app.index("static void player_toggle(void)")]
        self.assertIn("player_begin_audio();", pump)
        self.assertIn("if (prep.done)", pump)
        begin = app[app.index("static void player_begin_audio(void)"):
                    app.index("static int free_wave_index(")]
        self.assertNotIn("player_stage(", begin)
        # A skipped track's preparation is abandoned, not waited for; only
        # a session going away waits.
        release = app[app.index("static void player_release(void)"):
                      app.index("static void player_fail(")]
        self.assertIn("prep.abandoned = true;", release)
        # The same for a download: skipping never waits for its request.
        self.assertIn("dl_abandoned = true;", release)
        self.assertNotIn("download_wait(", release)
        self.assertNotIn("download_stop_join", app)
        self.assertIn("if (dl_abandoned && dl.done)\n\t\tdownload_reap();",
                      pump)
        # The next track only prepares once the old download is gone, and
        # the thumbnails' and the downloads' requests are over.
        start = app[app.index("static void player_start_prepare(void)"):
                    app.index("static void player_begin_audio(void)")]
        self.assertIn("if (prep.thread || dl.thread || thumbs_stopping() || "
                      "downloads_busy())", start)
        stop = app[app.index("static void player_stop(void)"):
                   app.index("static bool player_preparing(void)")]
        self.assertIn("prepare_wait();", stop)
        self.assertIn("download_wait();", stop)
        # Neither the thumbnails nor the cover join a busy worker.
        thumbs = (ui_dir / "thumbs.c").read_text(encoding="utf-8")
        stop_fn = thumbs[thumbs.index("void thumbs_stop(void)"):
                         thumbs.index("bool thumbs_stopping(void)")]
        self.assertNotIn("threadJoin", stop_fn)
        artwork = (ui_dir / "artwork.c").read_text(encoding="utf-8")
        request = artwork[artwork.index("void artwork_request("):
                          artwork.index("bool artwork_pending(")]
        self.assertNotIn("artwork_close(", request)
        self.assertIn("artwork->has_next = true;", request)
        # Leaving the tabs waits for every worker that may hold a socket,
        # before the app frees TLS and the sockets: one left running would
        # use them freed (it crashed the console on START).
        tabs_stop = app[app.index("static void stop_tabs(void)"):
                        app.index("/* --- Session start-up")]
        for wait in ("player_stop();", "search_stop();", "thumbs_wait();"):
            self.assertIn(wait, tabs_stop)
        self.assertNotIn("thumbs_stop();", tabs_stop)
        # ... and those joins are quick: every network wait is cut short.
        self.assertLess(tabs_stop.index("tls_set_abort(true);"),
                        tabs_stop.index("player_stop();"))
        self.assertLess(tabs_stop.index("thumbs_wait();"),
                        tabs_stop.index("tls_set_abort(false);"))
        platform = build.ROOT / "platform3ds"
        tls = (platform / "tls3ds.c").read_text(encoding="utf-8")
        wait_tls = tls[tls.index("static int wait_io(tls_conn *c"):
                       tls.index("static int bio_send(")]
        self.assertIn("poll_abortable(", wait_tls)
        self.assertIn('poll_abortable(c->host, "tls-send"', tls)
        ap = (platform / "ap3ds.c").read_text(encoding="utf-8")
        self.assertEqual(ap.count("poll(&p, 1, timeout_ms)"), 0)
        self.assertIn("if (tls_aborting())", ap)
        done = app.index("done:")
        for step in ("stop_tabs();", "artwork_close(&cover_art);"):
            self.assertLess(app.index(step, done),
                            app.index("tls_global_exit();", done))
            self.assertLess(app.index(step, done),
                            app.index("socExit();", done))
        wait_fn = thumbs[thumbs.index("void thumbs_wait(void)"):
                         thumbs.index("void thumbs_exit(void)")]
        self.assertIn("threadJoin(worker, U64_MAX);", wait_fn)
        # Two threads may write the config file: it is locked.
        cfg = (build.ROOT / "platform3ds" / "cfg3ds.c").read_text(
            encoding="utf-8")
        self.assertEqual(cfg.count("LightLock_Lock(&file_lock);"), 3)

    def test_gitignore_keeps_romfs(self):
        ignore = (build.ROOT.parent / ".gitignore").read_text(encoding="utf-8")
        for line in ignore.splitlines():
            self.assertNotIn("romfs", line.split("#")[0])

    def test_logo_png_is_small_rgba(self):
        # Drawn 1:1, so at most 256x128; its alpha may blend (2.7 makes an
        # exception for the logo).
        logo = build.ROMFS / "gfx" / "logo.png"
        self.assertTrue(logo.is_file(), f"{logo} is missing")
        width, height, pixels = read_png_rgba(logo.read_bytes())
        self.assertLessEqual(width, 256, "logo wider than 256 px")
        self.assertLessEqual(height, 128, "logo taller than 128 px")
        self.assertEqual(len(pixels), width * height * 4)

    def test_ui_text_comes_from_language_tables(self):
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")
        self.assertIn('#include "lang.h"', app)
        self.assertIn("language_init();", app)
        # SELECT: in the tabs' views and in the login view, which hands it
        # back to main.c to switch.
        login = (build.ROOT / "ui" / "view_login.c").read_text(encoding="utf-8")
        self.assertIn("down & KEY_SELECT", login)
        self.assertIn("if (action == LOGIN_ACTION_LANGUAGE)\n"
                      "\t\t\tswitch_language();", app)
        # Saved in the background: rewriting the config stalls a frame.
        self.assertIn('cfg_set_async("language", lang_code(lang_get()))', app)
        for spanish in ("Reproduccion local", "Resultados de Spotify",
                        "Sesion Spotify lista", "emparejamiento"):
            self.assertNotIn(spanish, app)

    def test_local_stb_vorbis_must_match_pinned_checksum(self):
        source = self.root / "deps" / "stb_vorbis.c"
        source.parent.mkdir()
        source.write_bytes(b"modified source")
        with patch.object(build, "ROOT", self.root):
            with self.assertRaisesRegex(SystemExit, "checksum mismatch"):
                build.stb_vorbis_source(False)

    def test_stb_vorbis_download_is_verified(self):
        payload = b"/* public API */\n//  HEADER ENDS HERE\n/* implementation */"
        with patch.object(build, "ROOT", self.root):
            with patch.object(build, "STB_VORBIS_SHA256",
                              hashlib.sha256(payload).hexdigest()):
                with patch.object(build.urllib.request, "urlopen",
                                  return_value=io.BytesIO(payload)):
                    source = build.stb_vorbis_source(True)
        self.assertEqual(source.read_bytes(), payload)
        self.assertEqual((self.root / "deps" / "stb_vorbis.h").read_bytes(),
                         b"/* public API */\n")

    def test_stb_vorbis_download_rejects_wrong_checksum(self):
        with patch.object(build, "ROOT", self.root):
            with patch.object(build.urllib.request, "urlopen",
                              return_value=io.BytesIO(b"corrupt")):
                with self.assertRaisesRegex(SystemExit, "checksum mismatch"):
                    build.stb_vorbis_source(True)
        self.assertFalse((self.root / "deps").exists())

    def test_qrcodegen_is_pinned_and_built_without_warnings(self):
        _, _, _, extra_sources = build.APPS["3dspotify"]
        self.assertIn("deps/qrcodegen.c", extra_sources)
        # The committed copies are the pinned ones, so an offline build works.
        source = build.qrcodegen_source(False)
        self.assertEqual(source, build.ROOT / "deps" / "qrcodegen.c")
        self.assertTrue((build.ROOT / "deps" / "qrcodegen.h").is_file())
        # A CRLF checkout would change the bytes the checksum pins.
        attributes = (build.ROOT.parent / ".gitattributes").read_text(
            encoding="utf-8")
        self.assertIn("source/deps/*.c -text", attributes)
        self.assertIn("source/deps/*.h -text", attributes)
        notice = (build.ROOT.parent / "NOTICE").read_text(encoding="utf-8")
        self.assertIn("QR-Code-generator", notice)

    def test_missing_qrcodegen_offline_has_actionable_error(self):
        with patch.object(build, "ROOT", self.root):
            with self.assertRaisesRegex(SystemExit, "--fetch-deps"):
                build.qrcodegen_source(False)

    def test_local_qrcodegen_must_match_pinned_checksum(self):
        deps = self.root / "deps"
        deps.mkdir()
        (deps / "qrcodegen.c").write_bytes(b"modified source")
        with patch.object(build, "ROOT", self.root):
            with self.assertRaisesRegex(SystemExit, "qrcodegen.c checksum"):
                build.qrcodegen_source(False)

    def test_qrcodegen_download_is_verified(self):
        files = {"qrcodegen.c": b"/* impl */", "qrcodegen.h": b"/* api */"}
        def answer(url, timeout):
            return io.BytesIO(files[url.rsplit("/", 1)[1]])
        with patch.object(build, "ROOT", self.root):
            with patch.multiple(
                    build,
                    QRCODEGEN_SHA256_C=hashlib.sha256(files["qrcodegen.c"]).hexdigest(),
                    QRCODEGEN_SHA256_H=hashlib.sha256(files["qrcodegen.h"]).hexdigest()):
                with patch.object(build.urllib.request, "urlopen",
                                  side_effect=answer) as fetch:
                    source = build.qrcodegen_source(True)
        self.assertEqual(source.read_bytes(), files["qrcodegen.c"])
        self.assertEqual((self.root / "deps" / "qrcodegen.h").read_bytes(),
                         files["qrcodegen.h"])
        for call in fetch.call_args_list:
            self.assertIn(f"/v{build.QRCODEGEN_VERSION}/c/", call.args[0])

    def test_qrcodegen_download_writes_nothing_unless_both_match(self):
        payload = b"/* impl */"
        with patch.object(build, "ROOT", self.root):
            with patch.object(build, "QRCODEGEN_SHA256_C",
                              hashlib.sha256(payload).hexdigest()):
                with patch.object(build.urllib.request, "urlopen",
                                  side_effect=lambda url, timeout:
                                  io.BytesIO(payload)):
                    with self.assertRaisesRegex(SystemExit,
                                                "qrcodegen.h checksum"):
                        build.qrcodegen_source(True)
        self.assertFalse((self.root / "deps").exists())

    def test_missing_mbedtls_offline_has_actionable_error(self):
        with patch.object(build, "ROOT", self.root):
            with self.assertRaisesRegex(SystemExit, "--fetch-deps"):
                build.mbedtls_tree(False)

    def test_existing_mbedtls_does_not_use_network(self):
        tree = self.root / "deps" / f"mbedtls-{build.MBEDTLS_VERSION}"
        header = tree / "include" / "mbedtls" / "ssl.h"
        header.parent.mkdir(parents=True)
        header.touch()
        with patch.object(build, "ROOT", self.root):
            with patch.object(build.urllib.request, "urlopen") as fetch:
                self.assertEqual(build.mbedtls_tree(False), tree)
                fetch.assert_not_called()

    def test_download_rejects_wrong_checksum(self):
        with patch.object(build, "ROOT", self.root):
            with patch.object(build.urllib.request, "urlopen",
                              return_value=io.BytesIO(b"corrupt")):
                with self.assertRaisesRegex(SystemExit, "checksum mismatch"):
                    build.mbedtls_tree(True)
        self.assertFalse((self.root / "deps").exists())

    def test_verified_download_extracts_sources(self):
        data = io.BytesIO()
        name = f"mbedtls-{build.MBEDTLS_VERSION}"
        with tarfile.open(fileobj=data, mode="w:bz2") as archive:
            info = tarfile.TarInfo(f"{name}/include/mbedtls/ssl.h")
            info.size = 4
            archive.addfile(info, io.BytesIO(b"test"))
        payload = data.getvalue()
        with patch.object(build, "ROOT", self.root):
            with patch.object(build, "MBEDTLS_SHA256",
                              hashlib.sha256(payload).hexdigest()):
                with patch.object(build.urllib.request, "urlopen",
                                  return_value=io.BytesIO(payload)):
                    tree = build.mbedtls_tree(True)
        self.assertEqual((tree / "include" / "mbedtls" / "ssl.h").read_bytes(),
                         b"test")

    def test_bars_seek_and_set_volume_by_touch(self):
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")
        view = (build.ROOT / "ui" / "view_player.c").read_text(
            encoding="utf-8")
        track = (build.ROOT / "platform3ds" / "track.c").read_text(
            encoding="utf-8")
        # The view turns a drag into actions; main acts on them.
        self.assertIn("return PLAYER_ACTION_SEEK;", view)
        self.assertIn("return PLAYER_ACTION_VOLUME_SET;", view)
        self.assertIn("player_seek(view_player_value());", app)
        # Only with the whole file downloaded.
        self.assertIn("dl.stream.dl_done &&", app)
        seek_fn = app[app.index("static void player_seek("):
                      app.index("/* One step of download")]
        self.assertIn("!dl.stream.dl_done", seek_fn)
        # A seek drops the queued audio, flushes the decoder and sends the
        # download where it is needed.
        seek = app[app.index("static void seek_to_byte("):
                   app.index("static size_t seek_guess(")]
        self.assertIn("ndspChnWaveBufClear(0);", seek)
        self.assertIn("stb_vorbis_flush_pushdata(s->decoder);", seek)
        self.assertIn("track_stream_seek(&dl.stream, at);", seek)
        # The decoder only reads bytes that are there.
        pump = app[app.index("static pump_step pump_once(void)"):
                   app.index("static void player_pump(void)")]
        self.assertNotIn("dl.stream.downloaded", pump)
        self.assertIn("track_stream_available(&dl.stream, s->source_pos)",
                      pump)
        # The download never writes over a chunk that is already whole.
        self.assertIn("if (span_blocked(ts)) {", track)
        self.assertIn("stream_next_missing(ts->chunk_done", track)


if __name__ == "__main__":
    unittest.main()
