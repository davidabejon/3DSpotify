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

    def test_native_target_excludes_proxy_sources_and_missing_image_unit(self):
        source_dirs, _, mbedtls, extra_sources = build.APPS["spotify3ds"]
        source_names = {
            path.name
            for directory in source_dirs
            for path in (build.ROOT / directory).glob("*.c")
        }
        source_names.update(Path(path).name for path in extra_sources)
        self.assertNotIn("fetch.c", source_names)
        self.assertNotIn("parse.c", source_names)
        self.assertNotIn("stb_image_impl.c", source_names)
        self.assertIn("native", source_dirs)
        self.assertNotIn("probe_play", source_dirs)
        self.assertEqual(mbedtls, "release")
        self.assertIn("deps/stb_vorbis.c", extra_sources)

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
