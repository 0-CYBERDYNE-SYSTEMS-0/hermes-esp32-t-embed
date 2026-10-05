# Hermes hotspot validation

Date: 2026-10-05.

## Behavior

- `WIFI2` saves hotspot credentials separately from the home Wi-Fi credentials.
- Automatic selection starts with home Wi-Fi and alternates saved networks after
  failed connection attempts. A working connection is retained until disconnected.
- The Wi-Fi Network menu offers Auto, Home Wi-Fi and Phone Hotspot. Manual
  selection lasts for the current Hermes session and does not rewrite credentials.
- TLS allocations use PSRAM, with software AES to avoid the observed hardware
  AES DMA allocation failure when internal memory is low.

## Host and build checks

- The standalone repository's host suite covers knob gestures, primary/fallback
  storage and retry behavior, manual selection, quoted provisioning commands,
  and all six menu rows fitting above the footer.
- `python3 -m unittest discover -s tests`: all six tests passed using Python 3.12.
- The repository's only firmware profile (`t-embed`) built and exported with
  pinned ESP-IDF `25fe69f946311abdaf9ad56591f25fedbc20ac98`.
- Generated configuration confirms `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` and
  hardware AES disabled.
- Standalone app: 1,174,944 bytes; SHA-256
  `0b3084be195449cc293845736239166002dee63c35ad3e8e4f87a5d0a36aa432`.
- `git diff --check` passed.

## Hardware observations

These checks used the local integration firmware containing these changes.
The standalone package was not reflashed during publication.

- Home Wi-Fi reached `WIFI=UP HERMES=ready PAIRED=YES` through the shared secure
  gateway endpoint after the TLS memory fix.
- A hotspot discovery failure (reason 201) was traced to straight versus curly
  apostrophes in the supplied and broadcast SSIDs. Correcting the saved name
  resolved the failure; the password and pairing key were preserved.
- With both networks available, selecting Phone Hotspot reached
  `WIFI_PROFILE=FALLBACK WIFI_MODE=MANUAL WIFI=UP HERMES=ready PAIRED=YES`.
  The assigned address was in the iPhone hotspot's subnet. The user confirmed
  the connection works.
- The user approved enabling Tailscale Funnel on the gateway. Public TLS access
  and rejection of an invalid enrolled-device key were verified. No gateway
  hostname, credentials, device keys, or device backups are included here.

Automatic fallback when home Wi-Fi disappears is covered by host checks but was
not forced on hardware. Sustained voice use, reconnect stress and quantitative
latency remain untested.
