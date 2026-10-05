# T-Embed microphone bridge

The T-Embed streams 16 kHz mono microphone frames over its existing USB Serial/JTAG connection. This Mac helper sends them to the BlackHole 2ch virtual audio device, which transcription and recording apps can select as their microphone.

Install BlackHole 2ch on each Mac and restart that Mac:

```sh
brew install --cask blackhole-2ch
```

Create a Python environment and install the bridge packages from the repository root:

```sh
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r tools/requirements-t-embed-mic-bridge.txt
```

Connect the T-Embed with a USB data cable, choose **USB Microphone** in its launcher, and run the helper with its serial device:

```sh
ls /dev/cu.usbmodem*
python tools/t_embed_mic_bridge.py --port /dev/cu.usbmodem1101
```

In the microphone app, press the encoder knob to mute or unmute. The board's LEDs stay dim cyan while the microphone is live and dim red while muted; muted USB frames contain silence.

Select **BlackHole 2ch** as the microphone in the app that should receive the T-Embed audio. Keep the helper running while using the microphone. Press Ctrl+C to stop the bridge.
