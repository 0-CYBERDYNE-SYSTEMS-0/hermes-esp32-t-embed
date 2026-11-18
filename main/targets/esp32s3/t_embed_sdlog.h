/* Session logging to the T-Embed microSD slot (SPI3, CS 39 / SCK 40 /
 * MOSI 41 / MISO 38).
 *
 * The device is the capture surface in the field: a walk must produce a
 * session directory that a laptop can analyze afterwards, with no cable and no
 * network. Everything here is best-effort and never blocks the radio for long:
 * a full card or a missing card degrades to "not logging", never to a crash.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mounts on first use. Returns false when no card is present or the mount
 * fails; the caller may retry later (the card can be inserted at any time). */
bool t_embed_sdlog_mount(void);
bool t_embed_sdlog_ready(void);

/* One-line JSON for the serial CLI. */
void t_embed_sdlog_status_json(char *out, size_t cap);

/* Wall clock injection: the host (or NTP elsewhere) owns absolute time, the
 * device owns monotonic time. Returns false for a non-positive value. */
bool t_embed_sdlog_set_clock(int64_t unix_ms);
int64_t t_embed_sdlog_now_ms(void); /* 0 when the clock is unsynced */

/* Starts a session: creates /sdcard/sigint/<stamp>/, opens frames.ndjson and
 * events.ndjson, writes session.json. */
bool t_embed_sdlog_start(const char *label, unsigned sample_rate_hz,
                         unsigned nfft, unsigned lo_mhz, unsigned rate_code,
                         unsigned lpf_code, int gain_index);
void t_embed_sdlog_stop(void);
bool t_embed_sdlog_active(void);
uint32_t t_embed_sdlog_frames_written(void);
const char *t_embed_sdlog_session(void); /* "" when idle */
const char *t_embed_sdlog_dir(void);     /* "" when no session is open */

/* Rate-limited NDJSON frame append (default 200 ms between frames). */
void t_embed_sdlog_note_frame(const uint8_t *codes, unsigned nfft,
                              unsigned lo_mhz, unsigned rate_hz,
                              int gain_index);
void t_embed_sdlog_note_event(unsigned khz, const char *level, unsigned reason,
                              int over_floor);
void t_embed_sdlog_set_interval_ms(unsigned ms);

/* Writes an IQ8 capture as a SigMF recording pair. Returns true when both the
 * data and the metadata landed on the card. */
bool t_embed_sdlog_write_iq(const uint8_t *iq, size_t bytes, unsigned samples,
                            unsigned rate_code, unsigned lo_mhz, int gain_index,
                            const char *label);

#ifdef __cplusplus
}
#endif
