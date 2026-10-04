# Hermes T-Embed firmware specification

## Purpose and supported hardware

Build a single-purpose push-to-talk Hermes Agent companion for the standard LILYGO T-Embed ESP32-S3R8. The firmware provides a one-entry launcher, local display and knob controls, the built-in microphone, Wi-Fi/WebSocket connectivity, persistent provisioning, and optional speaker playback through an attached MAX98357A amplifier. It does not include the ESP-SDR receiver or USB microphone bridge.

The supported board has 16 MB flash, 8 MB OPI PSRAM, a non-touch ST7789 display, and the board's integrated ES7210 microphone ADC. Other T-Embed variants are outside this v1 target. Speech recognition, model inference, and TTS are provided by the configured Hermes gateway and its services.

## Board resources

| Resource | GPIO / interface | Requirement |
| --- | --- | --- |
| Display | ST7789 SPI: MOSI 11, SCLK 12, CS 10, DC 13, RST 9, backlight 15 | 320 × 170 landscape; preserve the board panel initialization and visible-area offset. |
| Encoder | A 2, B 1 | Decode the factory TWO03 detent convention. |
| Encoder push / BOOT | GPIO0, active low | The only application push input. Never drive it; a low level during reset selects ROM download mode. |
| Main power enable | GPIO46 | Preserve board power sequencing. |
| Microphone ADC | ES7210, I2C SDA18/SCL8, address 0x40; I2S MCLK48/BCLK47/WS21/DIN14 | Capture mono PCM16 through bounded buffers. |
| Speaker amplifier | MAX98357A, independent I2S BCLK7/WS5/DOUT6 | Use a separate TX bus; GPIO6 is speaker data, not a button or microphone input. |
| USB | Native USB GPIO19/20 | Keep serial/JTAG available for provisioning and read-only diagnostics. |

## Controls and screens

The launcher has one Hermes entry; clicking the knob opens it. Home returns through a controlled restart to the launcher; the device does not auto-resume the app after reboot. The launcher must not start Wi-Fi or audio before Hermes is selected.

In Ready or a reply view, hold the knob for about 300 ms to enter Listening, then release to send the recording. Capture starts only after Listening is rendered. While Thinking or Speaking, a hold cancels the active turn and starts a new recording; a short click opens the action menu. While Listening, turning the knob cancels and discards the recording, and the release must not send it.

One debounced dispatcher owns GPIO0 and encoder events. Interpret an input using its context at physical press-down, require release after entering a mode, and suppress stale edges and delayed clicks after a hold. The approximate timing values are 30 ms debounce and 300 ms hold recognition. Rotary input maps to Up/Down except where a local overlay owns it.

The action menu offers Cancel Turn, New Conversation, Volume, Home, and Back when online and paired; otherwise show only supported actions. Back is the default. New Conversation requires a second confirmation with No selected by default. Home must be an explicit selection.

For an approval prompt, let the user read or scroll the full prompt, then open an answer view defaulted to Deny. Rotation selects Deny or Approve. Require a fresh physical click after the 600 ms arming window. Bind the answer to the current prompt ID and expiry, recheck authorization immediately before sending, and consume the prompt ID once. Prompt closure, expiry, or disconnect withdraws the answer. A held press or voice recording must never approve a prompt. Keep the core event loop and prompt expiry active while local overlays are shown; use one display/SPI owner and restore the current core framebuffer when an overlay closes.

Display the connection and pairing state, Listening/Thinking/Speaking state, transcript or reply, and actionable audio/network errors. Do not claim touch input, an extra Cancel key, wake-word support, multi-host selection, or OTA support.

## Audio and event ownership

Use the ES7210 receive path on I2S0 and the independent MAX98357A transmit path on I2S1. Keep microphone capture bounded and mono PCM16. AudioIn start/stop and App callbacks belong to the Hermes owner task; the adapter exposes nonblocking reads for bounded chunks and begins capture after the Listening view is flushed. Keep playback bounded to about 1.5 seconds of audio, report busy accurately, and abort promptly on cancellation or barge-in. Advertise speaker support only when initialization succeeds; if it fails, preserve text replies and report the audio error.

One Hermes owner task calls the core and owns UI updates. Driver tasks post bounded events; they do not call App methods directly. Stop network work and audio before Home/restart, wait for knob release, and preserve stored configuration. Allocate application queues and audio buffers at runtime; avoid large static BSS growth.

## Connectivity and provisioning

The board is a 2.4 GHz Wi-Fi station that connects to one configured Hermes Gadget WebSocket endpoint. It is not a Tailscale node and does not discover the gateway. Use a host or relay reachable from the board; use WSS when the network path requires confidentiality, including initial device-key enrollment.

Provision through the native USB serial console after selecting Hermes. `HELP` lists commands. `WIFI "ssid" "password"` saves Wi-Fi values and restarts; SSID is 1–32 bytes and password is 0–63 bytes. `SERVER "url"` saves a lowercase `ws://` or `wss://` URL with a nonempty host and restarts; maximum decoded URL length is 255 bytes. Wi-Fi quoted values support only `\"` and `\\` escapes; decoded quotes and backslashes are rejected in SERVER URLs. Control characters are rejected and the whole line is limited to 511 bytes. Values are entered into the serial terminal, not a shell.

Persist Wi-Fi configuration, gateway URL, volume, and Hermes pairing data in NVS. Keep the `hgadget` namespace for device identity. If NVS or settings initialization fails, report an actionable error; never erase NVS automatically. Mask credentials and pairing keys in output. `STATUS`, `DIAG`, and `HELP` are read-only and must not reveal SSID, password, URL, or key contents. On disconnect, reject new recordings, discard incomplete audio, stop playback as needed, display the reason/status, and reconnect with bounded backoff.

## Build and packaging

Support one build profile: `t-embed`, targeting ESP32-S3 using ESP-IDF commit `25fe69f946311abdaf9ad56591f25fedbc20ac98`. The build helper validates `IDF_PATH` against that commit and reads the single root `sdkconfig.defaults`. Build from source with:

```sh
python3 tools/build_firmware.py --profile t-embed --version local --output artifacts
```

The helper exports `bootloader.bin`, `partition-table.bin`, `hermes_t_embed.bin`, and `manifest.json` under `artifacts/t-embed/`. Preserve the 16 MB flash configuration and factory app partition at offset `0x10000`, size `0x400000`; verify the generated image fits. Do not enable OTA or change the NVS partition layout as part of v1. The CI workflow performs a pinned-SDK compile and uploads this package; it does not flash a device or run a hardware test.

Before any device write, make and verify a fresh full flash backup and confirm the board identity, image offsets, and hashes. Never perform a full erase as an ordinary update. Standalone-package hardware verification remains pending.

## Acceptance and known limits

- A clean CI build uses only the pinned ESP-IDF revision and produces the four named package files and a manifest with image offsets and hashes.
- The application image fits the 4 MiB app partition; target and flash configuration match the standard T-Embed ESP32-S3R8.
- The UI keeps the sole physical knob context-sensitive, waits until Listening is visible before capture, and fails closed for stale, closed, expired, or unauthorized prompts.
- Audio queues remain bounded, start/stop and abort behavior are deterministic, and failed speaker initialization does not disable text replies.
- Configuration and pairing survive restart; diagnostics do not expose secrets or erase NVS.

Wi-Fi connection, gateway pairing, and a voice interaction were confirmed on one standard board before the repository was pruned to a standalone Hermes package. That observation is not a validation of this standalone package. Prompt approval, repeated Home/restart cycles, reconnect stress, and sustained audio under network congestion still need hardware verification. Gateway startup, first-use model loading, speech-provider configuration, and response latency depend on the host and are not firmware guarantees.

## Provenance

This integration originated in [ESPARGOS/esp-sdr](https://github.com/ESPARGOS/esp-sdr), but this repository's firmware scope is Hermes only. The portable core source and its upstream revision are recorded in [platform/hermes_core/PROVENANCE.md](platform/hermes_core/PROVENANCE.md). See the root GPL license, core MIT license, and preserved notices for project and third-party terms.

## References

- [Standard T-Embed schematic](https://github.com/Xinyuan-LilyGO/T-Embed/blob/master/schematic/schematic.pdf)
- [Hermes Gadget SDK pinned reference](https://github.com/Adolanium/hermes-gadget-sdk/tree/edecb81c22f1c9308338735b0613be3d341e94ac)
- [SDK porting contracts](https://github.com/Adolanium/hermes-gadget-sdk/blob/edecb81c22f1c9308338735b0613be3d341e94ac/docs/porting.md)
- [SDK protocol and prompt semantics](https://github.com/Adolanium/hermes-gadget-sdk/blob/edecb81c22f1c9308338735b0613be3d341e94ac/docs/protocol.md)
