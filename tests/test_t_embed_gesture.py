from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("cc"), "Host C compiler unavailable")
class TEmbedGesture(unittest.TestCase):
    def compile_run(self, body):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "check.c"
            path.write_text(body)
            exe = Path(tmp) / "check"
            subprocess.run([
                "cc", "-std=c11", "-I" + str(ROOT / "main/targets/esp32s3"),
                str(path), str(ROOT / "main/targets/esp32s3/t_embed_gesture.c"),
                "-o", str(exe),
            ], check=True)
            subprocess.run([str(exe)], check=True)

    def test_press_is_debounced_contextual_and_clicks_only_on_release(self):
        self.compile_run(r'''
#include <assert.h>
#include "t_embed_gesture.h"
int main(void) {
    t_embed_gesture_state_t state = {0};
    t_embed_gesture_event_t event;
    t_embed_gesture_update(&state, false, 0, 100000, 30, 300, 2, &event);
    t_embed_gesture_update(&state, true, 0, 200000, 30, 300, 2, &event);
    assert(!event.press_started && !event.short_click);
    t_embed_gesture_update(&state, true, 0, 229999, 30, 300, 7, &event);
    assert(!event.press_started && !event.short_click);
    t_embed_gesture_update(&state, true, 1, 230000, 30, 300, 7, &event);
    assert(event.press_started && event.detents == 1);
    assert(event.press_context == 2 && event.pressed_at_us == 200000);
    t_embed_gesture_update(&state, false, 0, 300000, 30, 300, 7, &event);
    assert(!event.released && !event.short_click);
    t_embed_gesture_update(&state, false, 0, 330000, 30, 300, 7, &event);
    assert(event.released && event.short_click && event.press_context == 2);
    return 0;
}
''')

    def test_hold_suppresses_short_click_and_boot_hold_requires_release(self):
        self.compile_run(r'''
#include <assert.h>
#include "t_embed_gesture.h"
int main(void) {
    t_embed_gesture_state_t state;
    t_embed_gesture_event_t event;
    t_embed_gesture_init(&state, true, 0);
    t_embed_gesture_update(&state, false, 0, 40000, 30, 300, 5, &event);
    assert(!event.released);
    t_embed_gesture_update(&state, false, 0, 70000, 30, 300, 5, &event);
    assert(event.released && !event.short_click);
    t_embed_gesture_update(&state, false, 0, 80000, 30, 300, 5, &event);
    t_embed_gesture_update(&state, true, 0, 100000, 30, 300, 5, &event);
    t_embed_gesture_update(&state, true, 0, 130000, 30, 300, 5, &event);
    assert(event.press_started);
    t_embed_gesture_update(&state, true, 0, 400000, 30, 300, 5, &event);
    assert(event.hold && event.pressed_at_us == 100000);
    t_embed_gesture_update(&state, false, 0, 500000, 30, 300, 5, &event);
    t_embed_gesture_update(&state, false, 0, 530000, 30, 300, 5, &event);
    assert(event.released && !event.short_click);
    return 0;
}
''')

    def test_rotation_moves_in_both_directions_and_wraps(self):
        self.compile_run(r'''
#include <assert.h>
#include "t_embed_gesture.h"
int main(void) {
    assert(t_embed_menu_move(0, -1, 3) == 2);
    assert(t_embed_menu_move(2, 1, 3) == 0);
    assert(t_embed_menu_move(1, 4, 3) == 2);
    assert(t_embed_menu_move(1, -4, 3) == 0);
    assert(t_embed_menu_move(7, 1, 0) == 0);
    return 0;
}
''')

    def test_release_debounce_uses_physical_release_time_for_hold_arbitration(self):
        self.compile_run(r'''
#include <assert.h>
#include "t_embed_gesture.h"
int main(void) {
    t_embed_gesture_state_t state = {0};
    t_embed_gesture_event_t event;
    t_embed_gesture_update(&state, false, 0, 100000, 30, 300, 2, &event);
    t_embed_gesture_update(&state, true, 0, 200000, 30, 300, 2, &event);
    t_embed_gesture_update(&state, true, 0, 230000, 30, 300, 2, &event);
    t_embed_gesture_update(&state, false, 0, 499999, 30, 300, 2, &event);
    assert(!event.hold && !event.released);
    t_embed_gesture_update(&state, false, 0, 529999, 30, 300, 2, &event);
    assert(event.released && event.short_click);

    t_embed_gesture_init(&state, false, 600000);
    t_embed_gesture_update(&state, true, 0, 700000, 30, 300, 2, &event);
    t_embed_gesture_update(&state, true, 0, 730000, 30, 300, 2, &event);
    t_embed_gesture_update(&state, false, 0, 1000000, 30, 300, 2, &event);
    assert(!event.hold && !event.released);
    t_embed_gesture_update(&state, false, 0, 1030000, 30, 300, 2, &event);
    assert(event.released && !event.short_click);
    return 0;
}
''')
