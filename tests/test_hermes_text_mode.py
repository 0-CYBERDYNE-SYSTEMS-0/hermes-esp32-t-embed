from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "platform/hermes_core"


@unittest.skipUnless(shutil.which("c++"), "Host C++ compiler unavailable")
class HermesTextMode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.exe = Path(cls.temp.name) / "hermes-text-mode"
        subprocess.run([
            "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-I", str(CORE / "include"),
            str(ROOT / "tests/hermes_text_mode.cpp"),
            *map(str, sorted((CORE / "src").glob("*.cpp"))),
            "-o", str(cls.exe),
        ], check=True)

    def test_manual_reading_survives_streaming_and_idle(self):
        subprocess.run([str(self.exe), "reading"], check=True)

    def test_wrapping_and_profiles_without_scroll_controls(self):
        subprocess.run([str(self.exe), "wrapping"], check=True)

    def test_silent_replies_keep_microphone_and_text_active(self):
        subprocess.run([str(self.exe), "voice"], check=True)

    def test_prompt_expiry_and_card_paging(self):
        subprocess.run([str(self.exe), "overlays"], check=True)


if __name__ == "__main__":
    unittest.main()
