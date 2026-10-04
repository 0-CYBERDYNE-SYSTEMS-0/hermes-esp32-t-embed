# Hermes for LILYGO T-Embed

This repository builds one application: a push-to-talk Hermes Agent companion for the standard **LILYGO T-Embed ESP32-S3R8**. Use the built-in microphone and display, and connect a MAX98357A I2S amplifier for spoken replies. The rotary knob controls recording, menus, and prompt answers.

The firmware connects over 2.4 GHz Wi-Fi to a reachable Hermes Gadget gateway. Speech recognition, language-model responses, and speech synthesis run on that gateway or its configured providers; they do not run on the T-Embed.

## Build and setup

1. Clone the repository:

   ```sh
   git clone https://github.com/0-CYBERDYNE-SYSTEMS-0/hermes-esp32-t-embed.git
   cd hermes-esp32-t-embed
   ```
2. Install and activate the pinned ESP-IDF revision using the [build and flash instructions](tools/README-t-embed-hermes.md#build-and-validation), then export the flash package:

   ```sh
   python3 tools/build_firmware.py --profile t-embed --version local --output artifacts
   ```

   The helper requires an activated ESP-IDF checkout at the pinned commit and writes the bootloader, partition table, Hermes application image, and manifest to `artifacts/t-embed/`.
3. Review the [known limitations and validation status](docs/HERMES-RELEASE-NOTES.md), flash the board using the guide, then complete its [gateway setup](tools/README-t-embed-hermes.md#set-up-the-hermes-gadget-gateway), Wi-Fi provisioning, and pairing steps.

The first boot opens the Hermes-only launcher. Click its Hermes entry, provision Wi-Fi and the gateway URL over USB, then pair the device. Hold the knob to speak and release it to send. The setup guide documents the serial commands, speaker pins, prompt controls, and build/flash workflow.

## Hardware and validation

The supported board is the standard T-Embed ESP32-S3R8 with 16 MB flash and 8 MB OPI PSRAM. Other T-Embed variants are not validated. The speaker requires an external MAX98357A amplifier; see the setup guide for wiring.

On one board, Wi-Fi connection, gateway pairing, and a voice interaction were confirmed. The newly pruned standalone package still needs hardware verification. Prompt approval, reconnect stress, repeated app restarts, and sustained audio/network congestion have not been comprehensively validated.

## Project scope and credits

This is the Hermes application integration only; it does not include ESP-SDR receiver features or the USB microphone bridge. The repository originated from [ESPARGOS/esp-sdr](https://github.com/ESPARGOS/esp-sdr), whose GNU GPL version 3 license remains in [LICENSE](LICENSE). The portable Hermes core is from [Adolanium/hermes-gadget-sdk](https://github.com/Adolanium/hermes-gadget-sdk); its MIT license and artwork/board-port notices are preserved in [platform/hermes_core](platform/hermes_core/PROVENANCE.md). The community Gadget plugin is installed separately on the gateway. This project is unofficial and is not endorsed by Nous Research.
