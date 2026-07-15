# AURA

AURA (Augmented Understanding & Response Assistant) is an open-source
AI-assisted smart glasses project powered by the Seeed Studio XIAO ESP32-S3
Sense. The glasses capture what the wearer sees and hears, and a web app on
your phone views the live camera, captures photos, records audio from the
on-board mic, and runs AI on it all (OCR, scene description/detection, and
speech transcription) via the Gemini API.

## How it works

```
┌───────────────────────┐   WiFi (iPhone hotspot   ┌──────────────────────┐
│ XIAO ESP32-S3 Sense   │    or home network)      │ Phone / PC browser   │
│  OV2640 camera        │ ◄──────── HTTP ────────► │  AURA web app        │
│  PDM microphone       │   MJPEG / JPEG / WAV     │  (served BY the      │
│  serves the web app   │                          │   glasses at /)      │
└───────────────────────┘                          └──────────┬───────────┘
                                                              │ HTTPS
                                                              ▼
                                                     Gemini API (OCR, vision,
                                                     audio transcription)
```

No PC or router is required at runtime: the glasses join your iPhone's
Personal Hotspot, you open `http://aura.local` in Safari, and the phone's
cellular connection handles the AI calls.

## Hardware

* **Seeed Studio XIAO ESP32-S3 Sense** — WiFi, dual-core MCU, 8 MB PSRAM,
  on-board OV2640 camera and PDM microphone, microSD slot.
* **MAX98357A I2S amplifier + speaker** — DIN=GPIO1 (D0), BCLK=GPIO2 (D1),
  LRC=GPIO3 (D2), Vin=5V. Runs on I2S1 (the mic owns I2S0), so it can play
  while recording.
* Two touch/button inputs on GPIO5 (D5 = capture) and GPIO6 (D6 = record audio).

## Firmware (`firmware/AuraGlasses/`)

Arduino sketch for the ESP32 core 3.x. It:

1. Joins the first available known network (`WiFiMulti`) and announces itself
   as `aura.local` via mDNS. Edit `KNOWN_APS` in `AuraGlasses.ino` for your
   hotspot/network credentials.
2. Serves the AURA web app (gzipped into `app_index.h`) plus these endpoints:

| Endpoint | Description |
|---|---|
| `GET /` | The AURA web app |
| `GET /capture` | Single JPEG frame (configured size/quality, up to UXGA) |
| `GET :81/stream` | MJPEG live stream (on demand) |
| `GET /rec/start` `/rec/stop` `/rec/status` | Continuous audio logging to SD (rotating WAV segments) |
| `GET /audio/start` `/audio/stop` `/audio/status` | Manual clip; stop downloads the WAV (also saved to SD) |
| `GET /sd/list?dir=` `/sd/file?path=` `/sd/delete?path=` | Browse/download/delete SD files |
| `POST /sd/write?path=&mode=a\|w` | Write logs (used by the app's journal) |
| `GET /config` | Get/set persisted settings: photo interval, image size/quality, segment length, boot autostart, timezone |
| `GET /tone?freq=&ms=&vol=` | Play a beep through the speaker |
| `POST /speak?vol=` | Play a 16-bit PCM WAV through the speaker (used for spoken summaries) |
| `GET /pad` | Touch button state `{d5, d6}` |
| `GET /health` | `{ip, ssid, rssi, heap, psram, recording, continuous, sd, sd_free_mb}` |
| `GET /status`, `/control` | Camera sensor settings |

The glasses work standalone: on boot they join WiFi, sync time via NTP, and
(if enabled) start logging audio segments to `/rec/` and periodic photos to
`/img/` on the SD card. The app's **Rec → Journal** transcribes new segments
via Gemini and keeps a daily summary in `/logs/` on the card.

Older sketches (`CamServer/`, `GlassesFirmware/`) are kept as reference.

### Build & flash

One command (regenerates the embedded app, compiles, flashes):

```powershell
powershell -File tools\flash.ps1 -Port COM8 [-Monitor]
```

Or manually — note this board fails esptool's default reset ("Wrong boot mode
detected"), so `arduino-cli upload` doesn't work; `tools\flash.ps1` flashes via
esptool with `--before usb_reset` instead:

```sh
python tools/build_web.py   # regenerate app_index.h after editing app/index.html
arduino-cli compile -b esp32:esp32:XIAO_ESP32S3:PSRAM=opi,PartitionScheme=default_8MB firmware/AuraGlasses
arduino-cli monitor -p COM8 -c baudrate=115200
```

Gotcha: if opening COM8 fails with "semaphore timeout", a Bluetooth serial
port has grabbed the same COM number — disable the Bluetooth COM port in
Device Manager.

## Web app (`app/index.html`)

Single-file vanilla-JS app, embedded into the firmware. Features:

* **Live** — MJPEG stream with auto-reconnect, photo capture, frame bursts,
  one-tap "Analyze now".
* **Audio** — record from the glasses' mic, play back, transcribe.
* **Gallery** — captures persist in IndexedDB; per-item OCR, description,
  transcription, burst analysis, and free-form "Ask AI" questions.
* **Settings** — Gemini API key (get a free one at
  [aistudio.google.com](https://aistudio.google.com)), model choice, device
  host override, burst tuning.

### Using it on an iPhone

1. Enable Personal Hotspot and stay on the hotspot settings screen; power the
   glasses and wait until they join (watch the serial monitor if unsure).
2. Open `http://aura.local` in Safari (or the device IP shown on serial,
   usually `172.20.10.x`).
3. Share → **Add to Home Screen** for a fullscreen app experience.
4. Paste your Gemini API key in Settings.

### Developing the app without reflashing

Serve the app from your PC and point it at the device:

```sh
python -m http.server 8000 -d app
# open http://localhost:8000, set Settings → Device host to the device IP
```

Reflash (with a fresh `build_web.py` run) when you want the on-device copy
updated.

## Future work

* Wake-word / hands-free capture using the pad buttons.
* SD card space management (auto-prune old recordings/photos).
* SoftAP setup portal for entering WiFi credentials without reflashing.
* Optional local AI server mode (`server/video_streamer.py` shows BLIP
  captioning on a PC).

## License

This project is released under the MIT License. See `LICENSE` for details.
