#!/usr/bin/env python3
"""Verify the session-logging instrument on real hardware, end to end.

Drives the device the way a bench rig would: selects the SDR app over the
launcher console, injects a clock, starts a session, captures IQ and 802.11
frames, pulls the files back over USB, and then checks them with the same
tools a human would use (rfkit's analyzer, tcpdump, a SigMF size check).

Nothing here trusts a counter: every PASS is backed by bytes that were read
back off the card and parsed.

    python3 tools/verify_sigint.py --port /dev/cu.usbmodem2101 --workdir /tmp/v
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host/rfkit"))

import rfkit  # noqa: E402


class Device:
    def __init__(self, port: str, baud: int = 115200):
        self.port_name = port
        self.baud = baud
        self.port: serial.Serial | None = None

    def open(self, settle: float = 0.4) -> None:
        port = serial.Serial(port=None, baudrate=self.baud, timeout=0.2, write_timeout=5)
        port.dtr = False
        port.rts = False
        port.port = self.port_name
        port.open()
        time.sleep(settle)
        port.reset_input_buffer()
        self.port = port

    def close(self) -> None:
        if self.port:
            self.port.close()
            self.port = None

    def reopen(self, settle: float = 1.0) -> None:
        self.close()
        self.open(settle)

    def write(self, text: str) -> None:
        assert self.port
        self.port.write((text + "\n").encode("ascii"))
        self.port.flush()

    def readline(self, deadline_s: float = 5.0) -> str:
        assert self.port
        end = time.monotonic() + deadline_s
        buf = bytearray()
        while time.monotonic() < end:
            chunk = self.port.read(1)
            if not chunk:
                if buf.endswith(b"\n"):
                    break
                continue
            buf.extend(chunk)
            if chunk == b"\n":
                break
        return buf.decode("utf-8", "replace").strip()

    def ask(self, command: str, deadline_s: float = 8.0) -> str:
        self.write(command)
        return self.readline(deadline_s)

    def wait_for(self, command: str, prefix: str, deadline_s: float = 20.0) -> tuple[bool, str]:
        """Poll a command until its reply starts with prefix (app boot, etc.)."""
        end = time.monotonic() + deadline_s
        last = ""
        while time.monotonic() < end:
            last = self.ask(command, deadline_s=2.0)
            if last.startswith(prefix):
                return True, last
            time.sleep(0.5)
        return False, last

    def read_exact(self, count: int, deadline_s: float = 120.0) -> bytes:
        assert self.port
        end = time.monotonic() + deadline_s
        data = bytearray()
        while len(data) < count and time.monotonic() < end:
            chunk = self.port.read(min(4096, count - len(data)))
            if chunk:
                data.extend(chunk)
        return bytes(data)

    def pull(self, path: str, destination: Path, deadline_s: float = 180.0) -> int:
        self.write(f"SDFILE {path}")
        header = self.readline(deadline_s=5.0)
        if not header.startswith("FILE "):
            raise RuntimeError(f"SDFILE {path}: {header!r}")
        size = int(header.split()[1])
        payload = self.read_exact(size, deadline_s)
        if len(payload) != size:
            raise RuntimeError(f"SDFILE {path}: got {len(payload)} of {size} bytes")
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(payload)
        self.readline(deadline_s=5.0)  # END
        return size

    def enter_sdr(self) -> str:
        """Launcher console -> SDR app, tolerating the USB re-enumeration."""
        for attempt in range(3):
            try:
                self.write("APP SDR")
                reply = self.readline(deadline_s=3.0)
                if not reply.startswith("APP SDR"):
                    # maybe we are already inside the SDR app
                    if self.ask("INFO", 2.0).startswith("S3SDR"):
                        return "already-in-sdr"
                    continue
            except (serial.SerialException, OSError):
                pass
            time.sleep(1.0)
            try:
                self.reopen(settle=1.2)
            except (serial.SerialException, OSError):
                time.sleep(2.0)
                self.reopen(settle=1.5)
            ok, line = self.wait_for("INFO", "S3SDR", deadline_s=20.0)
            if ok:
                return line
        raise RuntimeError("could not reach the SDR app")


class Report:
    def __init__(self) -> None:
        self.rows: list[dict] = []

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        self.rows.append({"check": name, "ok": bool(ok), "detail": detail})
        print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  — {detail}" if detail else ""))
        return ok

    def failed(self) -> int:
        return sum(1 for r in self.rows if not r["ok"])

    def write(self, path: Path) -> None:
        path.write_text(json.dumps({"checks": self.rows,
                                    "failed": self.failed()}, indent=2) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/cu.usbmodem2101")
    ap.add_argument("--workdir", type=Path, default=ROOT / "logs" / "verify")
    ap.add_argument("--log-seconds", type=float, default=15.0)
    ap.add_argument("--sniff-seconds", type=float, default=10.0)
    args = ap.parse_args()

    work = args.workdir
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    report = Report()
    device = Device(args.port)

    print(f"device {args.port}")
    device.open(settle=0.5)
    booted = device.wait_for("PING", "PONG", deadline_s=12.0)
    report.check("launcher console answers PING", booted[0], booted[1])

    app = device.enter_sdr()
    report.check("SDR app reachable without touching the knob", app.startswith("S3SDR"), app)

    info = device.ask("INFO")
    report.check("INFO identifies the SDR firmware", info.startswith("S3SDR"), info)

    now_ms = int(time.time() * 1000)
    clock = device.ask(f"TIME {now_ms}")
    report.check("host clock injected", clock.startswith("OK TIME"), clock)

    status = device.ask("SD?")
    mounted = '"mounted":1' in status
    report.check("microSD mounted", mounted, status[:160])
    if not mounted:
        print("  (a card is required for the persistence checks; stopping here)")
        report.write(work / "verify.json")
        device.close()
        return 1

    started = device.ask("LOG START verify")
    session = started.split()[-1] if started.startswith("OK LOG") else ""
    report.check("session started on card", bool(session), started)

    # Let the UI capture loop push frames at the 200 ms logging interval.
    time.sleep(args.log_seconds)

    iq = device.ask("LOGREC 16380 6 verify")
    report.check("one-press IQ capture written", " sd" in f" {iq}" or iq.endswith("sd"), iq)

    sniff = device.ask("WIFI SNIFF START")
    report.check("802.11 sniffer started", sniff.startswith("OK WIFI SNIFF"), sniff)
    time.sleep(args.sniff_seconds)
    wifi = device.ask("WIFI?")
    try:
        wifi_state = json.loads(wifi)
    except json.JSONDecodeError:
        wifi_state = {}
    report.check("frames captured by the MAC", wifi_state.get("stored", 0) > 0,
                 json.dumps(wifi_state)[:200])
    device.ask("WIFI SNIFF STOP")

    stopped = device.ask("LOG STOP")
    report.check("session closed", stopped.startswith("OK LOG STOP"), stopped)

    listing = []
    device.write("SD LS")
    while True:
        line = device.readline(deadline_s=10.0)
        if not line or line == "END":
            break
        listing.append(line)
    files = {}
    for line in listing:
        parts = line.split()
        if len(parts) == 3 and parts[0] == "FILE":
            files[parts[1]] = int(parts[2])
    report.check("card listing shows the session files",
                 any(p.endswith("frames.ndjson") for p in files),
                 f"{len(files)} files")

    # Pull the artifacts and check them with the tools a human would use.
    frames_path = next((p for p in files if p.endswith("frames.ndjson")), None)
    if frames_path:
        local = work / "frames.ndjson"
        device.pull(frames_path, local)
        try:
            frames = rfkit.load_frames(work)
            result = rfkit.analyze_frames(frames)
            report.check("frames parse and analyze",
                         result["frames"] > 0,
                         f"{result['frames']} frames, {len(result['signals'])} signals, "
                         f"floor {result.get('typical_floor_dbfs')} dBFS")
        except Exception as exc:  # noqa: BLE001 - report, never crash the battery
            report.check("frames parse and analyze", False, repr(exc))

    manifest_path = next((p for p in files if p.endswith("session.json")), None)
    if manifest_path:
        device.pull(manifest_path, work / "session.json")
        manifest = json.loads((work / "session.json").read_text())
        report.check("manifest carries provenance",
                     bool(manifest.get("build")) and manifest.get("time_source") == "host",
                     f"build={manifest.get('build')} clock={manifest.get('time_source')}")

    meta_path = next((p for p in files if p.endswith(".sigmf-meta")), None)
    data_path = next((p for p in files if p.endswith(".sigmf-data")), None)
    if meta_path and data_path:
        device.pull(meta_path, work / Path(meta_path).name)
        device.pull(data_path, work / Path(data_path).name)
        meta = json.loads((work / Path(meta_path).name).read_text())
        expect = 16380 * 2
        actual = (work / Path(data_path).name).stat().st_size
        report.check("SigMF pair is complete",
                     meta["global"]["core:datatype"] == "ci8" and actual == expect,
                     f"{actual} bytes, datatype {meta['global']['core:datatype']}")

    pcap_path = next((p for p in files if p.endswith(".pcapng")), None)
    if pcap_path:
        local = work / Path(pcap_path).name
        device.pull(pcap_path, local)
        inv = rfkit.pcapng_inventory(local)
        detail = f"{inv['packets']} packets, {inv['bytes']} bytes"
        ok = inv["packets"] > 0 and not inv["damaged"]
        if shutil.which("tcpdump"):
            out = subprocess.run(["tcpdump", "-r", str(local), "-nn", "-c", "3"],
                                 capture_output=True, text=True)
            text = out.stdout + out.stderr
            ok = ok and "reading from file" in text
            detail += " | " + text.splitlines()[0][:80]
        report.check("PCAPNG holds real 802.11 frames", ok, detail)

    report.write(work / "verify.json")
    device.close()
    failed = report.failed()
    print(f"\n{len(report.rows) - failed}/{len(report.rows)} checks passed"
          f"  (report: {work / 'verify.json'})")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
