# Hermes internal-RAM and spoken-reply validation

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

## Spoken replies (2026-11-21, same board)

After the RAM fix, speech played for a few seconds, turned to static, and cut off. Four separate
causes, found from the board's serial log and the gateway log:

1. **Static.** `write()` queued whatever fit in the FreeRTOS stream buffer. A stream buffer keeps
   one byte free, so its room is odd; a partial write split a 16-bit sample and shifted every later
   byte. Fix: only whole samples are queued (`t_embed_speaker_queue.h`, host-tested).
2. **Memory overrun.** The static stream buffer was given `size` bytes of storage; FreeRTOS needs
   `size + 1`. Fixed, and the buffer is now ~4 s (128 KiB, PSRAM).
3. **Underruns.** The I2S DMA ring was 2 x 160 frames (20 ms). It is now 6 x 240 (90 ms), and the
   drain tail after the last write was lengthened to match.
4. **First press after boot sent nothing.** The transport send queue held one message and kept it
   there while it was being sent, so a mic frame in flight swallowed the next `audio.start`.
   The queue is now 8 deep, and a dropped send is logged with its reason.

The remaining cutoff was **not** in the firmware. The board's log showed `rx audio.abort` from the
gateway 10.1 to 10.4 s after every `rx audio.start`, immediately before the final `rx reply`. The
Hermes gateway waits a fixed 10 s for its streaming-TTS synthesiser to finish once the text is done
(`gateway/run_turn.py`, `wait_complete(timeout=10.0)`), then cancels the stream and never replays
it. A local speech model that needs longer than that for a long answer is cut off, with the rest
unspoken. The wait is now read from `tts.streaming.finalize_timeout_s` (default 10, set to 120 for
this setup). That change is in the gateway install, not in this repository, and a gateway update
may overwrite it. After the change and a gateway restart, long answers played through.

Turning streaming off (`tts.streaming.provider: none`) is not a workaround here: the local model
then fails on one large request ("peer closed connection... incomplete chunked read") and nothing
is spoken.
