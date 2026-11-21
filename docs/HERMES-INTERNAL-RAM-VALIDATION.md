# Hermes internal-RAM validation

Date: 2026-11-21. Board: LILYGO T-Embed ESP32-S3R8 on the bench, USB serial. ESP-IDF `25fe69f946`.

## Problem

After the signal-intel work, `main` could not complete the Hermes TLS handshake on hardware.
Every attempt logged `mbedtls_ssl_setup returned -0x008D` and the app stayed at
`HERMES=connecting PAIRED=NO`. `main` was flashed unchanged to confirm it reproduced the failure
(`INTERNAL_FREE=1558 INTERNAL_LARGEST=1408` at the time).

## Cause

All three apps share one image. The SDR receiver's working buffers were static BSS, so they used
internal SRAM even while Hermes ran. Static `.dram0.bss` was 81,728 bytes against 18,784 bytes in
the 2026-10-05 Hermes-only image. The largest items were `ring_capture.c` (52.8 KB) and the
core-1 stack in `s3_core1.S` (8 KB). The USB microphone app contributes no static RAM.

Internal free RAM measured with temporary probes (since removed), Hermes app:

| Stage | Internal free |
| --- | --- |
| App entry | 73,386 |
| After audio init | 54,690 |
| After transport init | 46,958 |
| After `esp_netif_init` | 45,546 |
| After `esp_wifi_init` | 11,014 |
| At WebSocket start, before TLS | ~9,700 |

## Changes

1. Mic and speaker task stacks 4096 -> 3072 bytes; `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=3` for
   the T-Embed profile. About 5 KB; enough to pass TLS setup, with about 4.6 KB left afterwards.
2. The S3 SPEC working buffers (`txq`, `window_q15`, `win2`, `fft_buf`, `accum_buf`, `bin_of`,
   `frame_out`) and the core-1 stack are now allocated once from the internal heap in
   `ring_capture_init()` / `c1_start()`. They stay in internal SRAM because the two cores hand
   data over without a data cache. If any allocation fails every run returns `RING_FAIL_ARG`.
   `sram_guard.ld` is unchanged.

## Results

Build: `.dram0.bss` 81,728 -> 22,336 bytes. Linker guard passed. App SHA-256
`21f1f349c186ad2a2fdfbb41650f4116ea1d5aa2df8e5d93f0d839cb0431d1e8`.

Hermes on hardware, after a reset:

| | Change 1 only | Changes 1 + 2 |
| --- | --- | --- |
| `INTERNAL_FREE` | 4,630 | 63,879 |
| `INTERNAL_LARGEST` | 2,560 | 31,744 |
| `STATUS` | `HERMES=ready PAIRED=YES` | `HERMES=ready PAIRED=YES` |

Connected from home Wi-Fi and from a second network (owner's confirmation). No TLS errors in
the serial log after the change.

SDR on hardware, `SPEC 4000 1 2 0 6 <nfft>`, 7 s host capture, frames checked for the `SPC1`
magic and CRC-32 over header and bins:

| Build | nfft | Valid frames | CRC errors | Sequence gaps |
| --- | --- | --- | --- | --- |
| Static buffers | 256 | 2,587 | 0 | 0 |
| Heap buffers | 256 | 2,588 / 2,588 | 0 | 0 |
| Static buffers | 2048 | 1,338 | 0 | 1,249 |
| Heap buffers | 2048 | 1,660 / 1,307 | 0 | 943 / 1,230 |

`DUAL?` returned `DUAL 1 1` (second core alive), `RING 1500 6` returned a statistics line, and
`APP MIC` launched. The 2048-bin sequence gaps occur on both builds, so they are not caused by
this change; their source was not investigated.

## Not covered

- The waterfall was not viewed on the panel; the owner should confirm it looks as before.
- Push-to-talk and speaker playback with the 3072-byte audio stacks were not exercised by the
  tester. Raise the stacks if a crash appears.
- Free internal RAM with Hermes connected was 63.9 KB; margin before the change was 4.6 KB.
