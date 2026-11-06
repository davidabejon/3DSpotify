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
        self.assertIn("action == HOME_ACTION_REPEAT", app)
        ask = app[app.index("static bool ask_search_query("):
                  app.index("static str_id search_error_advice(")]
        self.assertIn("if (confirmed)", ask)

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
        # 7.4: four rows per screen, L/R jump as many.
        self.assertIn("#define RESULTS_PER_PAGE 4", logic)
        results = (ui_dir / "view_results.c").read_text(encoding="utf-8")
        self.assertIn("move_focus(RESULTS_PER_PAGE)", results)
        self.assertIn("KEY_L", results)
        # Rows are cut in pixels, never the duration (5.3, 3.3).
        self.assertIn("parse_fitted(&row_title[i]", results)
        self.assertIn("ICON_NOTE", results)
        # On-demand render and view switching.
        frame = app[app.index("static void frame(void)"):
                    app.index("static void show_progress(")]
        self.assertIn("view_results_dirty()", frame)
        self.assertIn("view_results_draw_bottom()", frame)
        self.assertIn("view_results_init()", app)
        self.assertLess(app.index("view_results_exit();", app.index("done:")),
                        app.index("ui_exit();", app.index("done:")))
        # Searching is a state of the view; B and START still cancel.
        search = app[app.index("static search_result search_tracks("):
                     app.index("static bool ask_search_query(")]
        self.assertIn("view_results_searching(search.query)", search)
        self.assertIn("view_results_cancelling()", search)
        self.assertIn("down & (KEY_B | KEY_START)", search)
        # The selection survives playback (F3) and errors can be retried.
        self.assertIn("*selected = queue_index;", app)
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
        # Cancelled and joined before playback and on leaving the results.
        self.assertIn("threadJoin(worker", thumbs)
        flow = app[app.index("static flow_step results_flow("):
                   app.index("static void show_pair_screen(")]
        self.assertLess(flow.index("thumbs_stop();"),
                        flow.index("play_flow(session, &selected)"))
        search = flow[flow.index("static bool search_flow("):]
        self.assertLess(search.index("results_flow(session, query, result)"),
                        search.index("thumbs_stop();"))
        self.assertIn("thumbs_init();", app)
        self.assertLess(app.index("thumbs_exit();", app.index("done:")),
                        app.index("ui_exit();", app.index("done:")))
        # The view: uploads on update, a fixed-size slot either way.
        results = (ui_dir / "view_results.c").read_text(encoding="utf-8")
        self.assertIn("thumbs_poll()", results)
        self.assertIn("thumbs_image(index, &image)", results)
        self.assertIn("thumbs_set_window(first)", results)

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
        play = app[app.index("static flow_step play_flow("):
                   app.index("static flow_step results_flow(")]
        self.assertIn("playback(session, playback_queue", play)
        self.assertIn("*selected = queue_index;", play)
        self.assertNotIn("hidKeysDown", play)
        # Dragging (6.9) and the scroll indicator (5.11).
        results = (ui_dir / "view_results.c").read_text(encoding="utf-8")
        self.assertIn("list_drag_begin(&drag", results)
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
        self.assertIn("search.cancel_requested", app)
        self.assertIn("down & (KEY_B | KEY_START)", app)
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
        playback = app[app.index("static playback_result playback("):
                          app.index("static void frame(void)", app.index(
                              "static playback_result playback("
                          ))]
        self.assertIn("playback_queue_step(*track_index, queue_count", playback)
        logic = (build.ROOT / "ui" / "player_logic.h").read_text(
            encoding="utf-8"
        )
        self.assertIn("next >= 0 && next < count", logic)
        cleanup = playback[playback.index("cleanup:"):]
        cleanup_steps = [
            "download_stop_join(&job);",
            "stb_vorbis_close(decoder);",
            "ndspChnWaveBufClear(0);",
            "track_stream_free(&job.stream);",
        ]
        offsets = [cleanup.index(step) for step in cleanup_steps]
        self.assertEqual(offsets, sorted(offsets))

    def test_player_metrics_share_one_line_and_pause_holds_timestamp(self):
        source = (build.ROOT / "main.c").read_text(encoding="utf-8")
        playback = source[
            source.index("static playback_result playback("):
            source.index("static void frame(void)", source.index(
                "static playback_result playback("
            ))
        ]
        line_update = source[
            source.index("static void player_update_line("):
            source.index("static void download_worker(")
        ]
        self.assertIn("tr(STR_PLAYER_STATUS_FMT)", playback)
        lang_dir = build.ROOT / "lang"
        self.assertIn('"%02u:%02u/%02d:%02d Desc:%u%% Vol:%u%%"',
                      (lang_dir / "lang_es.c").read_text(encoding="utf-8"))
        self.assertIn('"%02u:%02u/%02d:%02d DL:%u%% Vol:%u%%"',
                      (lang_dir / "lang_en.c").read_text(encoding="utf-8"))
        self.assertIn("if (!paused)", playback)
        self.assertIn("displayed_played_frames = played_frames", playback)
        self.assertIn("played_frames = displayed_played_frames", playback)
        self.assertNotIn("player_update_status", playback)
        self.assertNotIn("last_progress", playback)
        self.assertIn("player_update_line(status)", playback)
        self.assertIn('"\\x1b[%d;1H%-*.*s"', line_update)
        self.assertNotIn("consoleGetDefault", line_update)

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
