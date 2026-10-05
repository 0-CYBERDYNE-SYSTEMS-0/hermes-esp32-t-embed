# T-Embed three-app firmware specification

## Purpose

The radio sections below describe the SDR app only. The three-app section defines the combined launcher, Hermes networking/audio, and the revised one-knob controls; its control design supersedes the earlier SDR long-press mapping.

Turn the connected **standard LILYGO T-Embed without the CC1101 shield** into a self-contained, receive-only 2.4 GHz spectrum viewer. Reuse ESPARGOS ESP-SDR's ESP32-S3 raw receiver and on-chip FFT path, then render live FFT rows as a waterfall on the built-in LCD. The rotary encoder tunes the view. A USB serial diagnostic remains available for build and acceptance checks.

The target is the board currently connected on `/dev/cu.usbmodem1101`: ESP32-S3 QFN56 revision 0.2, 40 MHz crystal, 8 MB embedded PSRAM, 16 MB quad flash. These observed details identify this unit; the firmware should still build for the ESP32-S3 profile and detect capabilities at runtime.

## Radio scope and source

- Base project: [ESPARGOS/esp-sdr](https://github.com/ESPARGOS/esp-sdr), ESP32-S3 profile, pinned ESP-IDF commit `25fe69f946311abdaf9ad56591f25fedbc20ac98` from `firmware-targets.json`.
- ESP-SDR uses the S3's integrated 2.4 GHz Wi-Fi receiver and its undocumented I/Q capture path. No CC1101 or added RF parts are part of this build.
- Use ESP-SDR's **on-chip capture and FFT reducer** as the actual display input. For the on-device UI, run it in bounded capture windows and publish completed spectra through the existing local latest-frame mailbox; do not create synthetic, demo, or animated substitute samples. Prefer 1024 bins as the initial setting, with supported values 256–2048 selectable/configurable after validation.
- The S3 supports continuous RF capture with on-chip FFT at nominal 16/40/80 MS/s according to ESP-SDR's current capability table. The firmware must query or use the compiled S3 capability definitions rather than assuming all chips or rates behave alike. S3 snapshot FFT is not advertised.
- Initial user-visible tune range: 2400–2483 MHz, in 1 MHz steps, centered on 2437 MHz at first boot. ESP-SDR exposes broader software tune attempts, but those endpoints are not validated RF performance. Keep the UI in the Wi-Fi/ISM band for v1 and label the shown span as nominal.
- Reception only. No transmit, replay, packet injection, deauthentication, or control of Wi-Fi connections.
- Gain and displayed levels are relative and uncalibrated. Use the device's supported default AGC unless manual gain is implemented with supported advertised controls.

## Board and LCD interface

Use the **standard T-Embed pinout**, not the CC1101 T-Embed variant:

| Signal | ESP32-S3 GPIO | Notes |
| --- | ---: | --- |
| TFT MOSI | 11 | ST7789 SPI data; write-only panel |
| TFT SCLK | 12 | ST7789 SPI clock |
| TFT CS | 10 | Panel chip select |
| TFT DC | 13 | Data/command |
| TFT RST | 9 | Panel reset |
| TFT backlight | 15 | Active HIGH |
| Main power enable | 46 | Drive HIGH during initialization |
| Encoder A | 2 | Rotary input |
| Encoder B | 1 | Rotary input |
| Encoder push / BOOT | 0 | Active LOW; preserve GPIO0 boot strapping behavior |
| Speaker DIN | 6 | MAX98357A audio output; not an application button |

The panel is ST7789, 170 × 320 in native portrait coordinates and 320 × 170 in landscape. LILYGO's official TFT_eSPI setup specifies RGB order, inversion ON, `TFT_MISO=-1`, and a 40 MHz SPI clock. Its factory example calls `setRotation(3)` and applies a board-specific ST7789 command table after `begin()`. Although `Setup210_LilyGo_T_Embed_S3.h` does not explicitly define `CGRAM_OFFSET`, the library's `ST7789_Defines.h` enables it for 170 × 320 dimensions, and `ST7789_Rotation.h` selects row start 35 for rotation 3. A native driver matching that official rotation path should apply a 35-pixel row offset (column offset 0) when addressing the visible 320 × 170 area. Confirm framebuffer bounds and alignment on the actual unit before calling the panel verified.

The official factory example's extra ST7789 sequence is `11` (then 120 ms delay), `3A 05`, `B2 0B 0B 00 33 33`, `B7 75`, `BB 28`, `C0 2C`, `C2 01`, `C3 1F`, `C6 13`, `D0 A7`, `D0 A4 A1`, `D6 A1`, `E0 F0 05 0A 06 06 03 2B 32 43 36 11 10 2B 32`, `E1 F0 08 0C 0B 09 24 2B 22 43 38 15 16 2F 37`. Validate this against the exact official source when implementing a native IDF panel driver.

### Display composition

Use landscape 320 × 170 RGB565. The focused Tokyo Night layout reserves a 24-pixel header, a 29-pixel spectrum trace, an 8-pixel frequency axis, a 97-row waterfall and a 12-pixel footer:

- Header: compact ESP-SDR label, prominent center frequency, sample-rate span, frequency step, and LIVE/PAUSE state. Omit control hints to give the live display more room.
- Spectrum trace: 320 columns by 29 rows.
- Axis: left/right nominal frequency labels for the displayed span.
- Waterfall: 320 columns by 97 rows. Each row is derived from one completed ESP-SDR FFT result and added to the waterfall history. Shift natural-order FFT output by half its bin count so increasing frequencies run left to right around the tuned center. When reducing more than 320 bins, pool bins per output pixel so narrow peaks do not disappear between selected columns.
- Footer: compact relative power color key; keep levels explicitly uncalibrated.

Use a dark navy background, muted indigo grid and axis labels, soft lavender primary text, and a cyan spectrum trace. Map magnitude in dB or the ESP-SDR's documented relative FFT scale through a monotonic waterfall palette from deep blue through violet and teal to restrained amber at the strongest signals. Keep most of the screen dark and reserve brighter colors for the trace, live state and signal peaks. Apply bounded smoothing only if it does not hide that fresh captures are arriving. Keep the scale fixed or expose its min/max; never present uncalibrated values as dBm.

Keep the 320 × 97 RGB565 FFT history buffer in detected octal PSRAM when enabled, with an internal-memory fallback, and reserve internal DMA-capable memory for the active transfer strips. Avoid a full-screen repaint for every capture unless measured performance remains within target. The single application loop should serialize the bounded RF runs, mailbox read, and panel draw so no extra task accesses the radio ring, USB transport, or SPI panel concurrently.

### Controls

- Turn encoder: adjust center frequency; default one detent equals 1 MHz. Clamp at the v1 band edges. Show the updated frequency immediately.
- Short press encoder: cycle frequency step (1, 5, 10 MHz).
- Long press encoder: pause/resume RF capture and waterfall rows; show PAUSED.
- Span/sample rate: select supported 16, 40 and 80 MS/s settings through the knob menu. GPIO6 is speaker DIN, not a documented user button.
- Apply stable-state debounce to the knob switch; decode detents using the official TWO03 latch convention. Bound all frequency arithmetic before tuning.
- GPIO0 is both the encoder push and BOOT strap. Do not drive it as an output; a held low during reset can enter the ROM download mode.

## Data path and task ownership

1. The single application loop initializes ESP-SDR's Wi-Fi PHY and owns radio tuning, USB/UART command polling, and the T-Embed UI. Preserve the existing command lease for host commands.
2. On each due interval, run a bounded `RING_MODE_SPEC` window with `local_output=true`. Keep the S3 SRAM linker reservations, capture-ring synchronization, interrupt timing and core-1 DSP ownership intact.
3. After capture returns and interrupts are restored, read the latest completed spectrum mailbox, increment a monotonic capture-frame counter, map the natural-order bins to one 320-pixel row, and append it to the PSRAM history.
4. The same application loop owns ST7789 SPI2 and GPIO input reads. Do not add another task that can concurrently access the radio ring, USB transport, or panel.
5. Keep USB diagnostic command ownership explicit. Provide a read-only `UI?` command that reports center frequency, span/rate, FFT size, cumulative RF frames/FFTs, successfully rendered screen rows, capture duration, late/dropped/error counts, and latest spectrum min/max/checksum. Use the existing `INFO`/`CAPS` responses for target identity and capabilities. This lets validation distinguish real capture-driven redraws from animation without a camera.

The firmware should expose read-only USB status suitable for acceptance checks, reporting at least: target via `INFO`, active center frequency and span, FFT size/rate, cumulative capture/FFT frames received, successfully rendered rows, latest-frame age, and radio/UI error counters. The display should show the live/pause state and an uncalibrated relative-power key; keep detailed counters on the USB status path. This lets validation distinguish real RF-driven redraws from an animation without camera access.

## Build and installation

- Build only the ESP32-S3 profile with the pinned ESP-IDF commit from the repository's target manifest. Keep the environment under `sandbox/t-embed-toolchain`; do not use the system ESP-IDF or change ESP-SDR's pinned revision.
- Preserve ESP-SDR's partition layout, capture-SRAM linker guard, and IDF configuration unless the measured chip/flash profile requires an explicit, reviewed adjustment. The connected unit has 16 MB flash; ESP-SDR's defaults currently state 2 MB and its partition CSV defines a 4 MB app region, so check the final generated flash layout and image sizes before flashing. Do not erase flash as part of an ordinary update.
- Enable and verify octal PSRAM only if the display history uses it. The connected chip reports 8 MB embedded PSRAM, and LILYGO's factory example requires OPI PSRAM for its build.
- Before the first firmware write, save and hash a full 16 MB readback of the original flash. Record the chip MAC, flash ID/size, ESP-IDF revision, source revision, image hashes, and recovery instructions alongside the build artifacts.
- Build and inspect the merged image locally. Flashing is a separate final hardware action; after flash, query `INFO`, `CAPS`, `LIMITS?`, and the read-only UI/radio status command.

## Acceptance criteria

1. **Build:** ESP32-S3 firmware compiles with the exact pinned SDK. Image and partition sizes fit the verified 16 MB device map. No SDK or firmware files are silently fetched from a floating branch during the build.
2. **Hardware identity and recovery:** preflash backup is exactly 16,777,216 bytes and has a recorded SHA-256. The device's S3 revision, MAC and 16 MB flash ID match the preflash log.
3. **Display:** the screen initializes at 320 × 170 landscape, has correct orientation/color/inversion and no clipped edges, and displays current status plus frequency labels. The entire waterfall area remains inside panel bounds.
4. **Real data:** the latest-spectrum sequence and rendered-row counters increase together during reception; no synthetic fallback is compiled into the normal view. With a quiet RF environment, noise-floor variation still produces nonconstant captured bin values. A nearby known 2.4 GHz Wi-Fi source should create repeatable channel energy at the expected nominal offset after frequency-order correction. Record the source and measured bins/behavior.
5. **Live rendering:** for at least 60 seconds, captured frames continue and the on-device history advances at a measured median of at least 5 rows/second. Display updates do not stall the capture consumer or produce watchdog resets. Report dropped/overrun/error counters.
6. **Controls:** knob rotation changes frequency monotonically within 2400–2483 MHz; the initial center is 2437 MHz; step cycling works; the knob menu selects the three supported sample-rate/span settings; pause/resume state is visible. Pressing/releasing GPIO0 outside reset does not reboot; holding it while reset produces the expected download-mode behavior.
7. **USB:** native USB remains present for `INFO`/capability/status queries and capture diagnostics; the LCD and spectrum loop do not steal or corrupt USB serial/JTAG traffic.
8. **Recovery:** document the verified manual download procedure (encoder center/BOOT GPIO0 held, press/release RST, release BOOT) and a command using the matching image at offset(s), but do not erase or restore the preflash backup during acceptance.

## Known limits

This device remains a low-cost experimental 2.4 GHz receiver with an undocumented sampling path. The nominal band labels and FFT x-axis are not calibrated measurement instruments. Captures are discontinuous/selected windows, so short bursts can be missed. Antenna response, gain, sample clock, receiver linearity, and performance outside the vendor Wi-Fi band are not validated by a working UI.

## Primary references

- [ESP-SDR source and current S3 feature table](https://github.com/ESPARGOS/esp-sdr)
- [ESP-SDR S3 SDK revision manifest](https://raw.githubusercontent.com/ESPARGOS/esp-sdr/main/firmware-targets.json)
- [LILYGO standard T-Embed pin map](https://raw.githubusercontent.com/Xinyuan-LilyGO/T-Embed/master/examples/factory/pin_config.h)
- [LILYGO standard T-Embed display setup](https://raw.githubusercontent.com/Xinyuan-LilyGO/T-Embed/master/lib/TFT_eSPI/User_Setups/Setup210_LilyGo_T_Embed_S3.h)
- [LILYGO T-Embed factory display initialization and orientation](https://raw.githubusercontent.com/Xinyuan-LilyGO/T-Embed/master/examples/factory/factory.ino)
- [LILYGO T-Embed official repository](https://github.com/Xinyuan-LilyGO/T-Embed)
- [LILYGO TFT_eSPI ST7789 offset and color definitions](https://raw.githubusercontent.com/Xinyuan-LilyGO/T-Embed/master/lib/TFT_eSPI/TFT_Drivers/ST7789_Defines.h)
- [LILYGO TFT_eSPI ST7789 rotation offsets](https://raw.githubusercontent.com/Xinyuan-LilyGO/T-Embed/master/lib/TFT_eSPI/TFT_Drivers/ST7789_Rotation.h)


## Three-app scope and evidence (2026-10-04)

Provide one boot launcher containing **ESP-SDR**, **USB Microphone**, and **Hermes**. Only the selected app owns runtime resources. This is a source integration into this repository, not installation of three upstream binaries. Existing uncommitted microphone/UI edits must be preserved. This section supersedes older control assumptions and describes the next implementation; it does not assert that the combined firmware already works.

Reference checkout: `/path/to/local/Dev/hermes-gadget-sdk`, commit `edecb81c22f1c9308338735b0613be3d341e94ac`. Repository baseline: `87d7de2e0777e7e71b37702cc17c34c7c0e042b5`, plus current working edits. Pin imported SDK source and dependency versions; builds must not depend on the sibling checkout or floating upstream branches. Retain MIT notices and applicable third-party notices.

### Exact hardware and confidence

Target the **standard LILYGO T-Embed ESP32-S3R8**, not T-Embed CC1101, Touch, or a Waveshare board. The recorded read-only hardware identification in `validation/hardware-and-toolchain.md` reports revision 0.2, 40 MHz crystal, 8 MB embedded PSRAM, 16 MB external flash, MAC `<DEVICE_MAC>`. `/dev/cu.usbmodem1101` is currently present, but port names are not identity: recheck chip/MAC before flashing. No fresh chip query or device write was performed for this design review.

LILYGO's schematic (2022-11-02, PDF pages 1, 4, 6, 7) and factory pin map show:

| Resource | Wiring / implementation requirement |
| --- | --- |
| Rotary encoder | A GPIO2, B GPIO1; factory `LatchMode::TWO03`. Retain PCNT decoding; verify polarity and physical detent ratio on this unit. |
| Knob push / BOOT | GPIO0, active low. The only documented application push switch. Never drive it; holding during reset invokes download mode. |
| Reset | Hardware EN/reset switch; restarts the chip. Cannot serve as logical Cancel or approval input. |
| Display | Non-touch ST7789, 320 × 170 landscape; retain the documented initialization, 35-row offset, RGB/inversion and existing SPI wiring. |
| Microphones | ES7210 ADC, I2C SDA18/SCL8, address 0x40; I2S MCLK48/BCLK47/WS21/DIN14. Use existing two-channel capture and bounded stereo-to-mono conversion. |
| Speaker | MAX98357A, separate I2S BCLK7/WS5/DOUT6; amplifier SD_MODE follows its schematic wiring. Check actual speaker attachment and audible output before declaring voice-out working. |
| LEDs / power | APA102 data42/clock45; main power enable46. Retain board power sequencing. |
| USB | Native USB GPIO19/20; retain serial/JTAG console and recovery. |

**Correction required in implementation:** current `PIN_USER_KEY GPIO6`, button ISR, span/exit handling, and “HOLD SIDE BUTTON” hint contradict the standard schematic. Remove GPIO6 button ownership in the T-Embed profile before enabling speaker playback. The previous validation record documents prior assumptions; it does not establish an additional physical switch. If this particular board is modified, establish that separately rather than assuming a variant.

### Control design: one knob, no extra buttons

One debounced input dispatcher owns GPIO0 and encoder events. Interpret inputs using the context at press start. Require release after entering a mode, menu, or prompt; suppress stale edges and any delayed short click following a long press. Proposed timing: 30 ms debounce and 300 ms hold recognition. These are design values to test, not manufacturer specifications.

| Context | Rotate | Short click, processed on release | Hold |
| --- | --- | --- | --- |
| Launcher | Select one of three rows; CW next, CCW previous, wrap | Open highlighted app | No action; never launch on hold |
| SDR | Tune using selected step, clamp to band | Cycle step as today | At 800 ms open SDR menu; release must not cycle step |
| SDR menu | Select Pause/Resume, Span, Home, Back | Execute selection; Span opens 16/40/80 submenu | No alternate action |
| USB Microphone | No action | Toggle mute as today | At 800 ms open Home/Back menu; no mute on release |
| Hermes ready or reply | Scroll reply if present | Open action menu | At 300 ms begin push-to-talk; release sends utterance |
| Hermes thinking/speaking | Scroll available text | Open action menu | Cancel active turn explicitly, then begin new recording and stop playback |
| Hermes listening | No navigation | Not applicable | Continue recording; release sends. Turning the knob while held cancels recording and consumes release |
| Hermes action menu | Select Cancel turn, New conversation, Volume, Home, Back | Execute selected action | No speech capture |
| Hermes approval | Scroll prompt text or select Deny/Approve through explicit answer view | Confirm highlighted answer, default Deny | No voice capture and no implicit approval |
| Hermes offline/pairing | Scroll status/code where needed | Open Home/Back menu | No recording; display connection/pairing status |

Keep TALK/Menu arbitration in the board adapter: do not forward the initial GPIO0 down to `hg::App`, whose stock TALK immediately records or approves a prompt. Forward logical TALK only after the hold threshold. When an existing turn is active, explicitly send Cancel before TALK; upstream `start_listening()` stops local playback but does not itself send a Cancel frame. Require a visible Listening indication before speech; the initial hold-arbitration interval is not recorded. Map rotation to logical Up/Down outside local menus. Implement approval navigation in the adapter/UI, since upstream Up/Down ignores prompts; send a correlated `prompt.reply` only after the explicit answer click. Preserve the SDK's 600 ms prompt arming window using the **physical press-down timestamp**, not delayed TALK dispatch time; require a fresh press after prompt arrival and release gating. In the serialized WebSocket event path, cache prompt ID/title/text and TTL deadline and process prompt.close before input dispatch. Immediately before answering, recheck the active ID, authorization and deadline and consume that ID once. Upstream `answer_prompt()` is private and clears expiry in tick(), so do not infer approval safety from delayed logical TALK or bypass correlation with an unguarded console `yes` command. Send the correlated answer through the shared transport and synchronize core prompt dismissal through its normal prompt.close handler. No held TALK may answer a prompt. Menu defaults are Back or Deny, never Home, New conversation, or Approve. New conversation requires a second confirmation; Home is an explicit selection. Volume is bounded and stored in the Hermes namespace.

### Screen and behavior

Launcher: 24-pixel title, three distinct rows inside the 320 × 170 bounds, footer “TURN SELECT / PRESS OPEN”. Hermes: compact host/device title, visible connection/listening/thinking/speaking state, scrollable transcript/reply or approval text, and context-specific knob hints. Test the SDK renderer at this exact rectangular size; use its portable UI where possible, with a small local menu/approval adapter. A 320 × 170 RGB565 framebuffer is 108,800 bytes in PSRAM; retain internal DMA transfer strips. Do not import a second panel owner or assume touch gestures. Local menus use the same LCD HAL: keep the core framebuffer updated while suppressing its physical flush during an overlay, draw local menu strips through that same owner, then flush the full core framebuffer when the overlay closes. Continue core ticks and network/prompt-expiry processing under menus; never freeze the protocol to hold a screen. This avoids two competing renderers or stale cached bands after dismissal.

Provide voice-in, streaming text/status, voice-out, pairing, cancellation, conversation reset, prompt handling, cards/images supported by the core, and read-only diagnostics. Bound playback queues and abort promptly for barge-in. No wake word, multi-host selector, fleet aggregation, SDR/Hermes concurrency, or OTA in the first integration. These exclusions keep the third app complete for its supported SDK interaction without claiming every optional upstream feature.

### Integration and resource ownership

Keep the pinned ESP-IDF revision `25fe69f946311abdaf9ad56591f25fedbc20ac98`; do not upgrade SDR to match the reference SDK. The SDK declares IDF >=5.3 and documents 6.x support, but compatibility with this exact revision remains a build gate. Its codec dependency `~1.5` differs from this repository's `esp_codec_dev 1.6.2`: adapt against the existing version. Resolve and pin `esp_websocket_client` and its dependencies. Enable imported core C++17 explicitly.

Import the portable `firmware/core` as a component with provenance; adapt only needed Wi-Fi, WebSocket, storage, events, display and audio ports. Do not import upstream `app_main`, global driver instances, touch/Waveshare drivers, or automatic NVS erase recovery. This launcher retains the sole `app_main`; a C-callable `t_embed_hermes_run()` creates app objects only after Hermes selection. Set `DeviceProfile` explicitly: `has_cancel_button=false`, `has_scroll_buttons=true` for rotary Up/Down, `touch_screen=false`, and knob/menu labels. Override stock TALK/CANCEL prompt hints with the actual menu/approval instructions; never advertise a nonexistent Cancel key. Existing panel and PCNT initialization stays shared. Allocate Hermes objects, queues, framebuffers and audio buffers at runtime rather than adding large static BSS allocations.

The upstream codec port assumes an ES8311/ES7210 duplex bus. This board instead needs ES7210 RX on I2S0 and independent MAX98357A TX on I2S1. Reuse the existing microphone setup through a small shared helper only where required; keep USB packet framing unchanged. Driver tasks post bounded events; a single Hermes app task calls the core and owns UI updates. Advertise speaker capability only if initialization succeeds; otherwise display replies and a clear audio error.

Apps return Home through `esp_restart()` after cancelling active Hermes work/audio and waiting for knob release. Boot always returns to launcher, not auto-resume. This matches the existing restart approach while avoiding fragile SDR PHY teardown. Launcher must not start Wi-Fi/radio/audio ahead of selection. Hermes initializes normal station Wi-Fi; SDR initializes its raw PHY path in its own boot. Native USB will briefly reconnect on restart; pairing/configuration persists.

**Memory gate:** all linked code/static data count even when apps are inactive. Preserve `_iram_end <= 0x403a0000`, `_data_end <= 0x3fcb0000`, `_bss_end <= 0x3fcb0000`. A prior SDR build had only 336 bytes of BSS margin; this is historical evidence, not a measurement of the latest launcher build. Inspect the combined map, DMA heap, largest free internal block, PSRAM, stacks and worst-case audio/network buffers. A successful standalone Hermes build does not prove combined fit. If the guard fails after runtime allocation changes, stop and report the actual map; do not weaken the guard or claim the three-app image works.

### Connectivity, configuration and updates

Use one configured Hermes gateway endpoint initially. Provision Wi-Fi credentials and `server` URL over native USB, retain SDK device-key pairing in isolated `hgadget` NVS, and mask secrets in diagnostics. Keep NVS and PHY partitions unchanged. Do not erase NVS automatically on initialization failure; report an actionable error. Reject recording while offline; cancel incomplete streams after disconnect and reconnect with bounded backoff.

The board is a LAN Wi-Fi client, not a Tailscale node. For remote Hermes, use a LAN-accessible host relay that forwards WebSocket traffic over that host's tailnet connection; local Hermes may be addressed directly on LAN. Verify selected host, relay address, gateway plugin compatibility, STT and TTS configuration during integration. Do not assume running Hermes implies its gadget plugin or speech providers are configured. Local `hermes --version` reports v0.21.5+6155, upstream `b829f2af` (2026-09-24), Python 3.14.7; the SDK README reports CI against Hermes `2eceb02` (2026-10-03). Local gateway base code has streaming-TTS and approval hooks, but plugin compatibility must be tested rather than inferred from these names. No Gadget entry appeared in the local non-bundled plugin listing during review. No Hermes installation, update, restart or configuration change is included in this planning pass. Use WSS on network paths requiring confidentiality, especially initial device-key enrollment. No public port exposure is required.

Retain the current factory app partition at 0x10000, size 4 MiB, and 16 MiB flash configuration for v1. Combined image must fit; report actual bytes. SDK OTA expects a different dual-slot layout, so do not advertise `caps.ota` or enable its updater. Any future OTA applies to the entire three-app firmware and requires a separate partition/recovery design. Never flash upstream standalone images over this launcher as an app installation.

### Shadow-box walkthroughs and verification gates

The following are paper walkthroughs to reproduce in host simulation and on hardware; they are not recorded hardware passes.

1. **Cold boot:** no held knob -> three rows -> CW selects next, CCW previous -> click/release opens Hermes once. GPIO0 held at reset remains the documented download procedure.
2. **Launch click:** its release is consumed; Hermes opens idle, without a phantom recording. Boot with a held knob never approves a queued question.
3. **Voice:** hold 300 ms -> Listening -> speak -> release -> transcript/Thinking -> streaming text and speaker -> reply. Very short audio is discarded by core policy, with visible feedback.
4. **Cancel:** while recording, turn -> cancel -> release sends nothing. While thinking/speaking, tap -> menu -> Cancel -> no new turn and local speaker stops promptly.
5. **Approve safely:** prompt arrival during a held knob -> release without answering -> read full text -> answer view defaults Deny -> rotate to Approve -> fresh click after arming -> exactly one correlated response. Expiry/disconnect withdraws the selection; an old click cannot answer the next prompt.
6. **New conversation/Home:** menu -> deliberate selection -> reset confirmation where applicable -> release -> launcher. No Cancel hold accidentally resets a conversation; there is no physical Cancel button.
7. **Disconnect:** lose Wi-Fi during recording/playback -> discard incomplete recording, stop local audio, show Offline -> retain menu/Home -> reconnect without lost identity or phantom send.
8. **Cross-app:** Hermes -> Home/restart -> SDR tunes/captures -> Home/restart -> USB microphone mute/USB bridge -> Home -> Hermes reconnects with pairing intact. No GPIO6 input ISR remains, no competing SPI/I2S/Wi-Fi owner.
9. **Failure paths:** missing speaker, codec/PSRAM failure, invalid URL, denied pairing, expired prompt and allocation failure remain diagnosable and do not silently erase configuration.

Implementation order: (1) build/link spike with imported core and drivers on the exact SDK; inspect memory/image map, (2) one-knob dispatcher/menu and GPIO6 correction for all three modes, (3) exact-board display and split audio adapters, (4) network/provisioning/pairing and core event loop, (5) end-to-end acceptance with all app transitions. Add focused host tests for gesture arbitration, prompt correlation/expiry, launcher direction and menu actions; run repository host checks and reference core tests. Then verify real mic/STT, speaker/TTS, polarity, LCD bounds, USB continuity and repeated app switches, with a ten-minute voice/reconnect soak and measured heap/stack high-water marks. Build the supported profile matrix before distributing firmware.

Before any flash, take a **fresh full 16 MiB backup of the current installed launcher firmware and settings**, hash it and preserve matching source/config/artifacts. The original factory backup is not a substitute for the current installed image. Reidentify MAC/flash, inspect offsets and image hash, and flash only the reviewed combined build without full erase. This planning pass performs no device writes.

### Additional primary references

- [Standard board schematic](https://github.com/Xinyuan-LilyGO/T-Embed/blob/master/schematic/schematic.pdf), especially knob, MCU and amplifier pages.
- [Hermes reference revision](https://github.com/Adolanium/hermes-gadget-sdk/tree/edecb81c22f1c9308338735b0613be3d341e94ac).
- [SDK porting contracts](https://github.com/Adolanium/hermes-gadget-sdk/blob/edecb81c22f1c9308338735b0613be3d341e94ac/docs/porting.md).
- [SDK protocol and approval/transport semantics](https://github.com/Adolanium/hermes-gadget-sdk/blob/edecb81c22f1c9308338735b0613be3d341e94ac/docs/protocol.md).
- [Hermes gateway integration and configuration](https://github.com/Adolanium/hermes-gadget-sdk/blob/edecb81c22f1c9308338735b0613be3d341e94ac/docs/hermes-integration.md).
- Reference `firmware/core/src/app.cpp`, `firmware/esp32/main/{main,port_codec,port_storage,port_wifi}.cpp`, manifests and CMake files establish the exact integration limitations above.

### Planning validation result

Reference core configured and compiled with AppleClang 21 through `cmake -S ../hermes-gadget-sdk/firmware -B /tmp/hermes-gadget-core-review -DHG_BUILD_TESTS=ON`; its `hg_core_tests` CTest target passed. `git diff --check` passed for the specification. These host checks establish reference-core health only; combined ESP-IDF link, gesture simulation and physical hardware acceptance remain implementation gates.

Independent GPT-6-luna worker review at maximum reasoning identified physical-press timestamp/TTL races in approval handling and incorrect upstream default input capabilities; both are addressed above. The review was source-based, not a hardware or combined-link pass.
