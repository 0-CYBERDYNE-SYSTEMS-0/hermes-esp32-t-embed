#!/usr/bin/env python3
"""Collect bounded, text-only SDR diagnostics without resetting the board."""
import argparse
import json
import time
from pathlib import Path

import serial

UI_FIELDS = ["ready", "paused", "frequency_mhz", "span_mhz", "step_mhz", "nfft",
             "screen_frames", "rf_frames", "ffts", "bin_min", "bin_max", "checksum",
             "frame_age_ms", "capture_us", "late_max", "drops", "failures"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/cu.usbmodem1101")
    parser.add_argument("--duration", type=float, default=0)
    parser.add_argument("--interval", type=float, default=6)
    parser.add_argument("--output", type=Path)
    parser.add_argument("commands", nargs="*", default=["INFO", "CAPS", "LIMITS?", "UI?"])
    args = parser.parse_args()
    log = []
    connection = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=2)
    connection.dtr = False
    connection.rts = False
    connection.port = args.port
    connection.open()
    # Native USB control-line changes can restart the chip. Drain boot text
    # before sending protocol commands, which are unavailable during startup.
    boot_deadline = time.monotonic() + 4
    while time.monotonic() < boot_deadline:
        connection.read(4096)
    connection.reset_input_buffer()
    started = time.monotonic()

    def query(command):
        connection.write((command + "\n").encode("ascii"))
        connection.flush()
        deadline = time.monotonic() + 5
        response = bytearray()
        while time.monotonic() < deadline:
            chunk = connection.read(4096)
            if chunk:
                response.extend(chunk)
                if b"\n" in response:
                    break
        record = {"elapsed_s": round(time.monotonic() - started, 3), "command": command,
                  "reply": response.decode("utf-8", errors="replace").strip()}
        fields = record["reply"].split()
        if len(fields) == len(UI_FIELDS) + 1 and fields[0] == "UI":
            record["ui"] = {name: int(value, 16 if name == "checksum" else 10)
                            for name, value in zip(UI_FIELDS, fields[1:])}
        log.append(record)
        print(json.dumps(record), flush=True)
        if not response:
            raise RuntimeError("No reply to " + command)

    try:
        for command in args.commands:
            query(command)
        query("RELEASE")
        while time.monotonic() - started < args.duration:
            time.sleep(min(args.interval, max(0, args.duration - (time.monotonic() - started))))
            query("UI?")
            query("RELEASE")
    finally:
        connection.close()
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(log, indent=2) + "\n")


if __name__ == "__main__":
    main()
