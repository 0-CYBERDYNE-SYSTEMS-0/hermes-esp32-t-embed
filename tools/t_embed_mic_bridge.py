#!/usr/bin/env python3
"""Forward T-Embed serial microphone packets to BlackHole 2ch."""

import argparse
from array import array
import binascii
import struct
import sys
import time
from typing import List, Tuple


SAMPLE_RATE = 16000
FRAME_SAMPLES = 80
PAYLOAD_BYTES = FRAME_SAMPLES * 2
PACKET_DATA_BYTES = 8 + PAYLOAD_BYTES
PACKET_BYTES = PACKET_DATA_BYTES + 2
MAGIC = b"TMIC"


class FrameParser:
    def __init__(self):
        self.buffer = bytearray()
        self.expected_sequence = None
        self.dropped_frames = 0

    def feed(self, data: bytes) -> List[Tuple[int, bytes]]:
        self.buffer.extend(data)
        frames = []
        while True:
            start = self.buffer.find(MAGIC)
            if start < 0:
                del self.buffer[:-len(MAGIC) + 1]
                break
            if start:
                del self.buffer[:start]
            if len(self.buffer) < PACKET_BYTES:
                break

            sequence = struct.unpack_from("<I", self.buffer, 4)[0]
            checksum = struct.unpack_from("<H", self.buffer, PACKET_DATA_BYTES)[0]
            if binascii.crc_hqx(self.buffer[:PACKET_DATA_BYTES], 0xFFFF) != checksum:
                del self.buffer[0]
                continue
            if self.expected_sequence is not None:
                gap = (sequence - self.expected_sequence) & 0xFFFFFFFF
                if gap < 0x80000000:
                    self.dropped_frames += gap
            payload = bytes(self.buffer[8:PACKET_DATA_BYTES])
            frames.append((sequence, payload))
            self.expected_sequence = (sequence + 1) & 0xFFFFFFFF
            del self.buffer[:PACKET_BYTES]
        return frames


def mono_to_stereo(payload: bytes) -> bytes:
    samples = array("h")
    samples.frombytes(payload)
    if sys.byteorder != "little":
        samples.byteswap()
    stereo = array("h", [0]) * (len(samples) * 2)
    for index, sample in enumerate(samples):
        stereo[index * 2] = sample
        stereo[index * 2 + 1] = sample
    return stereo.tobytes()


def find_blackhole_output(sounddevice):
    for index, device in enumerate(sounddevice.query_devices()):
        if "blackhole 2ch" in device["name"].lower() and device["max_output_channels"] >= 2:
            return index
    raise RuntimeError(
        "BlackHole 2ch output not found. Install it with `brew install --cask blackhole-2ch` "
        "and restart macOS."
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="USB Serial/JTAG device, e.g. /dev/cu.usbmodem1101")
    args = parser.parse_args()

    try:
        import serial
        import sounddevice
    except ImportError as error:
        raise SystemExit(
            "Install bridge dependencies with: python -m pip install -r "
            "tools/requirements-t-embed-mic-bridge.txt"
        ) from error

    output_device = find_blackhole_output(sounddevice)
    frame_parser = FrameParser()
    pending_pcm = bytearray()
    last_ping = time.monotonic()
    played_frames = 0

    print("T-Embed mic -> BlackHole 2ch, 16 kHz stereo. Press Ctrl+C to stop.", flush=True)
    with sounddevice.RawOutputStream(
        samplerate=SAMPLE_RATE,
        channels=2,
        dtype="int16",
        device=output_device,
        blocksize=FRAME_SAMPLES * 2,
    ) as audio:
        with serial.Serial(args.port, 115200, timeout=0.02, write_timeout=1) as port:
            port.reset_input_buffer()
            port.write(b"START\n")
            try:
                while True:
                    data = port.read(4096)
                    for _, payload in frame_parser.feed(data):
                        pending_pcm.extend(payload)
                        while len(pending_pcm) >= PAYLOAD_BYTES * 2:
                            block = bytes(pending_pcm[:PAYLOAD_BYTES * 2])
                            del pending_pcm[:PAYLOAD_BYTES * 2]
                            audio.write(mono_to_stereo(block))
                            played_frames += FRAME_SAMPLES * 2

                    now = time.monotonic()
                    if now - last_ping >= 1:
                        port.write(b"PING\n")
                        last_ping = now
                    if played_frames:
                        print(
                            f"\r{played_frames} samples sent; dropped serial frames: "
                            f"{frame_parser.dropped_frames}",
                            end="",
                            flush=True,
                        )
                        played_frames = 0
            except KeyboardInterrupt:
                try:
                    port.write(b"STOP\n")
                except serial.SerialException:
                    pass
                print("\nBridge stopped.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
