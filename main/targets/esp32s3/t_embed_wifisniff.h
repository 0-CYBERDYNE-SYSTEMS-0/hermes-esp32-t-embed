/* 802.11 frame capture: the chip's own MAC does the demodulation, so a walk
 * yields identities (SSIDs, BSSIDs, client MACs, probe fingerprints) instead
 * of anonymous energy blobs.
 *
 * The Wi-Fi driver is already started and already in promiscuous mode in SDR
 * mode; this only adds the receive callback and a writer. Frames are queued in
 * PSRAM and written from the SDR loop, never from the Wi-Fi task, so the FAT
 * volume keeps a single writer.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* channel: 1..14. Returns false when the radio rejects the channel. */
bool t_embed_wifisniff_start(unsigned channel);
void t_embed_wifisniff_stop(void);
bool t_embed_wifisniff_active(void);
/* Drains queued frames to the capture file; call from the owner task only. */
void t_embed_wifisniff_service(void);
void t_embed_wifisniff_status_json(char *out, size_t cap);
/* True when the file opened: frames are landing on the card, not just counted. */
bool t_embed_wifisniff_recording(void);

#ifdef __cplusplus
}
#endif
