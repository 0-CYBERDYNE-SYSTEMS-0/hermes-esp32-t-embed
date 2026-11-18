#!/usr/bin/env python3
"""rfkit — logging + spike analysis for the ESP-SDR firmware on a LILYGO T-Embed.

Talks the documented newline-ASCII protocol over the USB serial port
(docs/spectrum.md of 0-CYBERDYNE-SYSTEMS-0/hermes-esp32-t-embed). The ESP-SDR
app must be running on the device (select it from the T-Embed launcher).

Subcommands:
  info     device/capability snapshot (INFO, CAPS, LIMITS?, SPECINFO?, ...)
  capture  continuous spectrum capture -> session dir with frames.jsonl,
           live top-spike printout, stats.jsonl
  sweep    step FREQ across a band plan with short captures -> occupancy log
  analyze  aggregate a session -> report.json (top spike frequencies,
           duty cycle, STEADY/BURSTY/SPORADIC classification, PERIODIC
           rhythm detection, threat verdict; --baseline diffs a prior
           session to mark KNOWN/NEW signals)
  report   render a self-contained report.html (pure HTML/CSS bars)

Examples:
  python3 rfkit.py info
  python3 rfkit.py capture --seconds 60 --center 2437
  python3 rfkit.py sweep --start 300 --stop 3000 --dwell-ms 300
  python3 rfkit.py analyze logs/session_20261025-120000
  python3 rfkit.py report logs/session_20261025-120000
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rfdecode import (  # noqa: E402
    CRC_BYTES, FRAME_MAGIC, HEADER_BYTES, STATS_MAGIC, END_PREFIX,
    FrameError, SpectrumFrame, code_to_dbfs, bin_frequency_mhz,
    find_peaks, parse_frame, parse_stats,
)

DEFAULT_PORT = "/dev/cu.usbmodem101"
LOG_ROOT = Path(__file__).resolve().parent / "logs"


# ---------------------------------------------------------------- serial core

class SerialPort:
    """Thin wrapper so tests can substitute a fake."""

    def __init__(self, port: str, baud: int = 115200):
        import serial  # pyserial
        self._s = serial.Serial(port=None, baudrate=baud, timeout=5, write_timeout=5)
        self._s.dtr = False
        self._s.rts = False
        self._s.port = port
        self._s.open()
        # Opening the native-USB port can restart the chip; drain boot text
        # before any protocol traffic (same approach as tools/probe_t_embed.py).
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            self._s.read(4096)
        self._s.reset_input_buffer()
        self._push = bytearray()

    def unread(self, data: bytes) -> None:
        self._push[:0] = data

    def readline(self) -> bytes:
        if b"\n" in self._push:
            i = self._push.index(b"\n") + 1
            out = bytes(self._push[:i])
            del self._push[:i]
            return out
        return self._s.readline()

    def write(self, data: bytes) -> None:
        self._s.write(data)
        self._s.flush()

    def read(self, n: int) -> bytes:
        return self._s.read(n)

    def read_exact(self, n: int) -> bytes:
        out = bytearray()
        while len(out) < n:
            chunk = self._s.read(n - len(out))
            if not chunk:
                raise TimeoutError(f"short read {len(out)}/{n}")
            out.extend(chunk)
        return bytes(out)

    def reset_input(self) -> None:
        self._s.reset_input_buffer()

    def close(self) -> None:
        self._s.close()


def sync_lease(port) -> None:
    """Take the single-client serial lease (or confirm we hold it).

    Sends RELEASE only when the line is quiet, so a burst of boot text can't
    queue up a pile of stale OK replies, then drains whatever is stale.
    """
    deadline = time.monotonic() + 12
    port.write(b"RELEASE\n")
    while time.monotonic() < deadline:
        try:
            line = port.readline()
        except TimeoutError:
            line = b""
        if not line:
            port.write(b"RELEASE\n")
            continue
        if line.strip() == b"OK":
            break
    else:
        raise TimeoutError("could not synchronize the serial lease (device in launcher?)")
    # clear stale OKs from duplicate retries; push anything else back
    for _ in range(4):
        try:
            line = port.readline()
        except TimeoutError:
            break
        if not line:
            break
        if line.strip() == b"OK":
            continue
        port.unread(line)
        break


def command(port, text: str, timeout: float = 5.0) -> str:
    port.write((text + "\n").encode("ascii"))
    line = port.readline()
    return line.decode("ascii", "replace").strip()


def open_session(port_name: str, baud: int = 115200):
    port = SerialPort(port_name, baud)
    sync_lease(port)
    return port


# ------------------------------------------------------------------ info cmd

QUERY_ORDER = ["INFO", "CAPS", "LIMITS?", "SPECINFO?", "RANGE?", "TRANSPORT?",
               "DUAL?", "DC?", "UI?"]


def collect_meta(port) -> dict:
    meta = {"captured_at": dt.datetime.now().astimezone().isoformat(timespec="seconds")}
    for q in QUERY_ORDER:
        try:
            reply = command(port, q)
        except Exception as exc:  # noqa: BLE001
            reply = f"<error: {exc}>"
        meta[q.rstrip("?")] = reply
        if q == "SPECINFO?" and reply.startswith("SPECINFO "):
            try:
                meta["specinfo"] = json.loads(reply[9:])
            except json.JSONDecodeError:
                pass
    return meta


def cmd_info(args) -> int:
    port = open_session(args.port, args.baud)
    try:
        meta = collect_meta(port)
        command(port, "RELEASE")
    finally:
        port.close()
    print(json.dumps(meta, indent=2))
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(meta, indent=2) + "\n")
    return 0


# --------------------------------------------------------------- capture cmd

def parse_spec_end(line: str) -> dict:
    fields = line.split()
    assert fields[0] == "SPECEND" and len(fields) == 13, line
    keys = ["status", "detail", "units", "pairs", "elapsed_us", "late_max",
            "work_max_cycles", "frames", "drops", "abandoned", "ffts",
            "stopped_by_host"]
    return dict(zip(keys, (int(v) for v in fields[1:])))


def stream_spec(port, spec_cmd: str, duration_s: float, session_dir: Path,
                lo_mhz: float, sample_rate_hz: float, top: int,
                quiet: bool = False):
    """Run one SPEC capture; write frames.jsonl/stats.jsonl; return summary.

    Streams until duration_s elapses (infinite SPEC runs get a stop byte),
    then returns after the SPECEND record. Raises on protocol errors.
    """
    reply = command(port, spec_cmd)
    fields = reply.split()
    if len(fields) != 5 or fields[0] != "SPEC":
        raise RuntimeError(f"unexpected SPEC start: {reply!r}")
    nfft = int(fields[1])
    sample_rate_hz = int(fields[2])
    lo_mhz = float(fields[4])

    frames_path = session_dir / "frames.jsonl"
    stats_path = session_dir / "stats.jsonl"
    frames_f = frames_path.open("a")
    stats_f = stats_path.open("a")
    started_iso = dt.datetime.now().astimezone().isoformat(timespec="seconds")
    t0 = time.monotonic()
    n_frames = n_stats = 0
    end_record = None
    last_print = 0.0
    try:
        while True:
            magic = port.read_exact(4)
            if magic == END_PREFIX:
                line = (magic + port.readline()).decode("ascii", "replace").strip()
                end_record = parse_spec_end(line)
                break
            if magic == STATS_MAGIC:
                rec = parse_stats(magic + port.read_exact(36))
                rec["t_s"] = round(time.monotonic() - t0, 3)
                stats_f.write(json.dumps(rec) + "\n")
                n_stats += 1
                continue
            if magic != FRAME_MAGIC:
                # resync is the firmware's documented recovery; here we fail loud
                raise RuntimeError(f"lost framing: {magic!r}")
            raw = magic + port.read_exact(HEADER_BYTES - 4 + nfft + CRC_BYTES)
            frame = parse_frame(raw)
            if frame.empty:
                continue
            peak_info = find_peaks(frame.codes, nfft, lo_mhz, sample_rate_hz, top=top)
            rec = {
                "seq": frame.seq,
                "ts": dt.datetime.now().astimezone().isoformat(timespec="milliseconds"),
                "t_s": round(time.monotonic() - t0, 3),
                "first_sample": frame.first_sample,
                "ffts": frame.ffts,
                "flags": frame.flags,
                "gain": frame.gain,
                "drops": frame.drops,
                "lo_mhz": lo_mhz,
                "nfft": nfft,
                "rate_hz": sample_rate_hz,
                **peak_info,
                "codes": frame.codes,
            }
            frames_f.write(json.dumps(rec) + "\n")
            frames_f.flush()
            n_frames += 1
            now = time.monotonic()
            if not quiet and now - last_print >= 1.0:
                last_print = now
                tops = "  ".join(
                    f"{p['freq_mhz']:.3f}MHz {p['dbfs']:+.1f}dB" for p in peak_info["peaks"][:5])
                print(f"[{rec['t_s']:7.1f}s] frames={n_frames} floor={peak_info['floor_dbfs']:+.1f}dB  {tops}",
                      flush=True)
            if duration_s and time.monotonic() - t0 >= duration_s:
                port.write(b"\n")  # stop byte
                duration_s = 0  # drain until SPECEND
    finally:
        frames_f.close()
        stats_f.close()
    return {"frames": n_frames, "stats": n_stats, "nfft": nfft,
            "sample_rate_hz": sample_rate_hz, "lo_mhz": lo_mhz,
            "end": end_record, "duration_s": round(time.monotonic() - t0, 2),
            "started": started_iso,
            "finished": dt.datetime.now().astimezone().isoformat(timespec="seconds")}


def cmd_capture(args) -> int:
    session_dir = make_session_dir(args.out, "capture")
    port = open_session(args.port, args.baud)
    try:
        meta = collect_meta(port)
        (session_dir / "meta.json").write_text(json.dumps(meta, indent=2) + "\n")
        if args.center:
            reply = command(port, f"FREQ {int(round(args.center))}")
            if reply != "OK":
                raise RuntimeError(f"FREQ rejected: {reply!r}")
        det = 1 if args.max_hold else 0
        spec = (f"SPEC 0 {args.stride} {args.units} {det} {args.rate} {args.nfft}"
                + (" 1" if args.stats else ""))
        print(f"capturing: {spec}  ({args.seconds}s -> {session_dir})")
        summary = stream_spec(port, spec, args.seconds, session_dir,
                              args.center or 2437.0, 16_000_000, args.top,
                              quiet=args.quiet)
        command(port, "RELEASE")
    finally:
        port.close()
    (session_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    print(f"session: {session_dir}")
    return 0


# ----------------------------------------------------------------- sweep cmd

def sweep_core(port, session_dir: Path, start: int, stop: int, step: int,
               dwell_ms: int, rate: int, nfft: int, top: int) -> dict:
    meta = collect_meta(port)
    (session_dir / "meta.json").write_text(json.dumps(meta, indent=2) + "\n")
    centers = list(range(start, stop + 1, step))
    rows_path = session_dir / "sweep.jsonl"
    rows_f = rows_path.open("a")
    try:
        for i, center in enumerate(centers):
            reply = command(port, f"FREQ {center}")
            if reply != "OK":
                print(f"  !! FREQ {center}: {reply}")
                continue
            spec = f"SPEC {dwell_ms} 1 1 1 {rate} {nfft} 0"
            summary = stream_spec(port, spec, 0, session_dir, float(center),
                                  16_000_000, top, quiet=True)
            frame_recs = [json.loads(l) for l in
                          (session_dir / "frames.jsonl").read_text().splitlines()]
            (session_dir / "frames.jsonl").unlink()
            merged: dict[int, dict] = {}
            for rec in frame_recs:
                for p in rec["peaks"]:
                    cur = merged.get(p["bin"])
                    if cur is None or p["code"] > cur["code"]:
                        merged[p["bin"]] = p
            row = {
                "center_mhz": center,
                "frames": summary["frames"],
                "nfft": summary["nfft"],
                "rate_hz": summary["sample_rate_hz"],
                "peaks": sorted(merged.values(), key=lambda p: -p["code"])[:top],
            }
            rows_f.write(json.dumps(row) + "\n")
            rows_f.flush()
            tops = "  ".join(f"{p['freq_mhz']:.3f} {p['dbfs']:+.1f}dB"
                             for p in row["peaks"][:3])
            print(f"[{i + 1}/{len(centers)}] {center}MHz  {tops}", flush=True)
    finally:
        rows_f.close()
    command(port, "RELEASE")
    # aggregate sweep rows into the same report.json shape analyze produces
    rows = [json.loads(l) for l in rows_path.read_text().splitlines() if l.strip()]
    steps = len(rows) or 1
    tracked: dict[float, dict] = {}
    for i, row in enumerate(rows):
        for p in row["peaks"]:
            key = round(p["freq_mhz"], 3)
            t = tracked.setdefault(key, {
                "freq_mhz": key, "bin": p["bin"], "bin_span": [p["bin"], p["bin"]],
                "max_code": 0, "max_dbfs": -999.0, "mean_dbfs": -999.0,
                "duty": 0.0, "score": 0.0, "seen": 0, "timeline": [None] * 60,
                "class": "SPORADIC"})
            t["seen"] += 1
            if p["code"] > t["max_code"]:
                t["max_code"] = p["code"]
                t["max_dbfs"] = p["dbfs"]
            bucket = int(i / max(1, steps - 1) * 59)
            prev = t["timeline"][bucket]
            t["timeline"][bucket] = p["code"] if prev is None else max(prev, p["code"])
    for t in tracked.values():
        t["duty"] = round(t["seen"] / steps, 3)
        t["class"] = ("STEADY" if t["duty"] >= 0.8 else
                      "BURSTY" if t["duty"] >= 0.1 else "SPORADIC")
        t["mean_dbfs"] = t["max_dbfs"]
        t["score"] = round(t["max_dbfs"] * (0.3 + t["duty"]), 1)
    signals = sorted(tracked.values(), key=lambda s: -s["score"])
    report = {
        "frames": sum(r["frames"] for r in rows),
        "duration_s": sum(r.get("frames", 0) for r in rows),
        "nfft": rows[0]["nfft"] if rows else 256,
        "lo_mhz": float(start),
        "rate_hz": rows[0]["rate_hz"] if rows else 16_000_000,
        "bin_hz": 0.0,
        "typical_floor_dbfs": -100.0,
        "threshold_db": 0.0,
        "mode": "sweep",
        "sweep": {"start_mhz": start, "stop_mhz": stop, "steps": steps},
        "signals": signals,
    }
    assess_threat(report)
    (session_dir / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def cmd_sweep(args) -> int:
    session_dir = make_session_dir(args.out, "sweep")
    port = open_session(args.port, args.baud)
    try:
        report = sweep_core(port, session_dir, args.start, args.stop,
                            args.step_mhz or 15, args.dwell_ms,
                            args.rate, args.nfft, args.top)
    finally:
        port.close()
    print(f"sweep session: {session_dir}  "
          f"({report['sweep']['steps']} steps, {len(report['signals'])} signals)")
    return 0


# ---------------------------------------------------------------- analyze cmd

def load_frames(session_dir: Path) -> list[dict]:
    path = session_dir / "frames.jsonl"
    return [json.loads(l) for l in path.read_text().splitlines() if l.strip()]

LEVELS = {"AMBIENT": 0, "WATCH": 1, "CONCERN": 2}

def rhythm_deltas(frames: list[dict], anchor: int, threshold_db: float) -> list[float]:
    """Inter-arrival seconds between frames where the bin shows a peak."""
    ts = [f["t_s"] for f in frames
          if f["codes"][anchor] >= f["floor_code"] + threshold_db]
    return [b - a for a, b in zip(ts, ts[1:])]

def rhythm_periodic(deltas: list[float]) -> tuple[bool, float | None]:
    """Periodic iff >=6 deltas, mean gap in [0.2, 60]s, CV (stddev/mean) < 0.25."""
    if len(deltas) < 6:
        return False, None
    mean = sum(deltas) / len(deltas)
    if not 0.2 <= mean <= 60:
        return False, None
    var = sum((d - mean) ** 2 for d in deltas) / len(deltas)
    if math.sqrt(var) / mean >= 0.25:
        return False, None
    return True, round(mean, 3)

def threat_rules(sig: dict, floor_dbfs: float, is_new: bool = False,
                 delta_db: float | None = None) -> tuple[str, list[str]]:
    """Threat level for one signal + fired-rule reasons (each names freq + rule)."""
    freq, cls, duty, mx = sig["freq_mhz"], sig["class"], sig["duty"], sig["max_dbfs"]
    level, reasons = "AMBIENT", []

    def fire(lv, text):
        nonlocal level
        reasons.append(f"{freq:.6f} MHz {lv}: {text}")
        if LEVELS[lv] > LEVELS[level]:
            level = lv

    if cls == "STEADY" and mx >= floor_dbfs + 20:
        fire("CONCERN", f"STEADY carrier {mx:+.1f} dB >= floor+20 ({floor_dbfs + 20:+.1f})")
    if sig.get("periodic"):
        fire("CONCERN", f"PERIODIC rhythm every {sig.get('period_s')} s")
    if is_new and duty >= 0.1 and (delta_db or 0.0) >= 10:
        fire("CONCERN", f"NEW vs baseline, duty {duty:.2f}, +{delta_db:.1f} dB over baseline floor")
    if cls == "BURSTY" and mx >= -30:
        fire("WATCH", f"BURSTY at {mx:+.1f} dB >= -30")
    if 0.3 <= duty < 0.8:
        fire("WATCH", f"duty {duty:.2f} in the 0.3-0.8 band")
    return level, reasons

def assess_threat(report: dict) -> dict:
    """Fill per-signal threat fields + session verdict threat{level, reasons}."""
    floor = report.get("typical_floor_dbfs", -100.0)
    top, reasons = "AMBIENT", []
    for s in report.get("signals", []):
        s.setdefault("periodic", False)
        s.setdefault("period_s", None)
        s.setdefault("is_new", False)
        s.setdefault("delta_db", None)
        level, rs = threat_rules(s, floor, s["is_new"], s["delta_db"])
        s["threat"] = level
        reasons += rs
        if LEVELS[level] > LEVELS[top]:
            top = level
    report["threat"] = {"level": top, "reasons": reasons}
    return report

def diff_baseline(report: dict, baseline: dict) -> dict:
    """Mark signals KNOWN (within 2 MHz of a baseline signal, delta dB reported)
    or NEW vs the baseline session (delta dB vs the baseline noise floor)."""
    base_sigs = baseline.get("signals", [])
    base_floor = baseline.get("typical_floor_dbfs", -100.0)
    for s in report["signals"]:
        near = [b for b in base_sigs if abs(b["freq_mhz"] - s["freq_mhz"]) <= 2.0]
        match = min(near, key=lambda b: abs(b["freq_mhz"] - s["freq_mhz"]), default=None)
        if match:
            s["is_new"] = False
            s["delta_db"] = round(s["max_dbfs"] - match["max_dbfs"], 2)
        else:
            s["is_new"] = True
            s["delta_db"] = round(s["max_dbfs"] - base_floor, 2)
    return report


def analyze_frames(frames: list[dict], threshold_db: float = 10.0,
                   min_frames: int = 2, timeline_buckets: int = 60,
                   baseline: dict | None = None) -> dict:
    if not frames:
        return assess_threat({"signals": [], "frames": 0})
    nfft = frames[0]["nfft"]
    lo = frames[0]["lo_mhz"]
    rate = frames[0]["rate_hz"]
    t_max = max(f["t_s"] for f in frames) or 1.0

    bin_max = [0] * nfft
    bin_sum = [0] * nfft
    bin_active = [0] * nfft
    floor_codes = sorted(f["floor_code"] for f in frames)
    typical_floor = floor_codes[len(floor_codes) // 2]
    for f in frames:
        for k, code in enumerate(f["codes"]):
            if code > bin_max[k]:
                bin_max[k] = code
            bin_sum[k] += code
            if code >= f["floor_code"] + threshold_db:
                bin_active[k] += 1

    # one signal per local maximum of the session max-hold; merge peaks
    # within 2 bins so a flat plateau counts once
    candidates = [k for k in range(nfft)
                  if bin_active[k] >= min_frames and bin_max[k] >= typical_floor + threshold_db]
    peaks = []
    for k in candidates:
        left = bin_max[k - 1] if k > 0 else -1
        right = bin_max[k + 1] if k + 1 < nfft else -1
        if bin_max[k] >= right and bin_max[k] > left:
            peaks.append(k)
    merged: list[int] = []
    for k in sorted(peaks, key=lambda j: -bin_max[j]):
        if k <= 2 or k >= nfft - 2:
            continue  # bin 0 = tuned center in the shifted wire convention;
                      # mixer DC leaks there and is not an emitter
        if any(abs(k - m) <= 2 for m in merged):
            continue
        merged.append(k)
    signals = []
    for anchor in merged:
        lo_k = max(0, anchor - 1)
        hi_k = min(nfft - 1, anchor + 1)
        duty = bin_active[anchor] / len(frames)
        mean_code = bin_sum[anchor] / len(frames)
        cls = ("STEADY" if duty >= 0.8 else "BURSTY" if duty >= 0.1 else "SPORADIC")
        periodic, period_s = rhythm_periodic(rhythm_deltas(frames, anchor, threshold_db))
        buckets = []
        for b in range(timeline_buckets):
            sel = [f for f in frames
                   if int(f["t_s"] / t_max * (timeline_buckets - 1)) == b]
            if sel:
                buckets.append(max(max(f["codes"][j] for j in range(lo_k, hi_k + 1))
                                   for f in sel))
            else:
                buckets.append(None)
        signals.append({
            "freq_mhz": round(bin_frequency_mhz(anchor, nfft, lo, rate), 6),
            "bin": anchor,
            "bin_span": [lo_k, hi_k],
            "max_code": bin_max[anchor],
            "max_dbfs": round(code_to_dbfs(bin_max[anchor], nfft), 2),
            "mean_dbfs": round(code_to_dbfs(mean_code, nfft), 2),
            "duty": round(duty, 3),
            "class": cls,
            "periodic": periodic,
            "period_s": period_s,
            "score": round((bin_max[anchor] - typical_floor) * (0.3 + duty), 1),
            "timeline": buckets,
        })
    signals.sort(key=lambda s: -s["score"])
    report = {
        "frames": len(frames),
        "duration_s": round(t_max, 2),
        "nfft": nfft,
        "lo_mhz": lo,
        "rate_hz": rate,
        "bin_hz": rate / nfft,
        "typical_floor_dbfs": round(code_to_dbfs(typical_floor, nfft), 2),
        "threshold_db": threshold_db,
        "signals": signals,
    }
    if baseline:
        diff_baseline(report, baseline)
    return assess_threat(report)


def load_baseline(path: Path) -> dict:
    """Baseline report: prefer report.json, else derive from the session frames."""
    if (path / "report.json").exists():
        return json.loads((path / "report.json").read_text())
    if (path / "frames.jsonl").exists():
        return analyze_frames(load_frames(path))
    raise FileNotFoundError(f"baseline {path} has no report.json or frames.jsonl")

def cmd_analyze(args) -> int:
    session_dir = Path(args.session)
    frames = load_frames(session_dir)
    try:
        baseline = load_baseline(Path(args.baseline)) if args.baseline else None
    except FileNotFoundError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    result = analyze_frames(frames, threshold_db=args.threshold, baseline=baseline)
    (session_dir / "report.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"frames: {result['frames']}  duration: {result['duration_s']}s  "
          f"floor: {result.get('typical_floor_dbfs')}dBFS")
    print(f"{'FREQ MHz':>12} {'MAX dBFS':>9} {'MEAN dBFS':>10} {'DUTY':>6} "
          f"{'CLASS':>10} {'THREAT':>8} {'NEW':>5} {'DELTA':>7} RHYTHM")
    for s in result["signals"][:25]:
        rhythm = f"PERIODIC {s['period_s']}s" if s["periodic"] else "-"
        known = "NEW" if s["is_new"] else ("known" if s["delta_db"] is not None else "-")
        print(f"{s['freq_mhz']:12.6f} {s['max_dbfs']:9.1f} {s['mean_dbfs']:10.1f} "
              f"{s['duty']:6.2f} {s['class']:>10} {s['threat']:>8} "
              f"{known:>5} "
              f"{s['delta_db'] if s['delta_db'] is not None else '-':>7} {rhythm}")
    verdict = result.get("threat", {"level": "AMBIENT", "reasons": []})
    word = "CLEAN" if verdict["level"] == "AMBIENT" else verdict["level"]
    print(f"verdict: {word}")
    for r in verdict["reasons"]:
        print(f"  {r}")
    print(f"report: {session_dir / 'report.json'}")
    return 0


# ----------------------------------------------------------------- report cmd

HTML_TEMPLATE = """<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>T-Embed RF session report</title>
<style>
 :root { --bg:#0f1216; --panel:#171c22; --ink:#e8e4da; --dim:#8d95a1;
         --accent:#4cc2ff; --hot:#ff5d5d; --warm:#ffb84d; --ok:#57d98a; }
 body { background:var(--bg); color:var(--ink); margin:0;
        font:14px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace; }
 .wrap { max-width:1080px; margin:0 auto; padding:28px 20px 60px; }
 h1 { font-size:22px; letter-spacing:.06em; margin:0 0 4px; }
 h2 { font-size:14px; letter-spacing:.14em; text-transform:uppercase;
      color:var(--dim); margin:34px 0 12px; border-bottom:1px solid #262d36;
      padding-bottom:6px; }
 .sub { color:var(--dim); margin-bottom:24px; }
 .meta { display:grid; grid-template-columns:repeat(auto-fit,minmax(180px,1fr));
         gap:10px; }
 .meta div { background:var(--panel); border-radius:8px; padding:10px 12px; }
 .meta b { display:block; color:var(--dim); font-size:11px;
           letter-spacing:.12em; text-transform:uppercase; }
 table { width:100%; border-collapse:collapse; }
 th,td { text-align:right; padding:6px 10px; border-bottom:1px solid #222932; }
 th { color:var(--dim); font-size:11px; letter-spacing:.1em; text-transform:uppercase; }
 td:first-child, th:first-child { text-align:left; }
 .bar { position:relative; background:#202732; border-radius:4px; height:14px;
        min-width:120px; overflow:hidden; }
 .bar i { position:absolute; inset:0 auto 0 0; border-radius:4px; }
 .cls-STEADY { color:var(--hot); } .cls-BURSTY { color:var(--warm); }
 .cls-SPORADIC { color:var(--ok); }
 .tl { display:grid; grid-auto-flow:column; gap:2px; height:16px; }
 .tl span { border-radius:2px; }
 .verdict { border:1px solid; border-radius:10px; padding:14px 18px; margin:0 0 18px; }
 .verdict ul { margin:8px 0 0; padding-left:20px; }
 .vword { font-size:17px; font-weight:700; letter-spacing:.12em; }
 .v-AMBIENT { background:#12291c; border-color:#2e6b46; }
 .v-AMBIENT .vword { color:var(--ok); }
 .v-WATCH { background:#2e2410; border-color:#7a5c22; }
 .v-WATCH .vword { color:var(--warm); }
 .v-CONCERN { background:#331414; border-color:#7c2c2c; }
 .v-CONCERN .vword { color:var(--hot); }
 .chip { display:inline-block; padding:1px 9px; border-radius:999px;
         font-size:11px; letter-spacing:.08em; }
 .chip-AMBIENT { background:#1b4a2d; color:#9bf3bd; }
 .chip-WATCH { background:#4a3a13; color:#ffd27a; }
 .chip-CONCERN { background:#4f1d1d; color:#ff9b9b; }
 .badge { display:inline-block; padding:1px 8px; border:1px solid var(--accent);
          color:var(--accent); border-radius:999px; font-size:11px;
          letter-spacing:.06em; }
 tr.row-AMBIENT td { background:rgba(87,217,138,.08); }
 tr.row-WATCH td { background:rgba(255,184,77,.10); }
 tr.row-CONCERN td { background:rgba(255,93,93,.12); }
 .coverage { border:1px dashed #3a424d; border-radius:8px; padding:10px 14px;
             color:var(--dim); margin:0 0 18px; }
 .foot { color:var(--dim); margin-top:40px; font-size:12px; }
</style></head><body><div class="wrap">
__VERDICT__
<div class="coverage">COVERAGE 2196-2806 MHz only &mdash; this radio cannot
see 4G GPS trackers or 433 MHz transmitters.</div>
<h1>T-EMBED RF SESSION</h1>
<div class="sub">__SUBTITLE__</div>
<h2>Session</h2>
<div class="meta">__META__</div>
<h2>Top spike frequencies</h2>
<table><tr><th>Frequency</th><th>Peak</th><th>Strength</th><th>Mean</th>
<th>Duty</th><th>Class</th><th>Threat</th><th>Activity (time &rarr;)</th></tr>
__ROWS__
</table>
<h2>Interpretation</h2>
<div class="sub">__NOTES__</div>
<div class="foot">Codes are on-device FFT power (0.5 dB quantization).
dBFS is approximate (Hann normalization, 10-bit full scale). Code 0 is a
quantized floor, not proof of no RF. STEADY = carrier active &ge;80% of frames
(classic hidden transmitter / video bug); BURSTY = periodic or handshake-like
traffic; SPORADIC = rare events. Frequencies span the software tuning range;
RF accuracy far from 2.4 GHz is unverified on this hardware.</div>
</div></body></html>
"""


def heat_color(dbfs: float, floor_dbfs: float) -> str:
    span = max(1.0, dbfs - floor_dbfs)
    if span >= 35:
        return "#ff5d5d"
    if span >= 20:
        return "#ffb84d"
    return "#57d98a"


def cmd_report(args) -> int:
    session_dir = Path(args.session)
    report = json.loads((session_dir / "report.json").read_text())
    meta = {}
    meta_path = session_dir / "meta.json"
    if meta_path.exists():
        meta = json.loads(meta_path.read_text())
    floor = report.get("typical_floor_dbfs", -100.0)
    if "threat" not in report:
        assess_threat(report)  # legacy report.json: derive levels from fields
    threat = report["threat"]
    word = "CLEAN" if threat["level"] == "AMBIENT" else threat["level"]
    reasons = "".join(f"<li>{r}</li>" for r in threat["reasons"]) or \
        "<li>no rules fired &mdash; nothing of concern in this session</li>"
    verdict_html = (f"<div class='verdict v-{threat['level']}'>"
                    f"<span class='vword'>VERDICT: {word}</span>"
                    f"<ul>{reasons}</ul></div>")

    meta_html = "".join(
        f"<div><b>{k}</b>{v}</div>"
        for k, v in [("frames", report.get("frames", 0)),
                     ("duration", f"{report.get('duration_s', 0)} s"),
                     ("center", f"{report.get('lo_mhz', 0)} MHz"),
                     ("bin width", f"{report.get('bin_hz', 0) / 1e3:.1f} kHz"),
                     ("noise floor", f"{floor:+.1f} dBFS"),
                     ("device", meta.get("INFO", "?"))]
    )
    rows = []
    for s in report.get("signals", [])[:30]:
        width = max(2, min(100, int((s["max_dbfs"] - floor) * 2.4)))
        color = heat_color(s["max_dbfs"], floor)
        tl = "".join(
            f'<span style="background:{"transparent" if v is None else heat_color(code_to_dbfs(v, report["nfft"]), floor)}"></span>'
            for v in s.get("timeline", []))
        level = s.get("threat", "AMBIENT")
        badge = (f" <span class='badge'>PERIODIC {s['period_s']} s</span>"
                 if s.get("periodic") else "")
        rows.append(
            f"<tr class='row-{level}'><td>{s['freq_mhz']:.6f} MHz</td><td>{s['max_dbfs']:+.1f} dB</td>"
            f"<td><div class='bar'><i style='width:{width}%;background:{color}'></i></div></td>"
            f"<td>{s['mean_dbfs']:+.1f} dB</td><td>{s['duty']:.2f}</td>"
            f"<td class='cls-{s['class']}'>{s['class']}</td>"
            f"<td><span class='chip chip-{level}'>{level}</span>{badge}</td>"
            f"<td><div class='tl'>{tl}</div></td></tr>")

    notes = ("Strongest signals first (score combines peak strength and duty). "
             "A STEADY entry near a fixed frequency is the signature of an "
             "always-on transmitter: covert audio/video bugs, analog video "
             "sends, and dedicated trackers hold a carrier. BURSTY entries at "
             "regular duty are typically digital chatter (Wi-Fi, BLE, sensors, "
             "telemetry). PERIODIC marks a regular burst rhythm (beacon or "
             "tracker heartbeat). Compare two sessions to see which signals "
             "follow you: a device that appears at every location is on your "
             "person or vehicle.")
    html = (HTML_TEMPLATE
            .replace("__VERDICT__", verdict_html)
            .replace("__SUBTITLE__",
                     f"session {session_dir.name} &mdash; generated "
                     f"{dt.datetime.now().astimezone().strftime('%Y-%m-%d %H:%M %Z')}")
            .replace("__META__", meta_html)
            .replace("__ROWS__", "\n".join(rows) or "<tr><td colspan='8'>no signals above threshold</td></tr>")
            .replace("__NOTES__", notes))
    out = session_dir / "report.html"
    out.write_text(html)
    print(f"report: {out}")
    return 0


# --------------------------------------------------------------------- utils

def make_session_dir(explicit, kind: str) -> Path:
    if explicit:
        d = Path(explicit)
    else:
        stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
        d = LOG_ROOT / f"{kind}_{stamp}"
    d.mkdir(parents=True, exist_ok=True)
    return d


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--baud", type=int, default=115200)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("info", help="device + capability snapshot")
    p.add_argument("--output", type=Path)
    p.set_defaults(fn=cmd_info)

    p = sub.add_parser("capture", help="continuous spectrum capture")
    p.add_argument("--seconds", type=float, default=60)
    p.add_argument("--center", type=float, default=0,
                   help="tune LO to this MHz before capture (0 = leave as-is)")
    p.add_argument("--rate", type=int, default=6, choices=(0, 1, 6),
                   help="rate code: 0=80MS/s 1=40MS/s 6=16MS/s")
    p.add_argument("--nfft", type=int, default=256)
    p.add_argument("--stride", type=int, default=1)
    p.add_argument("--units", type=int, default=1)
    p.add_argument("--max-hold", action="store_true",
                   help="max-power detector instead of mean")
    p.add_argument("--stats", action="store_true", help="request SPS1 telemetry")
    p.add_argument("--top", type=int, default=8)
    p.add_argument("--quiet", action="store_true")
    p.add_argument("--out", type=Path)
    p.set_defaults(fn=cmd_capture)

    p = sub.add_parser("sweep", help="step across a frequency range")
    p.add_argument("--start", type=int, required=True)
    p.add_argument("--stop", type=int, required=True)
    p.add_argument("--step-mhz", type=int, default=0)
    p.add_argument("--dwell-ms", type=int, default=300)
    p.add_argument("--rate", type=int, default=6, choices=(0, 1, 6))
    p.add_argument("--nfft", type=int, default=256)
    p.add_argument("--top", type=int, default=8)
    p.add_argument("--out", type=Path)
    p.set_defaults(fn=cmd_sweep)

    p = sub.add_parser("analyze", help="aggregate a session")
    p.add_argument("session", type=Path)
    p.add_argument("--threshold", type=float, default=10.0,
                   help="dB above per-frame floor to count as active")
    p.add_argument("--baseline", type=Path, default=None,
                   help="baseline session dir; diff its report.json to mark "
                        "signals KNOWN (delta dB) vs NEW")
    p.set_defaults(fn=cmd_analyze)

    p = sub.add_parser("report", help="render report.html for a session")
    p.add_argument("session", type=Path)
    p.set_defaults(fn=cmd_report)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    raise SystemExit(main())
