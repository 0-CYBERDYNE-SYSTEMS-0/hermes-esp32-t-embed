# Hermes T-Embed status

## Hardware observations

On one standard LILYGO T-Embed ESP32-S3R8 with an attached speaker, Wi-Fi connection, Gadget gateway pairing, and a voice interaction were confirmed. The user reported that a subsequent request responded immediately; no quantitative latency benchmark was recorded. The standalone Hermes-only source package has not yet received a separate hardware verification pass.

## Known limitations

- Standalone runtime memory headroom has not been measured; reconnect and sustained-audio validation is pending.
- The transport has one pending send slot. Under congestion, a control message such as `audio.end` can be rejected without a retry by the portable core. This is a reliability risk that has not been observed in the confirmed voice interaction.
- Stopping microphone capture may discard queued tail samples; speaker-buffer overflow drops audio. Congestion behavior is unvalidated.
- Prompt approval, cancellation, repeated Home/restart cycles, and reconnect stress are not comprehensively validated on hardware.
- The gateway assembles an utterance before transcription. The transcription model may download or load on first use; response time depends on the gateway, provider, and configuration.

## Build and distribution

The repository supports one profile, `t-embed`. The pinned ESP-IDF workflow compiles that profile and uploads a CI artifact containing `bootloader.bin`, `partition-table.bin`, `hermes_t_embed.bin`, and `manifest.json`. No firmware binary is committed to the repository. Review the manifest and image/partition sizes before any device write; use the build guide's backup and flash instructions.

The repository retains the upstream ESPARGOS/ESP-SDR GPL license as source provenance, but no SDR receiver or USB microphone bridge application is part of this firmware scope. The Hermes core's MIT license and third-party notices remain under `platform/hermes_core/`.
