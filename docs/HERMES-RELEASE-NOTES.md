# Hermes T-Embed experimental release

## Observed hardware results

On one standard LILYGO T-Embed ESP32-S3R8 with an attached speaker, the firmware connected to 2.4 GHz Wi-Fi, connected to the Gadget gateway, and completed pairing. The user confirmed a voice interaction worked and that a subsequent request responded immediately. No quantitative warm-latency benchmark or exhaustive hardware validation was performed.

The T-Embed build uses the ESP-IDF revision in `firmware-targets.json`. Wi-Fi/LwIP allocations and ordinary WebSocket buffers can use PSRAM; internal memory is reserved for task stacks and DMA. These settings resolved the observed Wi-Fi association and WebSocket client-init failures. Other inherited SDR profiles have not been rebuilt for this Hermes publication and are outside its supported hardware scope.

## Known limitations

- Internal heap remains tight after connection. Reconnects and sustained audio require further hardware validation.
- The transport has one pending send slot. Under congestion, a control message such as `audio.end` can be rejected without a retry by the portable core. This is an unobserved reliability risk, not the explanation for the successful request's first-use delay.
- Stopping microphone capture may discard queued tail samples; speaker-buffer overflow drops audio. Congestion behavior is not validated.
- Prompt approval, cancellation, repeated app transitions, and reconnect stress are not comprehensively validated on hardware.
- The gateway assembles an utterance before starting transcription. The local transcription model is loaded on first use; no latency improvement is promised across providers or machines.

## Distribution

Only source and build instructions are published. Device flash/NVS backups, credentials, generated configurations, and local build artifacts are excluded. Build this repository's `t-embed` profile; upstream browser flashers do not distribute this Hermes integration.
