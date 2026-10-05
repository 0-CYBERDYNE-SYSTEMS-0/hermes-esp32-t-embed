from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("cc"), "Host C compiler unavailable")
class TEmbedMenu(unittest.TestCase):
    def test_six_menu_choices_fit_and_last_choice_can_be_selected(self):
        source = (ROOT / "main/targets/esp32s3/t_embed_ui.c").read_text()
        renderer = source[source.index("void t_embed_ui_render_menu_overlay("):
                          source.index("void t_embed_ui_invalidate_overlays(")]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
enum { LCD_W = 320, LCD_H = 170 };
enum { COLOR_HEADER = 1, COLOR_BG, COLOR_GRID, COLOR_ACCENT, COLOR_TEXT, COLOR_DIM };
bool ui_ready = true;
uint8_t line[LCD_W * 2];
bool overlay_cache_valid, prompt_overlay_valid;
const char *overlay_title, *overlay_hint;
const char *const *overlay_items;
unsigned overlay_count, overlay_selection;
const char *const labels[] = {"CANCEL", "NEW", "VOLUME", "WI-FI", "HOME", "BACK"};
int first_seen[6] = {-1, -1, -1, -1, -1, -1};
int last_seen[6] = {-1, -1, -1, -1, -1, -1};
void pixel_be(uint8_t *p, uint16_t color) { p[0] = color >> 8; p[1] = color; }
void put_text_scanline(int x, unsigned top, unsigned y, const char *text, uint16_t color) {
    (void)x; (void)color;
    if (y < top || y >= top + 7) return;
    for (unsigned i = 0; i < 6; i++) {
        if (text == labels[i]) {
            if (first_seen[i] < 0) first_seen[i] = y;
            last_seen[i] = y;
        }
    }
}
int draw_row(unsigned y, const uint8_t *pixels) {
    assert(y < LCD_H && pixels == line); return 0;
}
void record_ui_result(int result) { assert(result == 0); }
'''
        checks = r'''
int main(void) {
    t_embed_ui_render_menu_overlay("MENU", labels, 6, 5, "SELECT");
    assert(overlay_count == 6 && overlay_selection == 5);
    for (unsigned i = 0; i < 6; i++) {
        assert(first_seen[i] >= 32 && last_seen[i] < 153);
        if (i) assert(first_seen[i] > last_seen[i - 1]);
    }
    // Existing five-choice menus keep their original text positions.
    overlay_cache_valid = false;
    for (unsigned i = 0; i < 6; i++) first_seen[i] = last_seen[i] = -1;
    t_embed_ui_render_menu_overlay("MENU", labels, 5, 4, "SELECT");
    for (unsigned i = 0; i < 5; i++) assert(first_seen[i] == 39 + (int)i * 22);
    assert(first_seen[5] == -1);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "check.c"
            path.write_text(harness + renderer + checks)
            exe = Path(tmp) / "check"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(path), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
