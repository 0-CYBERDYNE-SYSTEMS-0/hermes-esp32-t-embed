# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

Two firmware products share one ESP-IDF project (`esp_sdr`):

1. **T-Embed launcher** (`t-embed` profile, the focus of this fork): a three-app launcher for the standard LILYGO T-Embed ESP32-S3R8 (16 MB flash, 8 MB OPI PSRAM) — ESP-SDR waterfall, USB microphone bridge, and a push-to-talk Hermes Agent client that talks to a Hermes Gadget gateway over `ws://`/`wss://`.
2. **Upstream ESP-SDR** (all other profiles): raw I/Q capture / spectrum firmware for ESP32, C3, C5, C6, C61, S2, S3, S31, driven by a newline ASCII command protocol over USB/UART.

Design intent and constraints live in `SPEC.md` (T-Embed three-app spec) and `docs/HERMES-TEXT-MODE-SPEC.md`. User-facing setup, serial provisioning commands (`WIFI`, `WIFI2`, `SERVER`, `STATUS`, `DIAG`), and knob controls are in `tools/README-t-embed-hermes.md`.

## Commands

Host tests (no ESP-IDF needed; need a host `c++`/`cc` and **Python ≥ 3.11** — `hashlib.file_digest` is used, so macOS `/usr/bin/python3` 3.9 fails `test_firmware_collect`):

```sh
python3.12 -m unittest discover -s tests                     # what CI runs
python3.12 -m unittest tests.test_hermes_text_mode           # one module
python3.12 -m unittest tests.test_t_embed_menu.TEmbedMenu.test_six_menu_choices_fit_and_last_choice_can_be_selected
(cd tools && python3.12 -m unittest test_t_embed_mic_bridge) # mic-bridge host tool; not under tests/
```

`scripts/verify.sh` only runs `git fsck` and a clean-tree check here (no `pyproject.toml`/`requirements.txt` at root, so its Python step is skipped); it does not run the test suite.

Firmware build — requires an ESP-IDF checkout at the exact commit pinned in `firmware-targets.json` (`25fe69f946311abdaf9ad56591f25fedbc20ac98`; S31 uses a separate preview SDK). `tools/build_firmware.py` checks `IDF_PATH` is at that commit:

```sh
source ../esp-idf-hermes-t-embed/export.sh
python3 tools/build_firmware.py --profile t-embed --version local --output artifacts/<fresh-dir>
python3 tools/build_firmware.py --matrix          # JSON profile matrix used by CI
```

The output profile directory must not already exist. Build dir is `build-<profile>`. Flash the T-Embed build:

```sh
IDF_COMPONENT_MANAGER=1 python3 "$IDF_PATH/tools/idf.py" -C "$PWD" -B "$PWD/build-t-embed" -p /dev/cu.usbmodemXXXX flash
```

CI (`.github/workflows/firmware.yml`) runs the host tests, then builds every profile in `firmware-targets.json` against its pinned SDK, then bundles them with `tools/collect_firmware.py`.

## Build configuration

- Profiles are defined in `firmware-targets.json`. The `t-embed` profile sets `t_embed_apps` (→ `-DESP_SDR_T_EMBED_APPS=ON`), enables the IDF component manager (for `esp_codec_dev` 1.6.2 and `esp_websocket_client` 1.4.0 from `main/idf_component.yml`), and layers `sdkconfig.defaults` + `sdkconfig.defaults.esp32s3` + `sdkconfig.defaults.t-embed`. All other profiles build with the component manager **disabled**; ESP-DSP comes from the `components/esp-dsp` submodule.
- `ESP_SDR_T_EMBED_APPS` adds `platform/hermes_core` as a component and compiles the launcher/Hermes/mic sources; `CONFIG_ESP_SDR_T_EMBED_UI` adds the display UI and gesture sources. Both are S3-only (CMake errors otherwise).
- `main/CMakeLists.txt` selects sources per `IDF_TARGET`: `main/targets/<target>/receiver.c`, or `main/families/c5_c6_c61/receiver.c` (with `chip.h` from the target dir) for C5/C6/C61.
- Diagnostic probes (`RING_PROBE`, `SAMPLE_RATE_PROBE`, `FILTER_REGISTER_PROBE`, ...) are CMake options that must never be packaged.

## T-Embed architecture

- `t_embed_launcher.c` owns the only `app_main` when apps are enabled; the S3 `receiver.c` `app_main` becomes `t_embed_sdr_run()` under `#ifdef ESP_SDR_T_EMBED_APPS`. The launcher dispatches to `t_embed_sdr_run` / `t_embed_microphone_run` / `t_embed_hermes_run` (`t_embed_apps.h`).
- **Apps never return.** "Home" is `esp_restart()` back to the launcher; boot never auto-resumes an app. The launcher must not start Wi-Fi/radio/audio before selection. This avoids tearing down the SDR raw-PHY path.
- **Single owner per resource.** One loop/task owns the ST7789 panel (SPI2), the knob (PCNT + GPIO0), and in SDR mode the radio ring and USB transport. Driver tasks post bounded events; one Hermes app task calls the core and owns UI updates. Don't add tasks that touch the panel, radio ring, or USB concurrently. Local menus draw through the same LCD owner while suppressing the core framebuffer flush (see SPEC.md "Screen and behavior").
- Hermes is split into `t_embed_hermes.cpp` (app loop, NVS storage in the `hgadget` namespace, Wi-Fi primary/fallback logic, serial command parser, local menus), `t_embed_hermes_transport.cpp` (WebSocket), and `t_embed_hermes_audio.cpp` (ES7210 mic RX on I2S0, MAX98357A speaker TX on I2S1: GPIO7 BCLK / GPIO5 WS / GPIO6 DOUT). `t_embed_ui.c` is shared display/launcher/menu rendering; `t_embed_gesture.c` is knob hold/click arbitration.
- `platform/hermes_core` is vendored from Adolanium/hermes-gadget-sdk at a pinned commit (MIT). Any local change to it must be described in `platform/hermes_core/PROVENANCE.md`. Don't import upstream `app_main`, global drivers, touch drivers, or automatic NVS-erase recovery.
- GPIO0 is the encoder push **and** the BOOT strap — never drive it as an output. GPIO6 is speaker DOUT, not a button.

## Hard constraints

- **Memory gate:** S3 linker guards (`main/targets/esp32s3/sram_guard.ld`) enforce `_iram_end <= 0x403a0000`, `_data_end`/`_bss_end <= 0x3fcb0000`. Margin has historically been tiny. Allocate Hermes buffers/queues/framebuffers at runtime (PSRAM), not as static BSS. If the guard fails, report the map — never weaken the guard. Preserve every target's `sram_guard.ld` and the C61/S31 `--wrap=phy_write_gain_mem` gain-table wrapper when refactoring receiver code.
- Keep the pinned ESP-IDF revision, `partitions.csv` (4 MB factory app at 0x10000, no OTA slots), and NVS/PHY partitions unchanged without explicit review. Don't advertise OTA.
- Diagnostics (`STATUS`, `DIAG`) must never print SSID, password, server URL, or pairing key.

## Test conventions

Many host tests compile **slices of production source** extracted by string markers, e.g. `source[source.index("class HermesStorage "):source.index("class HermesDisplay ")]` in `tests/test_t_embed_hermes_wifi.py`, or the `t_embed_ui_render_menu_overlay(` … `t_embed_ui_invalidate_overlays(` span in `tests/test_t_embed_menu.py`, then link them against host fakes. Renaming, reordering, or moving those classes/functions breaks the tests — update the markers alongside the code. `tests/test_hermes_text_mode.py` builds `tests/hermes_text_mode.cpp` against the full `platform/hermes_core/src` with `-Wall -Wextra -Werror`.

A passing compile/host test does not validate hardware behavior; hardware evidence is recorded under `validation/`.
