"""SPC1/SPS1 wire-format decoding for the ESP-SDR T-Embed spectrum stream.

Frame format (docs/spectrum.md in hermes-esp32-t-embed):

  Offset  Bytes  Meaning
  0       4      ASCII "SPC1"
  4       4      frame sequence (u32 LE)
  8       8      first sample index (u64 LE)
  16      4      samples spanned (u32 LE)
  20      2      completed FFTs represented (u16 LE)
  22      1      flags: b0 max-detector, b1 skipped, b2 output drop, b3 gap
  23      1      gain metadata from first I/Q word
  24      2      cumulative frame drops (u16 LE)
  26      1      log2(FFT bins)
  27      1      dB code multiplier (2)
  28      N      power codes, natural FFT order
  28+N    4      CRC32 of header + bins (LE)

Power code: code = round(20*log10(mean-or-max |X|^2)) in the normalized Q15
FFT domain, clamped 0..255.  code 0 is a quantized floor, not "no RF".

SPS1 stats record: 4-byte magic + 36-byte body + 4-byte CRC32 = 40 bytes.
"""

from __future__ import annotations

import math
import struct
import zlib

FRAME_MAGIC = b"SPC1"
STATS_MAGIC = b"SPS1"
END_PREFIX = b"SPEC"  # "SPECEND ..." line
HEADER_BYTES = 28
CRC_BYTES = 4

# SPS1 body offsets
_SPS_BODY = 36


class FrameError(ValueError):
    pass


def _u16(b: bytes, at: int) -> int:
    return struct.unpack_from("<H", b, at)[0]


def _u32(b: bytes, at: int) -> int:
    return struct.unpack_from("<I", b, at)[0]


def _u64(b: bytes, at: int) -> int:
    return struct.unpack_from("<Q", b, at)[0]


def code_to_dbfs(code: float, nfft: int, full_scale: float = 512.0,
                 window_gain: float = 0.5) -> float:
    """Approximate dBFS from a power code.

    code/2 = 20*log10|X| and |X_peak| = A * nfft * window_gain for a tone of
    amplitude A (Hann coherent gain 0.5, 10-bit I/Q full scale = 512 counts).
    So dBFS(A) = code/2 - 20log10(full_scale) - 20log10(nfft * window_gain).

    code 0 is the quantized floor; the returned value is that floor's dBFS.
    """
    return code / 2.0 - 20.0 * math.log10(full_scale) - 20.0 * math.log10(nfft * window_gain)


def bin_frequency_mhz(bin_index: int, nfft: int, lo_mhz: float, sample_rate_hz: float) -> float:
    """Center frequency of natural-order FFT bin k. DC bin = LO."""
    bin_hz = sample_rate_hz / nfft
    k = bin_index - nfft if bin_index >= nfft // 2 else bin_index
    return lo_mhz + k * bin_hz / 1e6


class SpectrumFrame:
    __slots__ = ("seq", "first_sample", "samples", "ffts", "flags", "gain",
                 "drops", "nfft", "db_step", "codes")

    def __init__(self, seq, first_sample, samples, ffts, flags, gain, drops,
                 nfft, db_step, codes):
        self.seq = seq
        self.first_sample = first_sample
        self.samples = samples
        self.ffts = ffts
        self.flags = flags
        self.gain = gain
        self.drops = drops
        self.nfft = nfft
        self.db_step = db_step
        self.codes = codes

    @property
    def max_detector(self) -> bool:
        return bool(self.flags & 1)

    @property
    def empty(self) -> bool:
        return self.ffts == 0


def parse_frame(buf: bytes) -> SpectrumFrame:
    """Parse one complete SPC1 frame (magic..CRC inclusive). Raises FrameError."""
    if len(buf) < HEADER_BYTES + CRC_BYTES or buf[:4] != FRAME_MAGIC:
        raise FrameError(f"not an SPC1 frame ({len(buf)} bytes)")
    nfft = 1 << buf[26]
    expected = HEADER_BYTES + nfft + CRC_BYTES
    if len(buf) != expected:
        raise FrameError(f"bad frame length {len(buf)}, expected {expected}")
    crc = _u32(buf, len(buf) - CRC_BYTES)
    if zlib.crc32(buf[:-CRC_BYTES]) & 0xFFFFFFFF != crc:
        raise FrameError("frame CRC mismatch")
    codes = list(buf[HEADER_BYTES:HEADER_BYTES + nfft])
    return SpectrumFrame(
        seq=_u32(buf, 4),
        first_sample=_u64(buf, 8),
        samples=_u32(buf, 16),
        ffts=_u16(buf, 20),
        flags=buf[22],
        gain=buf[23],
        drops=_u16(buf, 24),
        nfft=nfft,
        db_step=buf[27],
        codes=codes,
    )


def parse_stats(buf: bytes) -> dict:
    """Parse one complete SPS1 record (magic..CRC inclusive). Raises FrameError."""
    if len(buf) != 40 or buf[:4] != STATS_MAGIC:
        raise FrameError("not an SPS1 record")
    crc = _u32(buf, len(buf) - CRC_BYTES)
    if zlib.crc32(buf[:-CRC_BYTES]) & 0xFFFFFFFF != crc:
        raise FrameError("stats CRC mismatch")
    return {
        "core0_load_pm": _u16(buf, 4),
        "core1_load_pm": _u16(buf, 6),
        "coverage_pm": _u16(buf, 8),
        "mode": _u16(buf, 10),
        "heap_free": _u32(buf, 12),
        "heap_largest": _u32(buf, 16),
        "abandoned": _u32(buf, 20),
        "dropped_frames": _u32(buf, 24),
        "late_max": _u16(buf, 28),
        "queue_fill_pm": _u16(buf, 30),
        "ffts_per_s": _u32(buf, 32),
    }


def find_peaks(codes: list[int], nfft: int, lo_mhz: float, sample_rate_hz: float,
               top: int = 8, min_delta: float = 10.0) -> dict:
    """Return floor + the strongest local-max bins of one frame.

    floor: 20th-percentile code (robust noise floor estimate).
    peaks: bins that are local maxima and >= floor + min_delta, strongest first.
    """
    ordered = sorted(codes)
    floor_code = ordered[max(0, int(0.2 * len(ordered)) - 1)]
    peaks = []
    for k, code in enumerate(codes):
        if code < floor_code + min_delta:
            continue
        left = codes[k - 1] if k > 0 else -1
        right = codes[k + 1] if k + 1 < nfft else -1
        if code >= left and code >= right:
            peaks.append({
                "bin": k,
                "freq_mhz": round(bin_frequency_mhz(k, nfft, lo_mhz, sample_rate_hz), 6),
                "code": code,
                "dbfs": round(code_to_dbfs(code, nfft), 2),
            })
    peaks.sort(key=lambda p: (-p["code"], p["bin"]))
    return {
        "floor_code": floor_code,
        "floor_dbfs": round(code_to_dbfs(floor_code, nfft), 2),
        "peaks": peaks[:top],
    }
