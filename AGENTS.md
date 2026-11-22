# AGENTS.md

ESP-IDF firmware project (`esp_sdr`). The `t-embed` profile is a three-app launcher for the
LILYGO T-Embed ESP32-S3R8 (16 MB flash, 8 MB OPI PSRAM): ESP-SDR waterfall, USB microphone
bridge, and a push-to-talk Hermes Agent client over `ws://`/`wss://`. All other profiles are the
upstream ESP-SDR I/Q-capture firmware for ESP32/C3/C5/C6/C61/S2/S3/S31. Host-side Python tests
run without ESP-IDF; firmware builds need a pinned SDK checkout.

## Dev environment

- Host tests: `python3.12` (needs ≥ 3.11 — `hashlib.file_digest`; macOS `/usr/bin/python3` is 3.9 and fails `test_firmware_collect`) plus a host `c++`/`cc` for the C++-slice tests.
- Firmware: ESP-IDF at the exact commit in `firmware-targets.json` (`25fe69f9...`; `esp32s31` uses a separate preview SDK). `tools/build_firmware.py` hard-fails if `IDF_PATH` is not at that commit; `IDF_PATH` must be a git checkout (the script runs `git rev-parse HEAD` on it).
- Clone with `--recurse-submodules` — `components/esp-dsp` is a submodule.

## Build & test

```sh
python3.12 -m unittest discover -s tests            # all tests/ (~41, what CI runs)
python3.12 -m unittest discover -s host/rfkit -p 'test_*.py'   # host toolkit (16, also in CI)
python3.12 -m unittest tests.test_t_embed_menu      # one module
python3.12 -m unittest tests.test_t_embed_menu.TEmbedMenu.test_six_menu_choices_fit_and_last_choice_can_be_selected   # one test
(cd tools && python3.12 -m unittest test_t_embed_mic_bridge)   # lives in tools/; NOT run by CI
```

```sh
source ../esp-idf-hermes-t-embed/export.sh
python3 tools/build_firmware.py --profile t-embed --version local --output artifacts/<fresh-dir>
python3 tools/build_firmware.py --matrix            # JSON matrix consumed by CI
```

`--output/<profile>` must not already exist (`parser.error: Output already exists`). Build dir is
`build-<profile>`; also accepts `--build-root`, `--jobs` (default 2). Flash:

```sh
IDF_COMPONENT_MANAGER=1 python3 "$IDF_PATH/tools/idf.py" -C "$PWD" -B "$PWD/build-t-embed" \
  -p /dev/cu.usbmodemXXXX flash
```

`scripts/verify.sh` is only `git fsck` + clean-tree here (no root `pyproject.toml`/`requirements.txt`, so its Python step is skipped). It does not run the tests. CI (`.github/workflows/firmware.yml`) runs the `tests/` and `host/rfkit/` suites, builds all 9 profiles from `--matrix` against their pinned SDKs, then bundles via `tools/collect_firmware.py`.

## Source layout

- `main/targets/<target>/` — per-chip receiver, tuning, `sram_guard.ld`. C5/C6/C61 share `main/families/c5_c6_c61/receiver.c` with `chip.h` from the target dir.
- `main/common/`, `main/diagnostics/` (probe code, never packaged), `platform/esp32s2/` (pinned ROM USB-CDC), `platform/hermes_core/` (vendored).
- `host/rfkit/` — host-side serial logging and spike analysis (Python, tested in CI). `tools/` — build, collect, export scripts and the mic bridge. `validation/` — hardware evidence. `components/esp-dsp` — pinned submodule.
- T-Embed only, all in `main/targets/esp32s3/`: `t_embed_launcher.c` (sole `app_main`), `t_embed_hermes.cpp` (app loop, NVS `hgadget` ns, Wi-Fi, serial parser, menus), `t_embed_hermes_transport.cpp`, `t_embed_hermes_audio.cpp`, `t_embed_ui.c`, `t_embed_gesture.c`, `t_embed_microphone.c`, `t_embed_audio_input.c`.

## Conventions

- C sources: `static` file-local helpers, `#ifdef`-selected entry points, upstream lowercase snake_case. C++ side tags logs with a file-local `kTag` and `ESP_LOGI/W/D`; state in `hg::` namespace when inside the core.
- Conditional compilation is per-target via CMake (`main/CMakeLists.txt` picks sources by `IDF_TARGET`), not by scattering `#if` in shared files.
- Host tests are `unittest.TestCase` classes named after the feature (`class TEmbedMenu(unittest.TestCase)`). Not every file has an `if __name__ == "__main__": unittest.main()` block; run them through `unittest` as shown above.
- Many tests compile **slices of production source** cut by string markers (e.g. `source[source.index("class HermesStorage "):source.index("class HermesDisplay ")]`, or the `t_embed_ui_render_menu_overlay(` … `t_embed_ui_invalidate_overlays(` span). Renaming/reordering those symbols silently breaks tests — update the markers with the code.
- `tests/test_hermes_text_mode.py` compiles `tests/hermes_text_mode.cpp` against all of `platform/hermes_core/src` with `-Wall -Wextra -Werror`.
- Commit subjects are imperative and capitalized with no type prefix (`Fix …`, `Add …`); the last 25 commits use no `fix:`/`docs:`/`chore:` prefixes. PR merges append `(#N)`. Match the surrounding file's style rather than reformatting.
- Any local edit under `platform/hermes_core/` must be described in `platform/hermes_core/PROVENANCE.md` (MIT, pinned upstream commit). GPL-3.0-or-later overall; third-party notices in `LICENSES/`.

## Hard constraints

- **Memory gate:** `main/targets/esp32s3/sram_guard.ld` enforces `_iram_end <= 0x403a0000`, `_data_end`/`_bss_end <= 0x3fcb0000`, margin historically tiny. Allocate Hermes buffers/queues/framebuffers at runtime in PSRAM, never static BSS. The SDR's SPEC working buffers and core-1 stack (`ring_capture.c`, `s3_core1.S`) are internal-heap allocations made in `ring_capture_init()`/`c1_start()`, not BSS: they cost ~59 KB that Hermes TLS needs. Keep new large buffers out of static BSS. If the guard fails, report the map — never weaken the guard.
- Preserve every target's `sram_guard.ld` and the C61/S31 `--wrap=phy_write_gain_mem` gain-table wrapper when refactoring receivers.
- Don't change the pinned IDF revision, `partitions.csv` (4 MB factory app at 0x10000, no OTA slots), or NVS/PHY partitions without explicit review. Don't advertise OTA.
- Diagnostics (`STATUS`, `DIAG`) must never print SSID, password, server URL, or pairing key.
- GPIO0 is the encoder push **and** the BOOT strap — never drive it as output. GPIO6 is speaker DOUT, not a button. Speaker I2S: GPIO7 BCLK / GPIO5 WS / GPIO6 DOUT.

## Pitfalls

- **Single owner per resource:** one task owns the ST7789 panel (SPI2), the knob (PCNT+GPIO0), and in SDR mode the radio ring and USB transport. Don't add tasks touching them concurrently. Apps never return — "Home" is `esp_restart()`; the launcher starts no Wi-Fi/radio/audio before selection.
- Probe builds (`SAMPLE_RATE_PROBE`, `RING_PROBE`, `FILTER_REGISTER_PROBE`, `S2_RF_PROBE`, `S3_RF_PROBE`, `C5_TUNE_PROBE`) are CMake options that must never ship. `tools/build_firmware.py` forces all of them OFF. `FILTER_REGISTER_PROBE` on esp32c6 is a CMake fatal error.
- `managed_components/`, `build-*/`, `artifacts/`, `sdkconfig`, and `.esp-idf/` are git-ignored generated output; only `sdkconfig.defaults*`, `dependencies.lock`, and `main/sdkconfig.rename` are tracked. Don't hand-edit generated SDK config.
- Serial provisioning (`WIFI`, `WIFI2`, `SERVER`) saves to NVS and reboots; commands echo nothing, and a malformed line returns a syntax error without restarting. See `tools/README-t-embed-hermes.md`.
- A passing compile or host test does not validate hardware; hardware evidence belongs in `validation/`. `CLAUDE.md`, `SPEC.md`, and `docs/HERMES-TEXT-MODE-SPEC.md` carry the deeper design rationale.
