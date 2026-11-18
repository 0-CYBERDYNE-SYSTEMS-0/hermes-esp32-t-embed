# host/rfkit — logging + spike analysis for the LILYGO T-Embed ESP-SDR

Host-side toolkit for the `esp_sdr` firmware (this repo, `host/` companion to
`main/`) over its USB serial protocol. Turns the raw SPC1 spectrum stream into
timestamped logs, top-spike tables, and an HTML report.

## Device state

The device runs the `t-embed` launcher build from this repo's CI
(`firmware-t-embed` artifact): knob-driven launcher (ESP-SDR / USB MICROPHONE /
HERMES). ESP-SDR opens on the HUNT screen; knob press cycles views (HUNT /
TRACK / COMPARE / SPECTRUM / LOG / INFO), hold opens the menu. The USB serial
CLI answers regardless of what's on glass (`HUNT?`, `LOG?`, `BASELINE
SAVE|CLEAR`). The INFO screen shows the exact git SHA of the running build.

Rollback: flash `../tembed-work/restore-6831eb2-20261102.bin` (or
`restore-t-embed-a44824e-20261026.bin`) at 0x0 with esptool; CI artifacts carry
a `manifest.json` with per-part sha256 for verification.

## Usage

```sh
python3 rfkit.py info                          # device + capability snapshot
python3 rfkit.py capture --seconds 60 --center 2437 --stats
python3 rfkit.py sweep --start 300 --stop 2700 --step-mhz 15 --dwell-ms 250
python3 rfkit.py analyze logs/capture_<stamp>  # top spikes + classification
python3 rfkit.py report  logs/capture_<stamp>  # self-contained report.html
```

Session layout: `logs/<kind>_<stamp>/` with `meta.json`, `frames.jsonl`
(per frame: ISO timestamp `ts`, relative `t_s`, peak list, noise floor, full
256-bin `codes`), `stats.jsonl` (SPS1 telemetry), `summary.json`,
`report.json`, `report.html`.

## Signal classification

Every tracked spike gets a duty cycle (fraction of frames where its bin is
≥ per-frame noise floor + threshold):

- **STEADY** (≥ 0.8) — always-on carrier: hidden audio/video transmitters,
  analog video sends, dedicated trackers.
- **BURSTY** (0.1–0.8) — periodic or handshake traffic: Wi-Fi, BLE, sensors.
- **SPORADIC** (< 0.1) — rare events.

Compare two sessions to see which signals follow you: a signal present at
every location is on your person or vehicle.

## Accuracy notes

- dBFS is approximate: `code/2 − 20log10(512) − 20log10(nfft·0.5)` (Hann
  coherent gain, 10-bit I/Q full scale). Code 0 is a quantized floor, not
  "no RF". Relative comparisons are reliable; absolute levels are indicative.
- Tuning range is 100–6000 MHz per `RANGE?`, but the firmware docs state RF
  accuracy/PLL lock are unverified outside 2100–2800 MHz. Treat sub-GHz and
  >3 GHz findings as leads to confirm, not calibrated measurements.

## Tests

```sh
python3 -m unittest test_rfkit -v   # 16 tests: decode, CRC, capture, analyze, report, threat rules
```

FakePort feeds a synthetic SPC1/SPS1 stream through the full pipeline —
including the report renderer — so the logging path is verified without
hardware.
