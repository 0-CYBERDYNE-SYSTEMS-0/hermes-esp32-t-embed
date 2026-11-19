"""Session-log wire format: what the device writes must feed the host toolkit.

The device has no laptop in the field, so the card is the interchange surface.
These tests compile the firmware's own serializers and assert three things:
values are anchored to externally computed ground truth (ISO timestamps, the
host's floor estimator), the JSON parses, and a session written by the device
code path analyzes unchanged in host/rfkit.
"""
from pathlib import Path
import datetime
import json
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host/rfkit"))

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "session_json.h"

static void write_file(const char *dir, const char *name, const char *text, size_t len) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    fwrite(text, 1, len, f);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *dir = argv[1];
    char buf[8192];
    size_t n;

    /* timestamps, printed one per line for the host to check */
    static const int64_t stamps[] = {1763500000123LL, 1763500000000LL, 0,
                                     1700000000999LL, 946684800000LL};
    for (size_t i = 0; i < sizeof(stamps) / sizeof(stamps[0]); i++) {
        sj_iso8601(buf, sizeof(buf), stamps[i]);
        printf("ISO %s\n", buf);
        char stamp[32];
        sj_stamp(stamp, sizeof(stamp), stamps[i], 7);
        printf("STAMP %s\n", stamp);
    }

    /* floor estimator over a deliberately ragged distribution */
    uint8_t codes[256];
    for (unsigned i = 0; i < 256; i++) codes[i] = (uint8_t)(i < 60 ? 12 : 40 + (i % 17));
    codes[200] = 200;
    printf("FLOOR %u\n", sj_floor_code(codes, 256));

    sj_manifest_t m = {
        .build = "testsha", .session = "20261118-230405", .label = "walk",
        .time_source = "host", .board = "lilygo-t-embed-s3r8",
        .started_unix_ms = 1763500000123LL, .lo_mhz = 2437, .rate_code = 6,
        .rate_hz = 16000000, .nfft = 256, .lpf_code = 0, .gain_index = 3,
        .battery_mv = 4012,
    };
    n = sj_manifest_json(buf, sizeof(buf), &m);
    write_file(dir, "session.json", buf, n);

    /* 40 frames, carrier steady at bin 200, at 1 frame/s */
    char iso[32];
    sj_iso8601(iso, sizeof(iso), m.started_unix_ms);
    uint8_t frame[256];
    for (unsigned f = 0; f < 40; f++) {
        for (unsigned i = 0; i < 256; i++) frame[i] = (uint8_t)(30 + (i * 7 % 5));
        frame[200] = 190;
        frame[199] = 120;
        frame[201] = 118;
        sj_frame_t fj = {
            .seq = f, .t_ms = (int64_t)f * 1000, .iso = iso, .lo_mhz = 2437,
            .nfft = 256, .rate_hz = 16000000, .gain_index = 3,
            .floor_code = sj_floor_code(frame, 256),
        };
        n = sj_frame_ndjson(buf, sizeof(buf), &fj, frame);
        if (!n) return 3;
        char path[512];
        snprintf(path, sizeof(path), "%s/frames.jsonl", dir);
        FILE *out = fopen(path, f ? "ab" : "wb");
        if (!out) return 4;
        fwrite(buf, 1, n, out);
        fclose(out);
    }

    n = sj_event_ndjson(buf, sizeof(buf), 12500, iso, 2441000, "CONCERN", 5, 34);
    write_file(dir, "events.ndjson", buf, n);

    /* IQ8 capture: 16380 samples -> 32760 bytes of ci8 */
    size_t iq_bytes = 32760;
    uint8_t *iq = (uint8_t *)malloc(iq_bytes);
    for (size_t i = 0; i < iq_bytes; i++) iq[i] = (uint8_t)(i * 37 + 11);
    write_file(dir, "iq-0001-test.sigmf-data", (const char *)iq, iq_bytes);
    free(iq);
    sj_sigmf_t s = {
        .datatype = "ci8", .hw = "lilygo-t-embed-s3r8",
        .description = "one-press evidence capture", .recording_id = "0001-test",
        .iso = iso, .center_hz = 2437000000.0, .sample_rate_hz = 16000000.0,
        .num_samples = 16380, .rate_code = 6, .gain_index = 3,
    };
    n = sj_sigmf_meta(buf, sizeof(buf), &s);
    write_file(dir, "iq-0001-test.sigmf-meta", buf, n);

    /* truncation must fail loudly instead of emitting half a line */
    char tiny[64];
    printf("TRUNC %u\n", (unsigned)sj_frame_ndjson(tiny, sizeof(tiny), &(sj_frame_t){
        .seq = 0, .t_ms = 0, .iso = NULL, .lo_mhz = 2437, .nfft = 256,
        .rate_hz = 16000000, .gain_index = 3, .floor_code = 30}, frame));
    return 0;
}
'''


@unittest.skipUnless(shutil.which("cc"), "Host C compiler unavailable")
class SessionJson(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name) / "session"
        cls.dir.mkdir()
        harness = Path(cls.tmp.name) / "harness.c"
        harness.write_text(HARNESS)
        cls.binary = Path(cls.tmp.name) / "harness"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        f"-I{ROOT / 'main/common'}", "-o", str(cls.binary), str(harness)],
                       check=True)
        cls.out = subprocess.run([str(cls.binary), str(cls.dir)], check=True,
                                 capture_output=True, text=True).stdout

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_iso8601_matches_an_independent_calendar(self):
        got = [line[4:] for line in self.out.splitlines() if line.startswith("ISO ")]
        stamps = [1763500000123, 1763500000000, 0, 1700000000999, 946684800000]
        expected = []
        for ms in stamps:
            if ms <= 0:
                expected.append("")
                continue
            dt = datetime.datetime.fromtimestamp(ms / 1000, datetime.timezone.utc)
            expected.append(dt.strftime("%Y-%m-%dT%H:%M:%S.") + f"{ms % 1000:03d}Z")
        self.assertEqual(got, expected)

    def test_session_stamp_and_unsynced_fallback(self):
        got = [line[6:] for line in self.out.splitlines() if line.startswith("STAMP ")]
        expected = datetime.datetime.fromtimestamp(
            1763500000123 / 1000, datetime.timezone.utc).strftime("%Y%m%d-%H%M%S")
        self.assertEqual(got[0], expected)
        self.assertEqual(got[2], "unsynced-007")

    def test_floor_matches_host_estimator(self):
        host = sorted([(12 if i < 60 else 40 + (i % 17)) for i in range(256)])
        host[200] = 200
        host = sorted(host)
        expected = host[max(0, int(0.2 * len(host)) - 1)]
        got = [line for line in self.out.splitlines() if line.startswith("FLOOR ")][0]
        self.assertEqual(int(got.split()[1]), expected)

    def test_manifest_is_json_with_provenance(self):
        manifest = json.loads((self.dir / "session.json").read_text())
        self.assertEqual(manifest["schema"], "tembed-sigint/1")
        self.assertEqual(manifest["build"], "testsha")
        self.assertEqual(manifest["time_source"], "host")
        self.assertEqual(manifest["lo_mhz"], 2437)
        self.assertEqual(manifest["battery_mv"], 4012)

    def test_frame_lines_parse_and_round_trip_codes(self):
        lines = (self.dir / "frames.jsonl").read_text().splitlines()
        self.assertEqual(len(lines), 40)
        first = json.loads(lines[0])
        self.assertEqual(first["nfft"], 256)
        self.assertEqual(len(first["codes"]), 256)
        self.assertEqual(first["codes"][200], 190)
        self.assertEqual(first["t_s"], 0.0)
        self.assertEqual(json.loads(lines[1])["t_s"], 1.0)

    def test_events_parse(self):
        event = json.loads((self.dir / "events.ndjson").read_text())
        self.assertEqual(event["level"], "CONCERN")
        self.assertEqual(event["khz"], 2441000)

    def test_truncated_frame_is_refused(self):
        line = [l for l in self.out.splitlines() if l.startswith("TRUNC ")][0]
        self.assertEqual(line, "TRUNC 0")

    def test_rfkit_analyzes_a_device_written_session_unchanged(self):
        import rfkit
        frames = rfkit.load_frames(self.dir)
        report = rfkit.analyze_frames(frames)
        self.assertEqual(report["frames"], 40)
        self.assertEqual(report["nfft"], 256)
        self.assertEqual(report["lo_mhz"], 2437)
        self.assertTrue(report["signals"], "the steady carrier must be reported")
        carrier = report["signals"][0]
        self.assertLessEqual(abs(carrier["bin"] - 200), 3)
        self.assertEqual(carrier["class"], "STEADY")
        self.assertGreaterEqual(carrier["max_code"], 190)

    def test_sigmf_metadata_is_structurally_valid(self):
        meta = json.loads((self.dir / "iq-0001-test.sigmf-meta").read_text())
        global_ = meta["global"]
        self.assertEqual(global_["core:datatype"], "ci8")
        self.assertEqual(global_["core:sample_rate"], 16000000)
        self.assertEqual(global_["core:version"], "1.2.6")
        self.assertEqual(meta["captures"][0]["core:frequency"], 2437000000)
        self.assertEqual(meta["captures"][0]["core:sample_start"], 0)
        data = (self.dir / "iq-0001-test.sigmf-data").stat().st_size
        self.assertEqual(data, 16380 * 2)


if __name__ == "__main__":
    unittest.main()
