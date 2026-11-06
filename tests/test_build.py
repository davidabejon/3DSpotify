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
        source_dirs, _, mbedtls, extra_sources = build.APPS["spotify3ds"]
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

    def test_native_target_uses_only_direct_artwork_pipeline(self):
        source_dirs, _, _, _ = build.APPS["spotify3ds"]
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
        for name in ("ui.c", "ui.h", "textscreen.c", "textscreen.h",
                     "image_util.h", "worker_priority.h"):
            self.assertTrue((ui_dir / name).is_file(), name)
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
                    app.index("static void show_progress(")]
        self.assertIn("textscreen_dirty()", frame)
        self.assertIn("artwork_dirty(&cover_art)", frame)
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
        for name in ("home_logic.h", "text_fit.h", "theme.h", "layout.h",
                     "press_logic.h"):
            source = (ui_dir / name).read_text(encoding="utf-8")
            self.assertNotIn("#include <3ds", source)
            self.assertNotIn("#include <citro", source)
        # The home view joins the on-demand render: a frame only when dirty.
        frame = app[app.index("static void frame(void)"):
                    app.index("static void show_progress(")]
        self.assertIn("view_home_dirty()", frame)
        self.assertIn("view_home_draw_bottom()", frame)
        # The repeat button runs the last query without the keyboard, and a
        # cancelled keyboard does not wipe it.
        self.assertIn("case HOME_ACTION_REPEAT:", app)
        ask = app[app.index("static bool ask_search_query("):
                  app.index("static void keyboard_search(")]
        self.assertIn("if (confirmed)", ask)
        # Reconnecting and pairing moved to the Settings tab.
        home = (ui_dir / "view_home.c").read_text(encoding="utf-8")
        self.assertNotIn("HOME_ACTION_RECONNECT", home)
        self.assertNotIn("HOME_ACTION_PAIR", home)

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
        for name in ("view_home.c", "view_results.c", "draw.c"):
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
        for source, name in ((draw, "draw.c"), (home, "view_home.c"),
                             (results, "view_results.c")):
            for call in ("C2D_DrawCircle", "C2D_DrawTriangle",
                         "C2D_DrawRectangle(", "C2D_DrawEllipse",
                         "color_fade", "color_mix"):
                self.assertNotIn(call, source, name)
        self.assertNotIn("C2D_DrawRect", home)
        self.assertNotIn("C2D_DrawRect", results)
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
        self.assertIn("move_focus(RESULTS_PER_PAGE)", results)
        self.assertIn("else if (down & KEY_RIGHT)\n\t\t\tmove_focus("
                      "RESULTS_PER_PAGE);", results)
        self.assertNotIn("KEY_L)", results)
        self.assertNotIn("KEY_R)", results)
        # Rows are cut in pixels, never the duration (5.3, 3.3).
        self.assertIn("parse_fitted(&row_title[i]", results)
        # The thumbnail and its note placeholder are the shared ui_art.
        self.assertIn("ui_art(list_thumb_rect(row)", results)
        draw = (ui_dir / "draw.c").read_text(encoding="utf-8")
        self.assertIn("ICON_NOTE", draw)
        # On-demand render and view switching.
        frame = app[app.index("static void frame(void)"):
                    app.index("static void show_progress(")]
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
        # The list follows the queue the player moves through, and errors
        # can be retried.
        self.assertIn("view_results_focus(index)", app)
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
                        play.index("player_play(session, index);"))
        loop = app[app.index("static tabs_exit run_tabs("):
                   app.index("static void stop_tabs(")]
        self.assertIn("view_results_allow_thumbs(!player_downloading());",
                      loop)
        # Joining a worker blocks the decoder: the track pauses meanwhile.
        stop_search = app[app.index("static void search_stop(void)"):
                          app.index("static void search_start(")]
        self.assertLess(stop_search.index("player_hold(true);"),
                        stop_search.index("search_join();"))
        stop = app[app.index("static void stop_tabs("):]
        self.assertIn("thumbs_stop();", stop[:stop.index("\n}\n")])
        results_src = (ui_dir / "view_results.c").read_text(encoding="utf-8")
        allow = results_src[results_src.index(
            "void view_results_allow_thumbs("):]
        self.assertIn("thumbs_stop();", allow[:allow.index("\n}\n")])
        self.assertIn("thumbs_init();", app)
        self.assertLess(app.index("thumbs_exit();", app.index("done:")),
                        app.index("ui_exit();", app.index("done:")))
        # The view: uploads on update, a fixed-size slot either way.
        results = (ui_dir / "view_results.c").read_text(encoding="utf-8")
        self.assertIn("thumbs_poll()", results)
        self.assertIn("thumbs_image(index, &image)", results)
        self.assertIn("thumbs_set_window(first)", results)
        # Nothing thumbs.c holds for another list is drawn.
        self.assertIn("thumbs_synced && thumbs_image(index, &image)", results)

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
        self.assertIn("player_play(session, index);", play)
        self.assertIn("switch_tab(TAB_PLAYER);", play)
        self.assertNotIn("hidKeysDown", play)
        # Dragging (6.9) and the scroll indicator (5.11).
        results = (ui_dir / "view_results.c").read_text(encoding="utf-8")
        self.assertIn("list_drag_begin(&drag", results)
        # A drag starts on the list, never on the tab bar.
        self.assertIn("list_area_contains(pos.px, pos.py)", results)
        self.assertIn("!tabbar_touching()", results)
        self.assertIn("list_drag_update(&drag", results)
        self.assertIn("press_cancel(&press_row)", results)
        # A drag may leave rows cut; the buttons align to whole rows.
        self.assertIn("list_align(scroll, selected, count)", results)
        # The focus survives a drag (6.9), and the first button press with
        # it out of view only brings the list back to it (6.6).
        drag_end = results[results.index("static void drag_end(void)"):]
        drag_end = drag_end[:drag_end.index("\n}\n")]
        for change in ("list_snap", "scroll =", "selected ="):
            self.assertNotIn(change, drag_end)
        self.assertIn("!list_row_whole(selected, scroll, count)", results)
        self.assertNotIn("list_keep_visible", results)
        self.assertIn("list_scroll_thumb(scroll, count)", results)
        self.assertIn("list_hit_row(pos.px, pos.py, scroll, count)", results)
        style = (build.ROOT.parent / "docs" / "STYLES.md").read_text(
            encoding="utf-8")
        self.assertIn("### 5.11 Indicador de desplazamiento", style)
        self.assertIn("### 6.9 Arrastre en listas", style)

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
        self.assertLess(
            app.index("if (!job->cancel_requested"),
            app.index("job->count = sp_track_metadata_batch"),
        )
        self.assertTrue(
            (build.ROOT / "ui" / "player_logic.h").is_file()
        )

    def test_native_queue_boundaries_and_cleanup_order_are_explicit(self):
        app = (build.ROOT / "main.c").read_text(
            encoding="utf-8"
        )
        skip = app[app.index("static void player_skip("):
                   app.index("static void player_hold(")]
        self.assertIn("playback_queue_step(player.index, playback_queue_count",
                      skip)
        logic = (build.ROOT / "ui" / "player_logic.h").read_text(
            encoding="utf-8"
        )
        self.assertIn("next >= 0 && next < count", logic)
        cleanup = app[app.index("static void player_release(void)"):
                      app.index("static void player_fail(")]
        cleanup_steps = [
            "download_stop_join(&s->job);",
            "stb_vorbis_close(s->decoder);",
            "ndspChnWaveBufClear(0);",
            "track_stream_free(&s->job.stream);",
        ]
        offsets = [cleanup.index(step) for step in cleanup_steps]
        self.assertEqual(offsets, sorted(offsets))

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
                    app.index("static void show_progress(")]
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
        self.assertIn("require_romfs(ROMFS) if app == \"spotify3ds\"", source)

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
        self.assertIn("ui_art(box,", artwork)
        theme = (ui_dir / "theme.h").read_text(encoding="utf-8")
        self.assertIn("ART_BIG = 216", theme)

    def test_top_screen_is_composed_by_top(self):
        ui_dir = build.ROOT / "ui"
        app = (build.ROOT / "main.c").read_text(encoding="utf-8")
        frame = app[app.index("static void frame(void)"):
                    app.index("static void show_progress(")]
        # One compositor for the tabs; the text screens keep the artwork.
        self.assertIn("top_set(&info);", frame)
        self.assertIn("top_dirty(active_tab, &cover_art)", frame)
        self.assertIn("top_draw(active_tab, &cover_art);", frame)
        self.assertIn("artwork_draw(&cover_art);", frame)
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
        self.assertIn("down & KEY_SELECT", app)
        self.assertIn('cfg_set("language", lang_code(lang_get()))', app)
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


if __name__ == "__main__":
    unittest.main()
