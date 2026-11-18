# From "SDR demo" to a field SIGINT instrument

Planning doc, 2026-11-18. Grounds every claim in this tree or in a fetched
upstream source; anything unverified is labelled as a lead.

## 1. What the hardware actually is

| Property | Value | Evidence |
| --- | --- | --- |
| RF the PLL will lock | **2196–2806 MHz** (measured VSG60 sweep 0.15–6 GHz) | `main/targets/esp32s3/receiver.c:58` |
| Advertised range | 100–6000 MHz, 1 MHz steps, "attempt limits, not validated" | `receiver.c:404` (`RANGE 100 6000 1`), `README.md:186` |
| Sample rates | 80 / 40 / 16 MS/s (codes 0/1/6) | `receiver.c:255`, `burst_limits.h` |
| Analog bandwidth | 13–69 MHz LPF, 6-bit DCAP filter | `README.md:198`, `receiver.c:76-84` |
| Max raw IQ, one shot | 16 380 complex samples = 205 µs @80, 410 µs @40, 1.02 ms @16 MS/s | `receiver.c:36`, `capture()` |
| Max raw IQ, ring | 3 banks × 16 384 pairs ≈ 0.61 ms @80 / 1.23 ms @40 / **3.07 ms @16 MS/s** | `ring_capture.h:33-40`, `RINGCAP` |
| Continuous spectrum | 256–2048 bins @80/40/16 MS/s, stride, up to 24 h, on-chip FFT | `SPEC` / `SPECINFO`, `ring_capture.c` |
| Transport | USB Serial/JTAG (native) + UART0; one client at a time, 5 s lease | `receiver.c:334`, `burst_serial` |
| Local storage | **microSD slot on SPI — CS 39 / SCK 40 / MOSI 41 / MISO 38 — unused by this firmware** | LILYGO `examples/factory/pin_config.h`; `grep -r sdmmc main/` → nothing |
| Battery sense | GPIO4 ADC (unused) | same |
| LEDs | APA102 on 45/42 | `t_embed_ui.c:31-32` |
| Free I2C | GPIO18/8 (Grove) — includes ES7210 mic in the Hermes app | `t_embed_audio_input.c:21-22` |
| TX | RX only; no modulator, no frame injection | `README.md:54` |
| Wi-Fi driver state in SDR mode | **initialised + promiscuous ON, but no sniffer callback registered** | `receiver.c:428-435` |
| In-RAM HUNT log | 12 events, lost at restart (Home == `esp_restart()`) | `t_embed_ui.c:141`, `AGENTS.md` |

Consequence: **this is a 2.4 GHz burst instrument, not a 100 MHz–6 GHz
scanner.** The rtl_433 / ADS-B / POCSAG / 868 MHz LoRa ecosystem is
physically out of reach — those need a 24–1700 MHz front end (RTL-SDR,
HackRF). Any roadmap that assumes otherwise is fantasy.

What only this device can do at 2.4 GHz is worth a lot:

- 80 MS/s complex baseband = full 802.11 20/40/80 MHz channel visibility to
  the sample level, with gapless 3 ms windows.
- Continuous 256–2048-bin spectra for 24 h (`SPEC` accepts `duration_ms` up to
  86 400 000; real runtime then depends on the pack and the radio's duty cycle).
- A display, a knob, and a battery: it works with a phone in airplane mode,
  no laptop, no OS, no app.

## 2. What is already good (do not rebuild)

1. **Continuous FFT engine** (`SPEC`): the hard part — bank rotation, S3 SIMD
   FFT, dual-core assist, drop/late telemetry. 24 h runs already work.
2. **HUNT triage** (`main/common/threat.h`): baseline-relative, duty-cycle and
   periodicity rules, EMA needle, trend arrow, threat levels, mirrored
   host-side in `host/rfkit` so the two cannot drift silently.
3. **Ring capture** (`RINGCAP`): gapless raw IQ larger than the one-shot buffer.
4. **Host toolkit** (`host/rfkit`): timestamped JSONL, spike tables, HTML
   report, session diffing ("which signals followed me").
5. **Field UI**: HUNT / TRACK / COMPARE / SPECTRUM / LOG / INFO, one-press view
   cycle, machine-readable CLI for every screen.

The gaps below are all additive; none require rewriting the acquisition path.

## 3. The gaps that change what the tool is

### G1 — The device is amnesiac (highest value, lowest risk)

Today a walk produces: a 12-entry RAM event log, an IQ buffer that only exists
if a laptop is plugged in, and no wall-clock time, no location, no battery, no
antenna note. Everything dies at restart.

Do:

- **SD logging task** (SPI, its own core, PSRAM ring → 4 KB cluster writes).
  Constraint: never block the SDR owner task; single-owner rule from AGENTS.md.
- **Two on-card formats, no invention needed:**
  - IQ → **SigMF** (`x.sigmf-data` + `x.sigmf-meta`): IQ8 is signed 8-bit
    interleaved I/Q = `ci8`; packed IQ10 becomes a non-conforming dataset with
    `core:datatype: "ci16_le"` + `esp_sdr:packing: "iq10"`. SigMF v1.2.6 core
    already carries `sample_rate`, `center_frequency`, `gain`, `antenna:*`, and
    per-segment `captures[].frequency` — exactly the retune gaps we have.
  - Events + spectra → NDJSON, one object per line (same shape rfkit writes:
    `ts`, `freq_khz`, `peak_codes`, `floor`, `codes[]`), so `rfkit analyze`
    works on card files unchanged.
- **Wall clock**: `TIME <unix_ms>` CLI command for the host/phone to inject on
  connect, plus NTP when Wi-Fi is up. No RTC on the board, so the manifest
  must record `time_source: "host|ntp|unsynced"`.
- **Session manifest** (`session.json`): firmware git SHA (already printed by
  INFO screen), band, gain index, LPF, rate, antenna, battery mV, calibration
  table version. Without this a capture is not defensible later.

Acceptance: walk 10 min, pull the card, `sigmf-validate` passes, every IQ file
opens in `inspectrum`, `rfkit analyze` runs on the NDJSON with no changes.

### G2 — It measures energy, not identity ("every bit of information")

The chip has a complete 802.11 MAC and BLE 5 controller. Demodulating Wi-Fi
out of IQ when silicon already does it is wasted effort — and IQ gives you
less information than the MAC does.

Do:

- **802.11 sniffer → PCAP.** The driver is already started and
  `esp_wifi_set_promiscuous(true)` is already called (`receiver.c:435`); what
  is missing is `esp_wifi_set_promiscuous_rx_cb()` + channel hopping + a
  PCAPNG writer. That yields beacons (SSID/BSSID/RSN/capabilities), probe
  requests (client MACs + their IE fingerprint = device class), data frame
  lengths, RSSI per frame. Straight into Wireshark. This is a step change:
  "there is energy at 2437" becomes "an iPhone probed for 6 SSIDs from this
  MAC, here is its vendor".
- **BLE advertisement log** (NimBLE scan): MAC + OUI → vendor (offline table in
  PSRAM), iBeacon/Eddystone, and tracker patterns — Apple Find My
  (`4C 00 12 19`), Tile, Samsung SmartTag, Chipolo. This is the feature the
  HUNT screen is *trying* to be: it finds the AirTag, names it, and points at
  it, instead of flagging a 2.4 GHz energy blob.
- **Open question to spike first**: does the MAC still decode frames while
  `adctrig` owns the PHY (raw IQ dump)? If not, schedule alternation —
  modality windows (e.g. 500 ms SDR sweep / 500 ms MAC sniff) with the
  timeline stitched in software. Everything downstream must tolerate windows.

Acceptance: `tcpdump -r session.pcapng` shows beacons and probe requests from
a 5-minute walk; the AirTag (or any BLE tracker) appears with MAC + vendor;
the same session's spectrum frames still line up on one timeline.

### G3 — There is no backhaul (the dashboard you actually want)

Current reality: USB cable to a phone, airplane mode, laptop required for
`rfkit`. That is a recording rig, not a tool.

Do, in this order:

1. **Store-and-forward on SD** (G1). Nothing blocks on a network.
2. **Device as its own AP + captive dashboard.** Phone joins
   `TEMBED-SIGINT`, opens `http://192.168.4.1`: live waterfall (downsampled
   PNG/WebP or sparse bin deltas over WebSocket), HUNT table, event list,
   "download session bundle" (zip of NDJSON + SigMF + pcapng + manifest).
   Works with airplane mode ON and Wi-Fi ON — no app store, no cable, any
   iOS/Android device.
   - Constraint: Wi-Fi TX/RX and the raw dump share the PHY. Serve during
     capture gaps or time-slice; do not promise live 30 fps waterfall while
     the FFT engine runs at 80 MS/s.
3. **Opportunistic uplink**: on a known SSID (home/lab/tailnet), POST finished
   sessions to `rfkit serve` (or Tailscale). Queue drains in the background;
   the device never holds a session hostage to network state.
4. **Host dashboard upgrade** on top of `rfkit report`: Leaflet map for
   geo-tagged events, session diffing UI, and IQ playback through the decode
   ladder.

Acceptance: phone joins the AP mid-HUNT, sees live rows within 2 s, downloads
the bundle; on returning home the same session lands on the laptop with zero
manual steps.

### G4 — The numbers are not measurements

`code/2 − 20log10(512) − …` is a relative scale; gain and frequency are
uncalibrated, and there is no TCXO. This is what separates a toy from an
instrument, and you already own the reference: a VSG60 was used for the
0.15–6 GHz sweep.

Do:

- **Amplitude calibration**: VSG60 CW at N levels × every gain index × every
  LPF setting × 3 rates → correction table (per-band offset + slope). Emit
  dBm alongside raw codes; keep raw codes in the log so old data stays usable.
- **Frequency error**: measure LO offset against a known carrier and store a
  correction; record ppm drift vs temperature if you care.
- **State the resolution honestly**: 1 MHz tuning granularity; peak frequency
  from FFT bins is finer, but PLL step + no calibration means ±kHz-class
  claims are unsupported. Put this in `INFO`/`LIMITS?` so the host can label
  every measurement with its uncertainty.

Acceptance: a known −60 dBm carrier reads within ±2 dB across all gain
indices; the UI/log labels every value with its calibration version.

### G5 — On-device decisions (make the walk self-explaining)

- **Signature library**: classify each tracked peak (OFDM-like, GFSK burst,
  chirp, analog FM/video, CW, pulsed) from envelope + spectral shape, and name
  it. Unknown emitters get `UNKNOWN-<hash>` and are the interesting ones.
- **One-press IQ record**: knob press on a tracked signal → 3 ms ring capture
  → SD as SigMF, named after the signature. This is the "evidence" button.
- **Geiger audio**: beep rate/pitch ∝ proximity on the tuned peak, so a
  directional antenna turns the device into a body-worn locator. Cheap, and it
  is the single most "instrument-feeling" feature available.
- **Named session + marks**: start/stop a session, knob hold = timestamped mark
  with a short note, so a walk becomes a report instead of a log dump.
- **Battery/RSSI in the UI** (GPIO4 ADC) — a field tool that dies mid-walk is a
  field tool you stop trusting.

### G6 — The modality the pasted research is actually about: CSI

The referenced projects (`ruvnet/RuView`, `MaliosDark/wifi-3d-fusion`,
`francescopace/espectre`, `Neumi/3D_wifi_scanner`, `StevenMHernandez/ESP32-CSI-Tool`,
`espressif/esp-csi`) are not SDR projects. They use **per-subcarrier channel
state** from decoded Wi-Fi frames. Different sensing mode, same silicon:

- `espressif/esp-csi` states CSI support on **all** ESP32 series including S3,
  ships `esp_wifi_sensing` (presence/motion, on-site training) and a browser
  Web Serial monitor. `ESP32-CSI-Tool` already demonstrates SD-card logging of
  CSI — the exact pattern G1 needs.
- What CSI buys: presence, motion, coarse ranging, breathing at short range,
  and through-wall detection of *movement*. At 12.5 cm wavelength you get
  metres-scale sensitivity to change, not geometry.
- What it does not buy: 3D point clouds, 17-keypoint pose, furniture geometry.
  RuView-class demos are not independently validated tools; treat them as
  research. If real 3D geometry is wanted, that is a **different sensor**
  (VL53L5CX ToF + IMU over the Grove I2C), not a firmware feature of this radio.

Recommendation: add CSI as a **fourth modality** with its own log channel and
its own view, fusing three independent "is someone there" signals —
SDR energy, MAC/BLE identity, CSI motion — which together are far more
convincing than any one of them alone.

## 4. What to deliberately not build

- Sub-GHz / 5 GHz hunting on this board. Hard PLL edges at 2196/2806 MHz.
- Wi-Fi frame *injection* / replay / jamming. Not implemented, and it is the
  one feature that turns a receiver into a legal problem. (The chip does expose
  a TX tone path; leave it alone.)
- 30 fps live waterfall to a phone while the FFT engine owns the radio.
- A parallel "3D room reconstruction" feature in this firmware.

## 5. Phased plan with acceptance tests

| Phase | Scope | Acceptance (real, not compile) | Risk |
| --- | --- | --- | --- |
| **P0** | SD logging task, SigMF + NDJSON, `TIME` injection, `session.json` manifest, battery, persistent HUNT events | 10-min walk → card parses in `sigmf-validate` + `inspectrum` + `rfkit analyze`; nothing lost on restart | SRAM guard: allocate in PSRAM, keep `_bss_end` margin |
| **P1** | dBm + frequency-error calibration table (VSG60), uncertainty labels in `INFO`/`LIMITS?`/logs | −60 dBm reference reads ±2 dB across gain indices | time on the bench |
| **P2** | 802.11 promiscuous → PCAPNG; NimBLE scan → JSONL with OUI + tracker patterns | Wireshark shows beacons/probes; a real BLE tracker is named with MAC+vendor | PHY contention with `adctrig` — spike first |
| **P3** | Device AP + captive dashboard + bundle download | phone on the AP sees live rows within 2 s mid-HUNT | Wi-Fi TX vs raw dump scheduling |
| **P4** | Opportunistic uplink + `rfkit serve` live view + map + session diff | session auto-lands on laptop after a walk, zero manual steps | none serious |
| **P5** | Host-side decode ladder on recorded SigMF (`gr-ieee802-11`, `gr-ieee802-15-4`, URH, inspectrum), promote winners on-device; CSI mode | a captured Zigbee/802.11 burst decodes to frames from the card; CSI mode reports presence/motion | 3 ms window caps what is decodable |

Order matters: **P0 and P2 are what make the device produce knowledge instead
of light.** P3 is what makes it usable without a laptop. P1 is what makes it
credible. Everything else is polish on top of those four.

## 6. Reference stack (verified to exist, 2026-11-18)

| Project | Why it matters here |
| --- | --- |
| `ESPARGOS/esp-sdr` | upstream this firmware derives from |
| `host/rfkit` (this repo) | already the logging/analysis spine |
| `sigmf.org` v1.2.6 | IQ + metadata interchange; opens in many tools |
| `IQEngine/IQEngine` | browser analysis/processing of SigMF recordings |
| `jopohl/urh` | reverse-engineer unknown 2.4 GHz protocols from IQ |
| `bastibl/gr-ieee802-11` | 802.11 demod from raw IQ (if MAC path is unavailable) |
| `bastibl/gr-ieee802-15-4` | Zigbee/802.15.4 demod from raw IQ |
| `merbanan/rtl_433` | **not applicable** (sub-GHz front end needed) — listed so it stops being suggested |
| `espressif/esp-csi`, `StevenMHernandez/ESP32-CSI-Tool` | CSI sensing + SD-logging pattern |
| `luarvique/openwebrx` | reference for a good multi-view web SDR UI |
