"""The launcher's machine interface: a rig must select an app without a knob."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "main/targets/esp32s3/t_embed_launcher.c"


@unittest.skipUnless(shutil.which("cc"), "Host C compiler unavailable")
class LauncherConsole(unittest.TestCase):
    def compile(self, checks: str) -> str:
        source = SOURCE.read_text()
        slice_ = source[source.index("static int console_parse("):
                        source.index("static int console_poll(")]
        harness = r'''
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#ifndef HUNT_BUILD_TAG
#define HUNT_BUILD_TAG "test"
#endif
static char reply[64];
static void console_reply(const char *text) { snprintf(reply, sizeof reply, "%s", text); }
static const char *last_reply(void) { return reply; }
''' + slice_ + checks
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "harness.c"
            path.write_text(harness)
            binary = Path(tmp) / "harness"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-o", str(binary), str(path)], check=True)
            return subprocess.run([str(binary)], check=True,
                                  capture_output=True, text=True).stdout

    def test_ping_answers(self):
        out = self.compile(r'''
int main(void) {
    if (console_parse("PING") != -1) return 1;
    if (strcmp(last_reply(), "PONG\n")) return 2;
    return 0;
}
''')
        self.assertEqual(out, "")

    def test_info_reports_build_tag(self):
        self.compile(r'''
int main(void) {
    console_parse("INFO");
    if (strncmp(last_reply(), "TEMBED launcher ", 16)) return 1;
    return 0;
}
''')

    def test_app_selects_by_name_and_index(self):
        self.compile(r'''
int main(void) {
    if (console_parse("APP SDR") != 0) return 1;
    if (strcmp(last_reply(), "APP SDR\n")) return 2;
    if (console_parse("APP mic") != 1) return 3;
    if (console_parse("APP 2") != 2) return 4;
    if (strcmp(last_reply(), "APP HERMES\n")) return 5;
    if (console_parse("APP 0") != 0) return 6;
    return 0;
}
''')

    def test_unknown_app_is_rejected_not_launched(self):
        self.compile(r'''
int main(void) {
    if (console_parse("APP RADIO") != -1) return 1;
    if (strcmp(last_reply(), "ERR app\n")) return 2;
    if (console_parse("SELECT 2") != -1) return 3;
    if (strcmp(last_reply(), "ERR command\n")) return 4;
    return 0;
}
''')

    def test_launch_reply_precedes_handover(self):
        """The reply must exist as text the host can read after the app takes the port."""
        source = SOURCE.read_text()
        handover = source.index("static void console_hand_over(")
        launch = source.index("console_hand_over();", handover)
        app = source.index("t_embed_sdr_run()", launch)
        self.assertLess(launch, app)


if __name__ == "__main__":
    unittest.main()
