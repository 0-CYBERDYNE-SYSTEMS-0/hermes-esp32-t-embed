from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / "main/targets/esp32s3"


@unittest.skipUnless(shutil.which("c++"), "Host C++ compiler unavailable")
class TEmbedSpeakerQueue(unittest.TestCase):
    def test_only_whole_samples_are_queued(self):
        harness = r'''
#include <cassert>
#include <cstdint>
#include "t_embed_speaker_queue.h"
int main() {
    // Plenty of room: the whole write goes in.
    assert(t_embed_whole_sample_bytes(1280, 48000) == 1280);
    // A FreeRTOS stream buffer's room is odd when it holds whole samples (it keeps one byte
    // free). Taking all of it would split a sample and turn the rest of the reply into static.
    assert(t_embed_whole_sample_bytes(1280, 639) == 638);
    assert(t_embed_whole_sample_bytes(1280, 1) == 0);
    assert(t_embed_whole_sample_bytes(1280, 0) == 0);
    // The result is always even and never exceeds the request or the room.
    for (size_t want = 0; want < 600; want += 2) {
        for (size_t room = 0; room < 700; ++room) {
            size_t n = t_embed_whole_sample_bytes(want, room);
            assert(n % 2 == 0 && n <= want && n <= room);
        }
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "speaker_queue.cpp"
            src.write_text(harness)
            exe = Path(tmp) / "speaker_queue"
            subprocess.run(
                ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(SOURCES),
                 str(src), "-o", str(exe)],
                check=True,
            )
            subprocess.run([str(exe)], check=True)

    def test_audio_write_uses_the_helper(self):
        source = (SOURCES / "t_embed_hermes_audio.cpp").read_text()
        write = source[source.index("void TEmbedHermesAudio::write("):source.index("void TEmbedHermesAudio::end()")]
        self.assertIn("t_embed_whole_sample_bytes(", write)
        # xStreamBufferCreateStatic needs xBufferSizeBytes + 1 bytes of storage.
        self.assertIn("kSpeakerBufferBytes + 1", source)


if __name__ == "__main__":
    unittest.main()
