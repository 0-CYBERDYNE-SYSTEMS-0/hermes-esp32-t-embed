# Session logging and identity capture

The SDR app can record what it sees to its own microSD card, capture 802.11
frames with the chip's MAC, and hand any of it back over USB. This is the
operator guide; the wire formats live in `main/common/session_json.h` and
`main/common/pcap_ng.h`, both host-tested against real parsers.

## Card

The microSD slot is on SPI3: **CS 39, SCK 40, MOSI 41, MISO 38**. It is
unrelated to the LCD (SPI2) and to the encoder. Cards are mounted on first use
and retried every 5 s, so inserting one while the app runs works without a
reboot. FAT long filenames are enabled in `sdkconfig.defaults.t-embed` because
session files are named `frames.ndjson`, `session.json`, `*.sigmf-meta`.

```
/sdcard/sigint/<stamp>/
  session.json      build SHA, label, clock source, LO, rate, nfft, gain, filter
  summary.json      written on LOG STOP: frame/event/IQ counts, duration
  frames.ndjson     one spectrum frame per line
  events.ndjson     HUNT level transitions (WATCH and above)
  iq/*.sigmf-{data,meta}   one-press captures, SigMF v1.2.6, ci8
  net/wifi-*.pcapng 802.11 frames, radiotap (channel + dBm), Wireshark-ready
```

`<stamp>` is UTC `YYYYMMDD-HHMMSS` when the clock is known, and
`unsynced-<uptime>` otherwise. The manifest records which, as
`time_source: "host" | "unsynced"` — never claim an absolute time the board
cannot prove.

## Time

The board has no RTC. A host injects absolute time once:

```
TIME 1763500000123     -> OK TIME 1763500000123
TIME?                  -> TIME 1763500003438 host      (or "... unsynced")
```

Monotonic time keeps running either way; frames carry both `t_ms` (since
session start) and `ts` (ISO 8601 UTC, empty when unsynced).

## Commands

| Command | Effect |
| --- | --- |
| `SD?` | JSON: mounted, card/free KB, logging, session, frames, drops, events, IQ files, clock |
| `SD MOUNT` | Force a mount attempt now |
| `SD LS [path]` | Recursive listing (`DIR`/`FILE <path> <bytes>`) under `/sdcard/` |
| `SDFILE <path>` | `FILE <bytes>\n` + raw bytes + `END\n` — pull any card file over USB |
| `TIME <unix_ms>` / `TIME?` | Inject / read the wall clock |
| `LOG START [label]` | Open a session; frames are appended at the logging interval |
| `LOG STOP` | Close it, write `summary.json`, reply `OK LOG STOP <frames>` |
| `LOGRATE <ms>` | Minimum gap between logged frames (default 200, range 20–60000) |
| `LOGREC [samples] [rate] [label]` | One-shot IQ capture straight to SigMF; `rate` 0=80, 1=40, 6=16 MS/s |
| `WIFI SNIFF START [ch]` | Capture 802.11 frames from the MAC into `net/*.pcapng` |
| `WIFI SNIFF STOP` | Stop capturing and flush |
| `WIFI?` | JSON: channel, seen/stored/dropped, mgmt/ctrl/data counts, beacons, probes, file |

`LOGREC` defaults to 16 380 samples at 16 MS/s (~1.02 ms) because that is the
longest window the one-shot buffer allows and it covers a BLE advertising
packet or a short 802.11 frame. `WIFI SNIFF` uses the tuned frequency when it
is a Wi-Fi channel, otherwise channel 6.

## Why the MAC, not demodulated IQ

The chip already contains an 802.11 MAC and a BLE controller. Demodulating
2.4 GHz frames out of raw IQ to recover an SSID is strictly harder and yields
strictly less than asking the MAC for the frame it just decoded. Raw IQ is
kept for what only raw IQ can do: measuring the physical layer — spectrum
shape, duty cycle, non-802.11 emitters, interference.

## Limits worth remembering

- **Receive chain:** the S3 PLL locks 2196–2806 MHz (measured, see
  `main/targets/esp32s3/receiver.c`). Everything outside that is a tuning
  attempt, not reception.
- **IQ window:** one-shot 16 380 samples = 205 µs @80 MS/s, 1.02 ms @16 MS/s;
  the ring adds up to three banks (~3 ms @16 MS/s). Long 802.11 frames do not
  fit; short ones do.
- **Sniffer vs raw capture:** the raw dump path takes over the PHY. Whether
  the MAC still decodes while a `SPEC` run is active is measured per build by
  `WIFI?`'s `seen`/`dropped` counters — if `seen` stops rising during
  capture windows, schedule the two modalities instead of running them
  together.
- **Uncalibrated:** codes are not dBm. Relative comparisons are trustworthy;
  absolute levels are not, until a calibration table exists.
- **One client:** the serial lease is unchanged (5 s idle release,
  `RELEASE` to hand over).
