# AURA

AURA (Augmented Understanding & Response Assistant) is an open-source
AI-assisted smart glasses project powered by the Seeed Studio XIAO ESP32-S3.
The goal is to capture what the wearer sees and hears, send that data to a
more powerful device or cloud service, and then play back spoken responses.
This repository contains firmware sketches, a minimal server and documentation
so you can build your own pair of assistive glasses.

## 1. Hardware Overview

* **Seeed Studio XIAO ESP32-S3 Sense** – provides Wi‑Fi/BLE, a dual‑core MCU and
  an on-board PDM microphone.
* **OV2640 camera module** – captures the user's point of view.
* **MAX98357A I2S amplifier + small speaker** – plays audio responses.
* Optional: momentary button for triggering capture and a Li‑Po battery for
  portable use.

### Wiring

1. Power the MAX98357A from the XIAO's 5V and GND pins.
2. Connect I2S lines:
   * `BCLK` → chosen GPIO for `I2S_BCLK`
   * `LRC`  → chosen GPIO for `I2S_LRCK`
   * `DIN`  → chosen GPIO for `I2S_DOUT`
3. Attach the speaker to the amplifier output.
4. Plug the OV2640 into the camera connector on the Sense board or wire the DVP
   pins manually.

## 2. Firmware

The `firmware/` directory contains a starter Arduino sketch
(`GlassesFirmware.ino`) that:

1. Connects to a Wi‑Fi network.
2. Configures the camera, microphone and I2S amplifier.
3. Demonstrates audio playback by generating a simple test tone via I2S.
4. Provides a loopback test routing microphone input directly to the speaker.

Further development should add routines to record microphone audio, capture
images and POST them to the server for processing. When a response is received,
stream the returned audio to the MAX98357A using the same I2S interface.

## 3. Companion Server

Heavy AI tasks are offloaded to a networked PC or phone. The `server/` folder
contains a minimal Flask application that exposes a `/query` endpoint. It
accepts an audio clip and optional image, then responds with a (currently
silent) WAV file. Replace the placeholder functions in `app.py` with calls to
services such as Whisper, Tesseract, GPT or Google Text‑to‑Speech.

## 4. End-to-End Flow

1. User presses the button or speaks a wake word.
2. XIAO records audio and, if needed, captures an image.
3. Data is sent over Wi‑Fi to the companion server on the same network.
4. The server performs speech recognition, vision analysis and language
   reasoning, then generates a spoken reply.
5. The reply audio is streamed back to the glasses and played through the
   MAX98357A amplifier.

## 5. Future Work

* Integrate real STT, vision and TTS services in `server/app.py`.
* Handle reminders and other assistant features.
* Optimize latency by running lightweight models locally where possible.

## License

This project is released under the MIT License. See `LICENSE` for details.
