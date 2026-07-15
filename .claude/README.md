# AURA — Project Context for Claude Sessions

> Share this file (or point Claude at `.claude/README.md`) when starting a new
> chat about this project. It documents the architecture, every decision made,
> the hardware quirks, and how to build/flash/test. Last updated: 2026-07-15.

## What AURA is

AI-assisted smart glasses. A **Seeed XIAO ESP32-S3 Sense** (OV2640 camera,
PDM mic, microSD on the Sense expansion board, 8 MB PSRAM) records life
continuously and serves a web app that an **iPhone** uses over WiFi. AI
(OCR, scene description, audio transcription, daily journal summaries) runs
via the **Gemini API free tier**, called directly from the browser.
A **MAX98357A I2S amp + speaker** (connected 2026-07-15) plays tones and
speech: DIN=GPIO1 (D0), BCLK=GPIO2 (D1), LRC=GPIO3 (D2), Vin=5V.

## Architecture (all decided with the user — don't relitigate)

- **iPhone is the target client.** Web Bluetooth is impossible on iOS and live
  video was required → **WiFi via iPhone Personal Hotspot** (`WikumIphone` /
  `wikum1234`), no router needed at runtime. Dev happens on `JCK-WiFi`
  (`12i405b3`) so the PC can reach the device. `WiFiMulti` tries both; DHCP.
- **The ESP32 serves the web app itself** at `/` over plain HTTP (iOS blocks
  mixed content, so an HTTPS-hosted app could never reach the device). App is
  gzipped into `app_index.h` by `tools/build_web.py`. iOS "Add to Home Screen"
  gives the app feel; no service worker possible on HTTP (accepted).
- **Device is a standalone logger** (works with phone away): continuous audio
  → SD in rotating WAV segments; periodic max-quality photos → SD. The app,
  when open, pulls new segments, transcribes them with Gemini, and writes
  transcripts + a rolling daily summary **back to the SD card** as logs.
- **AI = Gemini API** from the browser (`x-goog-api-key` header). Key entered
  in app Settings (localStorage). Free tier is Flash-models-only since
  Apr 2026; `gemini-2.5-flash` is retired for new users. App defaults to
  `gemini-3.5-flash` and has a **Load** button that fetches the key's actual
  model list from `/v1beta/models` — use that, never hardcode model names.
- mDNS: device is `http://aura.local` (works on iOS and Win11).
- NTP time (offset `gmt_off` minutes in config, default 330) for filenames.

## Repo layout

```
firmware/AuraGlasses/     ← THE firmware (CamServer/, GlassesFirmware/ are old, kept as rollback)
    AuraGlasses.ino       WiFi/mDNS/NTP, camera init, Preferences config, periodic photos, autostart
    app_httpd.cpp         all HTTP endpoints (port 80 + MJPEG on 81)
    audio.cpp/.h          PDM mic: capture task → PSRAM ring (512 KB) → SD writer task, WAV rotation
    speaker.cpp/.h        MAX98357A on I2S1 (STD TX): tones + streamed WAV playback
    sdcard.cpp/.h         SD SPI init + global mutex (ALL SD access via sd_lock/sd_unlock)
    config.h              AuraConfig struct (persisted to NVS by the .ino)
    app_index.h           GENERATED — never edit (tools/build_web.py)
app/index.html            web app source (single file, vanilla JS, no build step)
tools/build_web.py        gzip app + generate icon → app_index.h
tools/flash.ps1           build_web + compile + flash in one command (USE THIS)
server/video_streamer.py  legacy PC BLIP captioning client (obsolete)
```

## Hardware / tooling quirks (cost real debugging time)

1. **Flashing**: `arduino-cli upload` FAILS on this board ("Wrong boot mode
   detected (0x8)"). Flash via esptool with `--before usb_reset` —
   `tools/flash.ps1` does everything: `powershell -File tools\flash.ps1 -Port COM8`.
2. **FQBN**: `esp32:esp32:XIAO_ESP32S3:PSRAM=opi,PartitionScheme=default_8MB`
   (PSRAM is *disabled* by default → camera init fails without `opi`; no
   `huge_app` scheme exists for this board, `default_8MB` = same 3 MB app).
3. **COM port**: board is **COM8** (USB VID_303A PID_1001). A stale Windows
   Bluetooth SPP port once claimed the COM8 name ("semaphore timeout" on
   open) — fixed by disabling the BTHENUM COM8 device (elevated, 2026-07-14).
4. **Serial monitoring resets the chip**: opening COM8 with DTR/RTS asserted
   drops it into ROM download mode (prints only `ESP-ROM:esp32s3-...`).
   Recover: `esptool --before no_reset --after hard_reset read-mac`.
   Prefer HTTP `/health` for status checks over serial.
5. **framesize enum drift**: esp32 core 3.3.10's camera driver added
   `FRAMESIZE_128X128` and `FRAMESIZE_320X320`, shifting values (UXGA = 15
   now, not 13). Firmware uses `FRAMESIZE_*` macros only; `/config` returns a
   `sizes` table the app uses to build its picker. Never hardcode numbers.
   (NVS key renamed `fsize` → `fsize2` to invalidate the old stored value.)
6. **ESP32-S3 PDM RX only exists on I2S0** (git history's `I2S_NUM_1` code
   never worked). Core 3.x uses `ESP_I2S.h` (`setPinsPdmRx(42, 41)`).
   The MSM261 mic has a DC offset and is quiet → firmware applies a one-pole
   ~20 Hz high-pass + 4× digital gain in the capture task.
7. **`crypto.randomUUID` doesn't exist on plain-HTTP origins** — the app has
   a `uid()` fallback. Same class of bug: check any Web API for secure-context
   requirements before using it in this app.
8. SD card: Sense expansion board, SPI CS=**GPIO21** (sacrifices the user
   LED), default SPI = SCK 7 / MISO 8 / MOSI 9. `SD.begin(21)`.
10. **Speaker/I2S**: mic (PDM RX) owns I2S0, so the MAX98357A runs STD TX on
    **I2S1** — `speaker_init()` must be called AFTER `audio_init()` so the
    ESP_I2S auto-allocation assigns controllers in that order. Both run
    simultaneously (verified: mic recorded the speaker's own output).
    Speaker verification trick without ears: play `/tone?freq=880` while a
    mic clip records, then Goertzel-detect 880 Hz in the WAV (160× margin).
9. Dev environment: Windows 11, PowerShell 5.1, scoop, python 3.10, node.
   arduino-cli 1.5.1 + esp32 core 3.3.10 installed at
   `%LOCALAPPDATA%\Arduino15`. Device on JCK-WiFi was 192.168.8.101, PC .102.

## HTTP API (port 80 unless noted; CORS `*` everywhere)

| Endpoint | Notes |
|---|---|
| `GET /` | the web app (gzipped, from app_index.h) |
| `GET /capture` | single JPEG at configured framesize/quality |
| `GET :81/stream` | MJPEG (~12 fps at SVGA; on-demand only, app no longer autostarts it) |
| `GET /audio/start` `/audio/stop` `/audio/status` | manual clips; stop returns the WAV; saved to SD as `/rec/clip_*.wav` (RAM fallback ≤16 s if no SD) |
| `GET /rec/start` `/rec/stop` `/rec/status` | continuous logging; rotating segments `/rec/rec_YYYYMMDD_HHMMSS.wav` |
| `GET /sd/list?dir=/rec` | JSON `[{name,size}]` |
| `GET /sd/file?path=...` | download (wav/jpg/text content-type) |
| `GET /sd/delete?path=...` | delete file |
| `POST /sd/write?path=...&mode=a\|w` | body → file (app writes journal logs) |
| `GET /config` | get/set: `cap_interval, framesize, quality, seg_sec, rec_auto, gmt_off`; returns `sizes` table; persists to NVS |
| `GET /health` | ip/ssid/rssi/heap/psram/recording/continuous/sd/sd_free_mb |
| `GET /tone?freq=&ms=&vol=` | sine beep through the speaker (hardware test / notifications) |
| `POST /speak?vol=` | body = 16-bit PCM WAV (mono/stereo, 8–48 kHz) → streamed to the amp; 409 if busy |
| `GET /pad` | buttons: `{d5,d6}` (D5=capture, D6=clip in app) |
| `GET /status` `/control` | camera sensor get/set (stock) |

SD layout: `/rec/*.wav` (audio), `/img/img_<ts>.jpg` (periodic photos),
`/logs/YYYY-MM-DD_transcripts.md` + `YYYY-MM-DD_summary.md` (written by app).

## App behavior (app/index.html)

Tabs: **Live** (stream OFF by default; Capture/Burst/Analyze-now),
**Rec** (continuous toggle + status; manual clip; **Journal**: "Process new"
transcribes unprocessed SD segments → appends transcripts → asks Gemini to
rewrite the daily summary → writes both to SD `/logs/`; auto-process option
runs every 3 min while app open; recordings list with play/delete),
**Gallery** (IndexedDB, per-item OCR/Describe/Transcribe/Ask),
**Settings** (Gemini key + model Load button; device host; burst; device
config: image size/quality, photo interval, segment length, boot autostart —
saved to device NVS via /config).

Processed-segment tracking: `localStorage aura.processed` (capped 800).
Current-recording file is skipped. Segments <0.5 s skipped. "(no speech)"
responses are marked processed but not logged.

## Verified working (2026-07-15, on-device tests from PC)

- 60 s segments rotate exactly (1,920,044 B = 44 + 60×32000), timestamps from
  NTP, 0 dropped bytes; auto-start on boot works.
- Clip during continuous: returns clip WAV, continuous resumes on new segment.
- Periodic photos land in /img every 60 s; captures now true UXGA 1600×1200.
- sd list/file/write/delete, config persistence, /health SD fields — all pass.
- Audio quality: DC removed, speech-level signal after 4× gain.
- Gemini endpoint reachable; error shape matches app handling. **Full AI flow
  (transcribe/summary) not yet exercised — needs the user's API key on-device.**

## Known limitations / next steps

- Journal summaries require the app to be open (browser does the AI calls).
- Gemini free tier: requests are serialized in the app; 429 shows a toast.
- "🔊 Speak" tries Gemini TTS (model auto-discovered from the key's model
  list, cached as `aura.ttsModel`); if the key has no TTS model it falls back
  to phone-side `speechSynthesis`. **Gemini TTS path untested** (needs the
  user's key) — the device side (`/speak`) is fully verified.
- iPhone end-to-end checklist still pending: hotspot join, aura.local in
  Safari, Add to Home Screen, all features over cellular.
- No SD space management yet — old recordings/photos accumulate until the
  card fills (30 GB card ≈ weeks of audio; photos 160 KB×1/min ≈ 230 MB/day).
- BLE-primary connectivity was ruled out (no Web Bluetooth on iOS); a SoftAP
  credential-setup portal was offered but not yet requested/built.
- `server/` BLIP streamer is obsolete.
- Repo changes are **uncommitted on `main`** (user hasn't asked to commit).
- Device DHCP address moves around (was .101, then .100) — always use
  `aura.local` or check serial/`arduino-cli board list`; the PC-side curl
  tests should use mDNS.
- 2026-07-15: user changed device config via app (rec_auto=false, framesize
  HD, quality 5) — respect these; don't silently revert.

## Session history (condensed)

1. **Session 1 (2026-07-14/15)**: Explored repo (was stock CameraWebServer
   clones + BLIP PC script; README described removed audio code). User goals:
   PWA for image capture, audio record, OCR, STT, AI detection. Decisions:
   iPhone → hotspot WiFi (BLE impossible), device mic (not phone), Gemini
   free tier, ESP32 serves app. Built firmware/AuraGlasses + app/ + tools/;
   fixed pre-existing `/pad` bug (registered before httpd_start → 404);
   installed toolchain; solved COM8/BT conflict + usb_reset flashing; audio
   DC/gain fix; deployed & verified all endpoints. Fixed `crypto.randomUUID`
   (insecure origin) and retired `gemini-2.5-flash` (added dynamic model
   Load). 
2. **Session 2 (2026-07-15)**: User added SD card + new requirements:
   continuous timestamped recording with AI journal summaries stored on SD,
   periodic max-quality photos, stream on-demand only, user-selectable image
   quality, manual clips. Implemented sdcard/config/audio-pipeline rewrite
   (ring buffer → SD writer, segment rotation, clip-in-continuous), /rec /sd
   /config endpoints, NTP, NVS config, app Rec/Journal tab + device settings.
   Found & fixed framesize enum drift (UXGA=15). All verified on device.
3. **Session 2 cont.**: Discussed BLE-primary connectivity (impossible on
   iOS web app; hotspot flow already router-free; SoftAP portal offered).
   User wired a MAX98357A amp + speaker per my pin recommendation
   (DIN=GPIO1, BCLK=GPIO2, LRC=GPIO3, Vin=5V). Added speaker.cpp (I2S1),
   `/tone` + `/speak` (streaming WAV playback with stereo downmix), app
   "🔊 Speak" summary button (Gemini TTS w/ auto-discovery → /speak, phone
   speechSynthesis fallback) and Settings "Test speaker" button. Verified on
   hardware via mic loopback: 880 Hz tone detected at 160× background, and a
   Windows-SAPI speech WAV streamed to /speak played audibly (mic RMS 502 vs
   ~170 baseline).
