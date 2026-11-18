"""Tests for the T-Embed RF logging pipeline: wire decode -> capture -> analyze -> report.

Run: python3 -m unittest test_rfkit -v
"""

import json
import struct
import tempfile
import unittest
import zlib
from pathlib import Path

import rfkit
from rfdecode import (FrameError, code_to_dbfs, find_peaks, parse_frame,
                      parse_stats)

NFFT = 256
RATE = 16_000_000
LO = 2437.0


def make_frame(seq, codes, ffts=1):
    hdr = bytearray(28)
    hdr[0:4] = b"SPC1"
    struct.pack_into("<I", hdr, 4, seq)
    struct.pack_into("<Q", hdr, 8, seq * 256)
    struct.pack_into("<I", hdr, 16, 256)
    struct.pack_into("<H", hdr, 20, ffts)
    hdr[22] = 8
    hdr[23] = 3
    struct.pack_into("<H", hdr, 24, 0)
    hdr[26] = 8
    hdr[27] = 2
    body = bytes(hdr) + bytes(codes)
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def make_stats():
    buf = bytearray(40)
    buf[0:4] = b"SPS1"
    struct.pack_into("<H", buf, 4, 100)
    struct.pack_into("<H", buf, 6, 50)
    struct.pack_into("<H", buf, 8, 998)
    struct.pack_into("<H", buf, 10, 3)
    struct.pack_into("<I", buf, 12, 200000)
    struct.pack_into("<I", buf, 16, 100000)
    struct.pack_into("<I", buf, 20, 0)
    struct.pack_into("<I", buf, 24, 0)
    struct.pack_into("<H", buf, 28, 12)
    struct.pack_into("<H", buf, 30, 80)
    struct.pack_into("<I", buf, 32, 500)
    struct.pack_into("<I", buf, 36, zlib.crc32(bytes(buf[:36])) & 0xFFFFFFFF)
    return bytes(buf)


def tone_codes(peak_bin, peak_code=200, floor=10):
    codes = [floor] * NFFT
    codes[peak_bin] = peak_code
    return codes


class FakePort:
    """Byte-queue stand-in for pyserial."""

    def __init__(self, payload: bytes):
        self.buf = bytearray(payload)
        self.written = bytearray()

    def write(self, data):
        self.written.extend(data)

    def read(self, n):
        out = bytes(self.buf[:n])
        del self.buf[:n]
        return out

    def read_exact(self, n):
        out = self.read(n)
        if len(out) != n:
            raise TimeoutError(f"short read {len(out)}/{n}")
        return out

    def readline(self):
        if b"\n" not in self.buf:
            return b""
        i = self.buf.index(b"\n") + 1
        out = bytes(self.buf[:i])
        del self.buf[:i]
        return out

    def unread(self, data):
        self.buf[:0] = data

    def reset_input(self):
        pass

    def close(self):
        pass


class DecodeTests(unittest.TestCase):
    def test_frame_roundtrip_and_crc(self):
        codes = tone_codes(30)
        raw = make_frame(7, codes)
        f = parse_frame(raw)
        self.assertEqual(f.seq, 7)
        self.assertEqual(f.nfft, NFFT)
        self.assertEqual(f.codes, codes)
        self.assertFalse(f.max_detector)
        bad = raw[:-1] + bytes([raw[-1] ^ 0xFF])
        with self.assertRaises(FrameError):
            parse_frame(bad)
        with self.assertRaises(FrameError):
            parse_frame(raw[:-4])

    def test_stats_roundtrip(self):
        s = parse_stats(make_stats())
        self.assertEqual(s["core0_load_pm"], 100)
        self.assertEqual(s["ffts_per_s"], 500)

    def test_dbfs_math(self):
        import math
        # code/2 - 20log10(512) - 20log10(nfft * 0.5)
        expected = 200 / 2 - 20 * math.log10(512) - 20 * math.log10(128)
        self.assertAlmostEqual(code_to_dbfs(200, NFFT), expected, places=6)
        self.assertLess(code_to_dbfs(0, NFFT), -90)

    def test_find_peaks(self):
        info = find_peaks(tone_codes(30), NFFT, LO, RATE, top=5, min_delta=10)
        self.assertEqual(info["floor_code"], 10)
        self.assertEqual(info["peaks"][0]["bin"], 30)
        self.assertAlmostEqual(info["peaks"][0]["freq_mhz"], 2438.875)


class EndToEndTests(unittest.TestCase):
    def _session_payload(self, n_frames=20, peak_bin=30):
        out = bytearray(b"SPEC 256 16000000 12288 2437\n")
        for i in range(n_frames):
            out += make_frame(i, tone_codes(peak_bin))
            if i % 5 == 4:
                out += make_stats()
        out += b"SPECEND 0 0 5120 65536000 100000 0 0 20 0 0 20 0\n"
        return bytes(out)

    def test_capture_analyze_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            session = Path(tmp)
            port = FakePort(self._session_payload())
            summary = rfkit.stream_spec(port, "SPEC 0 1 1 0 6 256 1", 0,
                                        session, LO, RATE, top=5, quiet=True)
            self.assertEqual(summary["frames"], 20)
            self.assertEqual(summary["stats"], 4)
            self.assertEqual(summary["end"]["status"], 0)
            self.assertTrue(summary["started"])

            recs = [json.loads(l) for l in
                    (session / "frames.jsonl").read_text().splitlines()]
            self.assertEqual(len(recs), 20)
            self.assertIn("ts", recs[0])  # wall-clock timestamp per frame
            self.assertEqual(len(recs[0]["codes"]), NFFT)
            self.assertEqual(recs[0]["peaks"][0]["bin"], 30)

            result = rfkit.analyze_frames(recs)
            self.assertEqual(result["frames"], 20)
            self.assertEqual(len(result["signals"]), 1)
            sig = result["signals"][0]
            self.assertAlmostEqual(sig["freq_mhz"], 2438.875)
            self.assertEqual(sig["class"], "STEADY")
            self.assertEqual(sig["duty"], 1.0)

            (session / "report.json").write_text(json.dumps(result))
            (session / "meta.json").write_text(json.dumps({"INFO": "S3SDR 6 burst 16380"}))
            class Args:
                pass
            args = Args()
            args.session = session
            self.assertEqual(rfkit.cmd_report(args), 0)
            html = (session / "report.html").read_text()
            self.assertIn("2438.875000 MHz", html)
            self.assertIn("STEADY", html)
            self.assertNotIn("__ROWS__", html)
            self.assertNotIn("__META__", html)

    def test_bursty_classification(self):
        recs = []
        for i in range(20):
            codes = tone_codes(30)
            if i % 2:  # active half the frames only
                codes[30] = 10
            recs.append({"t_s": i * 0.5, "codes": codes, "floor_code": 10,
                         "nfft": NFFT, "lo_mhz": LO, "rate_hz": RATE})
        result = rfkit.analyze_frames(recs)
        self.assertEqual(result["signals"][0]["class"], "BURSTY")
        self.assertAlmostEqual(result["signals"][0]["duty"], 0.5, places=1)

    def test_sync_and_meta(self):
        payload = b"OK\n" + b"S3SDR 6 burst 16380\nCAPS UARTBAUD SPEC\n" + \
                  b"LIMITS x\nSPECINFO {}\nRANGE 100 6000 1\n" + \
                  b"TRANSPORT USB 115200\nDUAL 1 1\nDC 1\nERR command\n"
        port = FakePort(payload)
        rfkit.sync_lease(port)
        meta = rfkit.collect_meta(port)
        self.assertEqual(meta["INFO"], "S3SDR 6 burst 16380")
        self.assertIn("SPEC", meta["CAPS"])


class SweepTests(unittest.TestCase):
    META_REPLIES = (b"S3SDR 6 burst 16380\nCAPS UARTBAUD SPEC\nLIMITS x\n"
                    b"SPECINFO {}\nRANGE 100 6000 1\nTRANSPORT USB 115200\n"
                    b"DUAL 1 1\nDC 1\nERR command\n")

    def test_sweep_core_end_to_end(self):
        payload = bytearray(self.META_REPLIES)
        for center in (300, 315, 330):
            payload += b"OK\n"  # FREQ
            payload += f"SPEC 256 16000000 12288 {center}\n".encode()
            payload += make_frame(0, tone_codes(30))
            payload += make_frame(1, tone_codes(30))
            payload += b"SPECEND 0 0 512 8192000 20000 0 0 2 0 0 2 0\n"
        payload += b"OK\n"  # RELEASE
        port = FakePort(bytes(payload))
        with tempfile.TemporaryDirectory() as tmp:
            session = Path(tmp)
            report = rfkit.sweep_core(port, session, 300, 330, 15, 250, 6, NFFT, 4)
            self.assertEqual(report["sweep"]["steps"], 3)
            rows = (session / "sweep.jsonl").read_text().splitlines()
            self.assertEqual(len(rows), 3)
            freqs = [s["freq_mhz"] for s in report["signals"]]
            self.assertAlmostEqual(freqs[0], 301.875)
            self.assertAlmostEqual(freqs[1], 316.875)
            self.assertAlmostEqual(freqs[2], 331.875)
            self.assertAlmostEqual(report["signals"][0]["duty"], 0.333)

            class Args:
                pass
            args = Args()
            args.session = session
            self.assertEqual(rfkit.cmd_report(args), 0)
            html = (session / "report.html").read_text()
            self.assertIn("301.875000 MHz", html)
            self.assertIn("331.875000 MHz", html)


def rec(t_s, codes, floor=10):
    """Synthetic analyze_frames input row (frames.jsonl shape)."""
    return {"t_s": t_s, "codes": codes, "floor_code": floor,
            "nfft": NFFT, "lo_mhz": LO, "rate_hz": RATE}


class ThreatTests(unittest.TestCase):
    def test_steady_strong_carrier_is_concern(self):
        recs = [rec(i * 0.5, tone_codes(30)) for i in range(20)]
        result = rfkit.analyze_frames(recs)
        sig = result["signals"][0]
        self.assertEqual(sig["class"], "STEADY")
        self.assertEqual(sig["threat"], "CONCERN")
        self.assertEqual(result["threat"]["level"], "CONCERN")
        self.assertTrue(any("2438.875000 MHz CONCERN: STEADY" in r
                            for r in result["threat"]["reasons"]))

    def test_periodic_burst_train_is_periodic_concern(self):
        # one active frame every 51 at 20ms cadence -> 1.02s rhythm
        recs = []
        for i in range(400):
            codes = tone_codes(30)
            if i % 51:
                codes[30] = 10
            recs.append(rec(i * 0.02, codes))
        result = rfkit.analyze_frames(recs)
        sig = result["signals"][0]
        self.assertTrue(sig["periodic"])
        self.assertAlmostEqual(sig["period_s"], 1.02, delta=0.01)
        self.assertEqual(sig["threat"], "CONCERN")
        self.assertEqual(result["threat"]["level"], "CONCERN")
        self.assertTrue(any("PERIODIC" in r for r in result["threat"]["reasons"]))

    def test_irregular_wifi_bursts_are_ambient(self):
        active = set()
        for start, ln in ((5, 5), (40, 4), (45, 4), (100, 6),
                          (160, 4), (165, 3), (230, 5)):
            active.update(range(start, start + ln))
        recs = []
        for i in range(300):
            codes = tone_codes(30, peak_code=102)  # ~-45 dBFS, weak chatter
            if i not in active:
                codes[30] = 10
            recs.append(rec(i * 0.1, codes))
        result = rfkit.analyze_frames(recs)
        sig = result["signals"][0]
        self.assertEqual(sig["class"], "BURSTY")
        self.assertFalse(sig["periodic"])
        self.assertEqual(sig["threat"], "AMBIENT")
        self.assertEqual(result["threat"]["level"], "AMBIENT")

    def test_bursty_strong_is_watch(self):
        active = {0, 1, 10, 11}
        recs = []
        for i in range(20):
            codes = tone_codes(30, peak_code=160)  # ~-16 dBFS
            if i not in active:
                codes[30] = 10
            recs.append(rec(i * 0.5, codes))
        result = rfkit.analyze_frames(recs)
        sig = result["signals"][0]
        self.assertEqual(sig["class"], "BURSTY")
        self.assertEqual(sig["threat"], "WATCH")
        self.assertEqual(result["threat"]["level"], "WATCH")

    def test_baseline_diff_new_signal_is_concern(self):
        baseline = {"typical_floor_dbfs": -100.0,
                    "signals": [{"freq_mhz": 2438.875, "max_dbfs": -60.0}]}
        active_new = {0, 1, 2, 3, 8, 9, 10, 11, 16, 17}  # irregular, duty 0.5
        recs = []
        for i in range(20):
            codes = tone_codes(30, peak_code=120)  # known: 2438.875 MHz
            codes[100] = 200 if i in active_new else 10  # new: 2443.25 MHz
            recs.append(rec(i * 0.5, codes))
        result = rfkit.analyze_frames(recs, baseline=baseline)
        by_freq = {s["freq_mhz"]: s for s in result["signals"]}
        known, new = by_freq[2438.875], by_freq[2443.25]
        self.assertFalse(known["is_new"])
        self.assertAlmostEqual(known["delta_db"],
                               code_to_dbfs(120, NFFT) - (-60.0), places=1)
        self.assertTrue(new["is_new"])
        self.assertGreaterEqual(new["delta_db"], 10)
        self.assertEqual(new["threat"], "CONCERN")
        self.assertTrue(any("2443.250000 MHz CONCERN: NEW vs baseline" in r
                            for r in result["threat"]["reasons"]))

    def test_cmd_analyze_baseline_flag(self):
        baseline = {"typical_floor_dbfs": -100.0, "signals": []}
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bdir, session = root / "base", root / "cur"
            bdir.mkdir(), session.mkdir()
            (bdir / "report.json").write_text(json.dumps(baseline))
            recs = [rec(i * 0.5, tone_codes(30)) for i in range(20)]
            (session / "frames.jsonl").write_text(
                "\n".join(json.dumps(r) for r in recs) + "\n")
            class Args:
                pass
            args = Args()
            args.session = session
            args.threshold = 10.0
            args.baseline = bdir
            self.assertEqual(rfkit.cmd_analyze(args), 0)
            report = json.loads((session / "report.json").read_text())
            self.assertEqual(report["threat"]["level"], "CONCERN")
            self.assertTrue(report["signals"][0]["is_new"])

    def test_report_html_threat_render(self):
        recs = []
        for i in range(400):
            codes = tone_codes(30)
            if i % 51:
                codes[30] = 10
            recs.append(rec(i * 0.02, codes))
        result = rfkit.analyze_frames(recs)
        with tempfile.TemporaryDirectory() as tmp:
            session = Path(tmp)
            (session / "report.json").write_text(json.dumps(result))
            class Args:
                pass
            args = Args()
            args.session = session
            self.assertEqual(rfkit.cmd_report(args), 0)
            html = (session / "report.html").read_text()
            self.assertIn("VERDICT: CONCERN", html)  # word, not color alone
            self.assertIn("chip chip-CONCERN", html)
            self.assertIn("row-CONCERN", html)
            self.assertIn("PERIODIC 1.02 s", html)  # badge shows the rhythm
            self.assertIn("2196-2806 MHz", html)    # coverage note
            self.assertIn("4G GPS trackers", html)
            self.assertIn("433 MHz", html)

    def test_report_html_clean_banner(self):
        recs = [rec(i * 0.5, [10] * NFFT) for i in range(10)]
        result = rfkit.analyze_frames(recs)
        self.assertEqual(result["threat"], {"level": "AMBIENT", "reasons": []})
        with tempfile.TemporaryDirectory() as tmp:
            session = Path(tmp)
            (session / "report.json").write_text(json.dumps(result))
            class Args:
                pass
            args = Args()
            args.session = session
            self.assertEqual(rfkit.cmd_report(args), 0)
            html = (session / "report.html").read_text()
            self.assertIn("VERDICT: CLEAN", html)


class CardSessionTests(unittest.TestCase):
    """A session pulled off the device card must read without the device."""

    def make_session(self, root: Path) -> Path:
        session = root / "20261118-230405"
        (session / "iq").mkdir(parents=True)
        (session / "net").mkdir(parents=True)
        (session / "session.json").write_text(json.dumps({
            "schema": "tembed-sigint/1", "board": "lilygo-t-embed-s3r8",
            "build": "abc1234", "session": "20261118-230405", "label": "walk",
            "started_unix_ms": 1763500000123, "started_iso": "2025-11-18T21:06:40.123Z",
            "time_source": "host", "lo_mhz": 2437, "rate_code": 6,
            "rate_hz": 16000000, "nfft": NFFT, "lpf_code": 0, "gain_index": 3,
            "battery_mv": 0,
        }))
        frames = []
        for i in range(30):
            codes = [30 + (j * 7) % 5 for j in range(NFFT)]
            codes[200] = 190
            frames.append(json.dumps({
                "seq": i, "t_ms": i * 1000, "t_s": float(i),
                "ts": "2025-11-18T21:06:41.123Z", "lo_mhz": 2437,
                "nfft": NFFT, "rate_hz": RATE, "gain": 3,
                "floor_code": sorted(codes)[max(0, int(0.2 * NFFT) - 1)],
                "codes": codes,
            }))
        (session / "frames.ndjson").write_text("\n".join(frames) + "\n")
        (session / "events.ndjson").write_text(json.dumps({
            "t_ms": 12000, "ts": "2025-11-18T21:06:52.123Z", "khz": 2441000,
            "level": "CONCERN", "reason": 5, "over_floor": 34}) + "\n")
        meta = {
            "global": {"core:datatype": "ci8", "core:sample_rate": RATE,
                       "core:version": "1.2.6", "core:recorder": "esp_sdr",
                       "esp_sdr:rate_code": 6, "esp_sdr:gain_index": 3},
            "captures": [{"core:sample_start": 0, "core:frequency": 2437e6}],
            "annotations": [],
        }
        stem = session / "iq" / "000042-0001-ble"
        stem.with_suffix(".sigmf-meta").write_text(json.dumps(meta))
        stem.with_suffix(".sigmf-data").write_bytes(bytes(2000))  # 1000 ci8 samples
        shb = struct.pack("<IIIHHqI", 0x0A0D0D0A, 28, 0x1A2B3C4D, 1, 0, -1, 28)
        idb = struct.pack("<IIHHII", 1, 20, 127, 0, 1600, 20)
        epb = struct.pack("<IIIIIII", 6, 32 + 8, 0, 0, 0, 8, 8) + b"\x00" * 8 + struct.pack("<I", 40)
        (session / "net" / "wifi-42.pcapng").write_bytes(shb + idb + epb + epb)
        return session

    def test_ndjson_frames_load(self):
        with tempfile.TemporaryDirectory() as tmp:
            session = self.make_session(Path(tmp))
            frames = rfkit.load_frames(session)
            self.assertEqual(len(frames), 30)
            self.assertEqual(frames[0]["nfft"], NFFT)

    def test_pcapng_inventory_counts_packets(self):
        with tempfile.TemporaryDirectory() as tmp:
            session = self.make_session(Path(tmp))
            inv = rfkit.pcapng_inventory(session / "net" / "wifi-42.pcapng")
            self.assertEqual(inv["packets"], 2)
            self.assertEqual(inv["interfaces"], 1)
            self.assertFalse(inv["damaged"])

    def test_pcapng_inventory_flags_truncation(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "cut.pcapng"
            shb = struct.pack("<IIIHHqI", 0x0A0D0D0A, 28, 0x1A2B3C4D, 1, 0, -1, 28)
            path.write_bytes(shb + struct.pack("<II", 6, 400) + b"\x00" * 10)
            inv = rfkit.pcapng_inventory(path)
            self.assertTrue(inv["damaged"])
            self.assertEqual(inv["packets"], 0)

    def test_sigmf_inventory_derives_sample_count(self):
        with tempfile.TemporaryDirectory() as tmp:
            session = self.make_session(Path(tmp))
            recs = rfkit.sigmf_inventory(session / "iq")
            self.assertEqual(len(recs), 1)
            self.assertEqual(recs[0]["datatype"], "ci8")
            self.assertEqual(recs[0]["samples"], 1000)
            self.assertEqual(recs[0]["center_hz"], 2437e6)

    def test_ingest_writes_report_and_inventory(self):
        import contextlib
        import io
        with tempfile.TemporaryDirectory() as tmp:
            session = self.make_session(Path(tmp))

            class Args:
                pass
            args = Args()
            args.session = session
            args.threshold = 10.0
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(rfkit.cmd_ingest(args), 0)
            printed = out.getvalue()
            self.assertIn("build=abc1234", printed)
            self.assertIn("2 packets", printed)
            inventory = json.loads((session / "ingest.json").read_text())
            self.assertEqual(inventory["frames"], 30)
            self.assertEqual(len(inventory["events"]), 1)
            self.assertEqual(len(inventory["recordings"]), 1)
            self.assertEqual(len(inventory["captures"]), 1)
            self.assertTrue((session / "report.json").exists())
            self.assertGreater(inventory["signals"], 0)


if __name__ == "__main__":
    unittest.main()
