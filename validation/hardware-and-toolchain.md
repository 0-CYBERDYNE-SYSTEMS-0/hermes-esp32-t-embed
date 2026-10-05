# Historical hardware and toolchain record

This record predates the Hermes integration. Local filesystem paths and device identity are generalized for publication. For current build, flash, and setup instructions, use [the Hermes guide](../tools/README-t-embed-hermes.md); the image sizes and control assumptions below describe an earlier build.

## Device identification

Read-only esptool 4.8.1 identification on 2026-10-02:

- Port: `/dev/cu.usbmodem1101` (native USB Serial/JTAG)
- SoC: ESP32-S3 QFN56, revision 0.2
- Crystal: 40 MHz
- Embedded PSRAM: 8 MB, AP_3v3
- External flash: manufacturer/device ID `c8:4018`, 16 MB, quad, 3.3 V
- MAC: `<DEVICE_MAC>`

These details match the connected unit. `esptool chip_id` and `flash_id` only queried the chip and reset it; no flash sectors were written.

## Original flash backup

Full flash read completed before any firmware write:

- File: `/path/to/local/sandbox/t-embed-toolchain/t-embed-preflash-20261002.bin`
- Size: 16,777,216 bytes (16 MiB)
- SHA-256: `37fb4e30b9a2d9b187fb98d1ea67a76ff378d9de3382b95f0aedea09b66010dc`
- Read command: `esptool.py --port /dev/cu.usbmodem1101 --baud 460800 read_flash 0 0x1000000 <backup-path>`

Keep this image outside the project tree. A restoration has not been attempted.

## Pinned ESP-IDF toolchain

Installed under `/path/to/local/sandbox/t-embed-toolchain`:

- ESP-IDF commit: `25fe69f946311abdaf9ad56591f25fedbc20ac98`
- The pinned checkout declares version `6.2.0` in `tools/cmake/version.cmake`; `git describe` reports `v6.1-dev-6376-g25fe69f946`. The manifest commit hash is the authoritative SDK identity.
- Python: 3.12.14
- Xtensa compiler: `xtensa-esp-elf-gcc` 16.1.0
- esptool for board inspection: 4.8.1 (isolated Python venv)
- ESP-IDF tools and virtual environment: `t-embed-toolchain/espressif`
- Required project submodule: `components/esp-dsp` at `a53a0756833c045311ea1d79a2badf495cdfde4c`

Activate in a zsh shell:

```sh
export IDF_TOOLS_PATH=/path/to/local/sandbox/t-embed-toolchain/espressif
export PATH=/path/to/local/sandbox/t-embed-toolchain/espressif/python_env/idf6.2_py3.12_env/bin:$PATH
source /path/to/local/sandbox/t-embed-toolchain/esp-idf/export.sh
```

The isolated Python path must precede system Python in `PATH`, or `export.sh` can select the system 3.11 interpreter and look for a non-existent 3.11 IDF environment.

The ESP-SDR manifest requires exactly this IDF commit for the T-Embed (`t-embed`) profile. Do not substitute a floating IDF version.

## Build command

The validated source was built with the repository profile tool. Reproduce the build with:

```sh
export IDF_TOOLS_PATH=/path/to/local/sandbox/t-embed-toolchain/espressif
export PATH=/path/to/local/sandbox/t-embed-toolchain/espressif/python_env/idf6.2_py3.12_env/bin:$PATH
source /path/to/local/sandbox/t-embed-toolchain/esp-idf/export.sh
cd /path/to/local/sandbox/t-embed-sdr
python tools/build_firmware.py \
  --profile t-embed \
  --version 20261002-controls-two03-review \
  --build-root /path/to/local/sandbox/t-embed-toolchain \
  --output /path/to/local/sandbox/t-embed-toolchain/artifacts-controls-final \
  --jobs 8
```

The device has 16 MB flash. The T-Embed profile explicitly selects 16 MB flash and OPI PSRAM while retaining the 4 MB application partition. The latest app image is 717,328 bytes and starts at `0x10000`; bootloader and partition table are at `0x0` and `0x8000`. The linked `_bss_end` is `0x3fcafeb0`, 336 bytes below the ESP-SDR capture-ring guard at `0x3fcb0000`.

The complete frozen local source (including uncommitted and untracked files, excluding `.git` metadata and this self-referential validation record) is archived at `/path/to/local/sandbox/t-embed-toolchain/artifacts-controls-final/t-embed/source-snapshot.tar.gz`. It is 1,123,982 bytes with SHA-256 `04ec37fbd2685cb8b4e3aef1c89ed43ef8d997b69f696b5441f04b232897e811`. The package records version `20261002-controls-two03-review` and IDF commit above. The app SHA-256 is `d5f7f28d26274c9f2991fbccd69f701e51d579e7a14a3c5d630e1a2554c3847a`.

For flashing, run from `/path/to/local/sandbox/t-embed-toolchain/artifacts-final/t-embed` using the matching esptool installation:

```sh
python -m esptool --chip esp32s3 --port /dev/cu.usbmodem1101 --baud 460800 write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 t-embed/0-bootloader.bin \
  0x8000 t-embed/1-partition-table.bin \
  0x10000 t-embed/2-esp_sdr.bin
```

This command writes only the bootloader, partition table, and app; do not erase flash as part of an ordinary update.

## Host validation run

The documented upstream host checks passed from the project root:

```text
python3.12 -m unittest discover -s tests
Ran 27 tests in 3.642s
OK
```

The control-specific host review also passed:

```text
python3.12 validation/check_controls.py
PCNT actions match pinned ESP-IDF; CW/CCW, TWO03 half-cycle, bounce, and latched button paths pass.
```

## Hardware validation state

The first on-device run confirmed the screen and long-press pause path, but encoder turns and short GPIO6 taps were not detected. The firmware polls controls in the receiver's main loop, while `ring_capture_run()` masks core-0 interrupts across its bounded capture window and processing. The 85 ms RF window, followed by graph/waterfall SPI writes, makes that polling too sparse to observe the rotary phase sequence or reliably sample a short button press. This explains the observed failure; it is not evidence that the board's encoder itself is faulty.

LILYGO's standard-board factory example constructs its encoder for GPIO2/1 with `LatchMode::TWO03` and attaches CHANGE interrupts on both channels ([factory source](https://github.com/Xinyuan-LilyGO/T-Embed/blob/main/examples/factory/factory.ino#L48-L49), [edge setup](https://github.com/Xinyuan-LilyGO/T-Embed/blob/main/examples/factory/factory.ino#L315-L322)). The bundled library defines TWO03 as latching at phase states 0 and 3 ([library header](https://github.com/Xinyuan-LilyGO/T-Embed/blob/main/lib/RotaryEncoder/src/RotaryEncoder.h#L24-L28)). The official standard-board schematic identifies encoder switch J1 as Zippy `ANM-I1W-O1W-Z` / model `1001040005`; the pin map gives A/B/key GPIOs ([schematic](https://github.com/Xinyuan-LilyGO/T-Embed/blob/main/schematic/schematic.pdf), [pin map](https://github.com/Xinyuan-LilyGO/T-Embed/blob/main/examples/factory/pin_config.h#L27-L35)). The Zippy-approved datasheet I found does not specify detents or pulses per revolution, so do not infer its physical resolution from a generic library RPM formula. The pinned ESP-IDF checkout includes an official PCNT quadrature example that decodes both channels in hardware and documents four counter increments per complete rotary step in X4 mode ([source at pinned SDK commit](https://github.com/espressif/esp-idf/blob/25fe69f946311abdaf9ad56591f25fedbc20ac98/examples/peripherals/pcnt/rotary_encoder/main/rotary_encoder_example_main.c), [example README](https://github.com/espressif/esp-idf/blob/25fe69f946311abdaf9ad56591f25fedbc20ac98/examples/peripherals/pcnt/rotary_encoder/README.md)).

The control fix is now built using PCNT X4 quadrature counting with a ±2 edge threshold matching LILYGO's TWO03 external-position latch, plus falling-edge events for GPIO0/GPIO6. Host tests verify its configured phase actions, both directions, half-cycle detent scaling, and bounce cancellation; hardware confirmation remains pending. Turn both directions and confirm bounded, monotonic tuning; register short GPIO6 taps even when press/release occurs during RF capture; ensure one event per press despite contact bounce; verify short-click step cycling and long-hold pause without double events. Useful read-only diagnostics are separate raw-edge, completed-detent and button-event counters so phase polarity, detent ratio, and short-click retention can be distinguished without a camera. This validator performed no flash writes or serial operations; root owns all device access.
