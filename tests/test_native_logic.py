import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class NativeLogicTests(unittest.TestCase):
    def test_search_queue_and_cancel_helpers(self):
        compiler = (shutil.which("cc") or shutil.which("gcc") or
                    shutil.which("clang"))
        if compiler is None:
            self.skipTest("A host C compiler is required for native logic tests")

        with tempfile.TemporaryDirectory() as temp:
            executable = Path(temp) / "native_logic_test"
            subprocess.run(
                [
                    compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I", str(ROOT / "source" / "native"),
                    "-I", str(ROOT / "source" / "ui"),
                    "-I", str(ROOT / "source" / "lang"),
                    "-I", str(ROOT / "source" / "platform3ds"),
                    str(ROOT / "tests" / "native_logic_test.c"),
                    str(ROOT / "source" / "platform3ds" / "search_path.c"),
                    str(ROOT / "source" / "lang" / "lang.c"),
                    str(ROOT / "source" / "lang" / "lang_en.c"),
                    str(ROOT / "source" / "lang" / "lang_es.c"),
                    "-o", str(executable),
                    "-lm",
                ],
                check=True,
                cwd=ROOT,
            )
            subprocess.run([str(executable)], check=True, cwd=ROOT)


if __name__ == "__main__":
    unittest.main()
