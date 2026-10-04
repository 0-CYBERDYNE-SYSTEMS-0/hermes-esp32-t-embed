# T-Embed Hermes setup

This guide is for the standard T-Embed ESP32-S3R8 running the three-app launcher. The Hermes app uses the board's native USB Serial/JTAG connection for setup and diagnostics. This repository currently provides source-build instructions; it does not publish a firmware release image.

## Set up the Hermes Gadget gateway

Install and configure [Hermes Agent](https://github.com/NousResearch/hermes-agent) on the gateway computer first, including a working model provider and speech configuration. Then install the community Gadget platform plugin at its tested revision, enable it, restart the gateway, and ask it for the device URL:

```sh
hermes plugins install https://github.com/Adolanium/hermes-gadget-sdk.git#plugin --ref c3df35f82746d1f711ee1ee885d46552210dac2a --enable
hermes config set platforms.gadget.enabled true
hermes gateway restart
hermes gadget info
```

The default device URL is `ws://<gateway-LAN-address>:8765/gadget`. The port or path can be customized, and a gateway configured with TLS reports a `wss://` URL. Use the complete URL printed by `hermes gadget info`; the T-Embed must be able to reach that host and port over the LAN. Do not use `localhost` or `127.0.0.1` for a gateway running on another computer. Check that the firewall allows the configured port and that guest-network or VLAN isolation does not block the board.

## Provision Wi-Fi and connect the board

Connect the board by USB and find its serial port. On macOS:

```sh
ls /dev/cu.usbmodem*
screen /dev/cu.usbmodemXXXX 115200
```

Replace `XXXX` with the port shown on your Mac. At the launcher, rotate to **Hermes** and click the knob. Send one command per line; each line must end with Enter/newline. `HELP` prints the command list.

The serial console does not echo typed characters, so the line may look blank while you type. Press Enter and wait for the board's reply. This ESP32-S3 uses 2.4 GHz Wi-Fi; use a 2.4 GHz SSID and make sure the board and gateway can communicate on the same LAN.

Set Wi-Fi:

```text
WIFI "YOUR_SSID" "YOUR_WIFI_PASSWORD"
```

After the board restarts, select Hermes again and set the URL reported by `hermes gadget info`:

```text
SERVER "ws://YOUR_GATEWAY_LAN_ADDRESS:8765/gadget"
```

Replace the example with the exact URL printed by `hermes gadget info`; use its `wss://` scheme if TLS is configured. The firmware accepts lowercase `ws://` or `wss://` URLs with a nonempty host. It does not discover a gateway or choose a host, port, or WebSocket path for you.

Each successful `WIFI` or `SERVER` command is saved to NVS and replies `OK ... SAVED; RESTARTING`. The board restarts to the launcher, briefly reconnecting USB; select Hermes again. Wi-Fi credentials and Hermes pairing data are retained in the `hgadget` namespace. If NVS is unavailable, setup reports an error and does not erase it automatically.

When the device displays a pairing code, approve it from the Hermes host:

```sh
hermes gadget pair
```

Confirm the device name and pairing code shown by the command. After approval, the device should return to Ready.

On the tested host, Hermes used local `faster-whisper` with the `base` model on CPU/int8. Its first voice request took longer while the model downloaded and loaded. With idle unloading disabled, later requests reuse the model while the gateway process remains running. Your speech provider and configuration may differ; model startup is separate from the audio clip's duration.

### Command format and limits

- `WIFI "ssid" "password"`: SSID is required and may be up to 32 bytes; password may be empty and may be up to 63 bytes.
- `SERVER "url"`: URL may be up to 255 bytes and must start with `ws://` or `wss://` followed by a host.
- Wi-Fi quoted values may contain a literal quote as `\"` and a literal backslash as `\\`. These are the only supported escapes. `SERVER` URLs additionally reject decoded quotes and backslashes. Other control characters are rejected. The complete input line is limited to 511 bytes.
- Values are entered directly into the serial terminal, not through a shell. Do not include real credentials in shell history, screenshots, logs, or shared examples.

If a command is malformed, the board replies with a syntax error and stays running. `ERR WIFI SAVE FAILED`, `ERR SERVER SAVE FAILED`, or `ERR NVS UNAVAILABLE` means the setting was not confirmed saved; use `STATUS`/`DIAG` to inspect reported state and errors.

Wi-Fi disconnections show the ESP-IDF reason number and signal strength on screen and as `WIFI_REASON`/`WIFI_RSSI` in `DIAG`. Reason 4 means an association timeout or an inactivity disconnect from the access point, 201 means no access point was found, 202 means authentication failed, and 204 means the handshake timed out. A successful IP connection clears the reason to zero. These diagnostics do not display credentials.

The T-Embed profile allocates eligible Wi-Fi/LwIP memory in PSRAM and retains four internal static TX buffers for DMA. Ordinary allocations larger than 1024 bytes, including the WebSocket client's 4096-byte receive and transmit buffers, prefer PSRAM. A 32 KiB internal reserve supports task stacks and DMA; the SDK keeps allocations that require internal memory there.

## Status and diagnostics

Send these read-only commands from the Hermes app's serial console:

```text
STATUS
DIAG
```

`STATUS` reports whether Wi-Fi and a server URL are configured, Wi-Fi up/down, the Hermes screen state, pairing state, mic/speaker readiness, and NVS readiness. `DIAG` reports free/largest internal memory, free PSRAM, and NVS, Wi-Fi, mic, and speaker error names. Neither command prints the SSID, password, server URL, or pairing key. `HELP` lists all supported commands; there is no command for reading stored credentials.

## Speaker wiring

The Hermes speaker output is I2S for a MAX98357A amplifier: T-Embed GPIO7 is BCLK, GPIO5 is WS/LRCLK, and GPIO6 is DOUT to the amplifier's data input. Share ground and supply the amplifier according to its board/module specifications. Connect the speaker to the amplifier outputs; do not connect a passive speaker directly to an ESP32 GPIO.

## Knob controls

- In the launcher, rotate to Hermes and click to open it.
- In Hermes Ready or while viewing a reply, hold the knob for about 300 ms to start push-to-talk. Wait for **Listening**, speak, then release to send.
- While Thinking or Speaking, a hold cancels the active turn and begins a new recording. A short click opens the action menu.
- While Listening, turn the knob to cancel/discard the recording; the release is consumed and does not send it.
- In the action menu, rotate to select and click to activate. When Hermes is online and paired, it offers Cancel Turn, New Conversation, Volume, Home, and Back. Back is the default selection. New Conversation requires a second confirmation, defaulting to No; Home is an explicit selection.
- For an approval prompt, rotate to read/scroll the prompt, then click to open the answer view. It defaults to Deny. Rotate to choose, then make a fresh click after the 600 ms arming window to send the correlated answer. A held press cannot approve a prompt, and approval never uses voice capture.
- While offline or pairing, recording is disabled. Use the menu to return Home or Back.

## Build and validation

From the repository root, source `export.sh` from an ESP-IDF checkout at the T-Embed profile's pinned revision (`25fe69f946311abdaf9ad56591f25fedbc20ac98`). The build helper checks that `IDF_PATH` points to this exact commit, then builds and exports the T-Embed profile:

```sh
git clone https://github.com/espressif/esp-idf.git ../esp-idf-hermes-t-embed
git -C ../esp-idf-hermes-t-embed checkout 25fe69f946311abdaf9ad56591f25fedbc20ac98
git -C ../esp-idf-hermes-t-embed submodule update --init --recursive
../esp-idf-hermes-t-embed/install.sh esp32s3
source ../esp-idf-hermes-t-embed/export.sh
python3 tools/build_firmware.py --profile t-embed --version local --output artifacts
```

The output profile directory (`artifacts/t-embed`) must not already exist; choose a fresh `--output` directory if needed. Inspect the generated combined map and image/partition sizes before flashing. To flash the just-built profile, substitute the T-Embed's serial port:

```sh
IDF_COMPONENT_MANAGER=1 python3 "$IDF_PATH/tools/idf.py" \
  -C "$PWD" -B "$PWD/build-t-embed" \
  -p /dev/cu.usbmodemXXXX flash
```

Hardware validation confirmed Wi-Fi connection, gateway pairing, and a fast voice conversation on the board. Prompt handling and repeated app transitions have not been comprehensively tested; a successful compile alone does not verify those behaviors.
