# AURA — Project Context for Claude Sessions

> Share this file (or point Claude at `.claude/README.md`) when starting a new
> chat about this project. It documents the architecture, every decision made,
> the hardware quirks, and how to build/flash/test. Last updated: 2026-07-16.

## What AURA is

AI-assisted smart glasses **usable by a blind person for navigation**. A
**Seeed XIAO ESP32-S3 Sense** (OV2640 camera, PDM mic, microSD on the Sense
expansion board, 8 MB PSRAM) records life continuously, serves a web app that
an **iPhone** uses over WiFi, and — standalone, phone locked or absent —
answers touch-pad gestures by **capturing a photo, asking Gemini to describe
the scene or read visible text, and speaking the answer through its speaker**.
Peripherals:
- **MAX98357A I2S amp + speaker**: DIN=GPIO1 (D0), BCLK=GPIO2 (D1),
  LRC=GPIO3 (D2), Vin=5V (I2S1; mic owns I2S0).
- **Two TTP223 capacitive touch pads** (replaced plain buttons 2026-07-16):
  D5=GPIO5, D6=GPIO6, **active HIGH** momentary, firmware uses INPUT_PULLDOWN.
  Tap vs ~0.6 s hold = 4 remappable action slots (see /config `actions`).

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
- **AI = Gemini API**, called from BOTH sides:
  - **Browser** (journal transcription/summary, gallery OCR/describe/ask):
    `x-goog-api-key` header, key in localStorage. Free tier is
    Flash-models-only since Apr 2026; `gemini-2.5-flash` is retired. App
    defaults to `gemini-3.5-flash` and has a **Load** button that fetches the
    key's actual model list — never hardcode model names.
  - **Device** (pad-triggered describe/read/status, decided 2026-07-16 so it
    works with the phone locked): `gemini.cpp` does HTTPS with the ESP x509
    cert bundle (NOT setInsecure), streams the base64 JPEG up, parses the
    small vision response with cJSON, then calls Gemini TTS and scans the
    ~1 MB response incrementally (never JSON-parsed) into PSRAM PCM →
    speaker. Key pushed from app Settings via `POST /apikey` → NVS ns
    `"aura-sec"`; never echoed (only `key_set` booleans).
- mDNS: device is `http://aura.local` (works on iOS and Win11).
- NTP time (offset `gmt_off` minutes in config, default 330) for filenames.

## Repo layout

```
firmware/AuraGlasses/     ← THE firmware (CamServer/, GlassesFirmware/ are old, kept as rollback)
    AuraGlasses.ino       WiFi/mDNS/NTP, camera init (FBs boot-alloc'd at UXGA), NVS config,
                          capture_photo_to_sd(), init order (see below), periodic photos
    app_httpd.cpp         all HTTP endpoints (port 80, max_uri_handlers=32; MJPEG on 81)
    audio.cpp/.h          PDM mic: capture task → 512 KB PSRAM FIFO → SD writer (WAV rotation)
                          + rolling retention window (win_sec, mic always on when >0)
    speaker.cpp/.h        MAX98357A on I2S1 (STD TX): tones + streamed PCM playback
    pads.cpp/.h           TTP223 scan task (20 ms), tap/hold FSM (hold fires AT threshold), chirps
    actions.cpp/.h        action queue + single worker task (16 KB stack) + event ring (/events)
                          + feedback-tone vocabulary (tap 880, hold 2×1320, success, error buzz…)
    led.cpp/.h            GPIO21 status blinks under sd_lock (rec=slow, assist=fast, photo=2 flash)
    camctl.cpp/.h         camera mutex: photo framesize switching, stream/rolling-video arbitration
    video.cpp/.h          rolling VGA@10fps frame ring (2.5 MB PSRAM) + MJPEG-AVI muxer + manual rec
    gemini.cpp/.h         device-side Gemini: vision + TTS pipeline, API key NVS, cert-bundle TLS
    sdcard.cpp/.h         SD SPI init (3 retries) + global mutex (ALL SD access via sd_lock/sd_unlock)
    config.h              AuraConfig struct + AuraAction enum (persisted to NVS by the .ino)
    app_index.h           GENERATED — never edit (tools/build_web.py)
app/index.html            web app source (single file, vanilla JS, no build step)
tools/build_web.py        gzip app + generate icon → app_index.h
tools/flash.ps1           build_web + compile + flash in one command (USE THIS)
server/video_streamer.py  legacy PC BLIP captioning client (obsolete)
```

**Init order in setup() matters**: config → camera (UXGA alloc + apply cfg) →
sd → led → audio (starts mic if win_sec>0) → speaker (AFTER audio: I2S order)
→ camctl → video → gemini → actions → pads → WiFi → NTP/mDNS → httpd →
rec_auto autostart.

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
8. SD card: Sense expansion board, SPI CS=**GPIO21**, default SPI = SCK 7 /
   MISO 8 / MOSI 9. `SD.begin(21)` with 3 retries (a crash mid-write can make
   the first begin fail).
9. **GPIO21 = SD CS = user LED (active LOW)** — the LED is NOT lost: led.cpp
   pulses it ≤25 ms while holding `sd_lock()` (bus idle, no clock → the card
   ignores the CS wiggle) and always leaves it HIGH. Slow blink = recording,
   fast = assist in flight, double flash = photo. Zero dropped audio verified.
10. **Speaker/I2S**: mic (PDM RX) owns I2S0, so the MAX98357A runs STD TX on
    **I2S1** — `speaker_init()` must be called AFTER `audio_init()` so the
    ESP_I2S auto-allocation assigns controllers in that order. Both run
    simultaneously (verified: mic recorded the speaker's own output).
    Speaker verification trick without ears: play `/tone?freq=880` while a
    mic clip records, then Goertzel-detect 880 Hz in the WAV (160× margin).
11. **Task watchdog (5 s) will reboot the chip if a task loops on SD writes
    without yielding** — the video ring dump did exactly that (IDLE0 starved
    → abort → SD unmountable until reset). Any bulk-write loop must
    `vTaskDelay(1)` every few chunks. Same lesson applies to long downloads.
12. `File.size()` on an open FILE_WRITE handle returns stale/0 before flush —
    compute AVI/RIFF sizes arithmetically, don't read them back.
13. Free GPIO: only GPIO4 (D3) remains. Everything else: 1/2/3 speaker,
    5/6 pads, 7/8/9/21 SD(+LED), 41/42 mic, 43/44 UART, rest camera.
14. Dev environment: Windows 11, PowerShell 5.1, scoop, python 3.10, node.
    arduino-cli 1.5.1 + esp32 core 3.3.10 installed at
    `%LOCALAPPDATA%\Arduino15`. Device on JCK-WiFi = 192.168.8.100, PC .102.

### PSRAM budget (8 MB total; guard rails are in code)

| Buffer | Size | When |
|---|---|---|
| Camera FBs (2× UXGA, boot-fixed) | ~0.9 MB | always |
| Audio drain FIFO | 0.5 MB | always |
| Audio window (`win_sec`×32 KB) | 1.9 MB @60 s | win_sec>0, boot-alloc |
| Video ring | 2.5 MB | vid_en (runtime toggle) |
| Gemini transients (jpeg+b64 / TTS PCM cap) | ~0.5 / 1.0 MB | per request |

Boot clamp: `vid_en && win_sec>120` → win_sec=120. With defaults on, ~2.3 MB
stays free — enough for the Gemini peaks. Alloc failures disable the feature
with an event, never crash.

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
| `GET /config` | get/set: `cap_interval, framesize, quality, seg_sec, rec_auto, gmt_off, pad_d5_tap/[d5_hold/d6_tap/d6_hold], hold_ms, vid_en, win_sec, avol, lang`; returns self-describing `sizes` + `actions` tables + `key_set`; persists to NVS |
| `GET /health` | + `assist_busy, video, video_rec, key_set` |
| `GET /tone?freq=&ms=&vol=` | sine beep through the speaker (hardware test / notifications) |
| `POST /speak?vol=` | body = 16-bit PCM WAV (mono/stereo, 8–48 kHz) → streamed to the amp; 409 if busy |
| `GET /pad` | touch state `{d5,d6}` (active-high; display only — actions run in firmware) |
| `GET /pad/sim?slot=d5_tap\|d5_hold\|d6_tap\|d6_hold` | fire a slot's configured action (test hook, same queue as physical touch) |
| `GET /events` | last 8 action results `[{id,ts,type,ok,detail}]` newest-first (app polls for toasts + assist card) |
| `GET /assist?mode=describe\|read\|status` | queue the AI pipeline; 202 immediately; result in /events + spoken |
| `POST /apikey` (body=key, `?gmodel=&tmodel=`) | store Gemini key (empty body clears) + model names; never echoed |
| `GET /audio/window/save` | dump the rolling audio window → `/rec/win_<ts>.wav` (via worker, 202) |
| `GET /video/start` `/video/stop` `/video/status` | manual MJPEG-AVI recording to SD, 60 s cap |
| `GET /video/save` | dump rolling video ring → `/video/vid_<ts>.avi` (via worker, 202) |
| `GET /status` `/control` | camera sensor get/set (stock) |

Pad actions (values in /config `actions` table): 0 none, 1 describe scene,
2 read text, 3 capture photo, 4 save video ring, 5 save audio window,
6 toggle audio clip, 7 speak status. Defaults: d5 tap=1 hold=2, d6 tap=3
hold=4. Hold fires AT the threshold (not release) with a double chirp.

SD layout: `/rec/*.wav` (rec_=segments, clip_=manual, win_=window dumps),
`/img/img_<ts>.jpg` (photos), `/video/*.avi` (vid_=ring dumps, clip_=manual),
`/logs/YYYY-MM-DD_transcripts.md` + `YYYY-MM-DD_summary.md` (written by app).
AVIs are MJPEG 640×480@10fps with idx1 (play in VLC/ffmpeg/Windows; iOS
Safari won't inline-play them — download).

## App behavior (app/index.html)

Tabs: **Live** (stream OFF by default; Capture/Burst/Analyze-now; Record
video + Save-last-10s buttons; assist card showing the latest spoken
description; pad-action hint line from /config), **Rec** (continuous toggle;
manual clip; rolling-window card with Save button; **Journal** as before),
**Gallery** (segmented: 📱 Phone = IndexedDB with per-item AI; 🖼/🎬/🎙 On
device = SD browser over /sd/list with lazy thumbnails, download, delete),
**Settings** (Gemini key + model Load + **Send AI key to glasses** (+ Clear,
key-on-device indicator); device host; burst; device config: image
size/quality, photo interval, segment length, window length, rolling-video
toggle, 4 pad-action pickers built from the `actions` table, hold threshold,
assist volume, spoken language, boot autostart).

The app polls `/events` every 4 s → toasts for pad-action results; pad
polling is display-only (firmware owns the actions — the old JS
capture-on-press was removed to prevent double triggers).

Processed-segment tracking: `localStorage aura.processed` (capped 800).
Current-recording file is skipped. Segments <0.5 s skipped. "(no speech)"
responses are marked processed but not logged.

## Verified working (2026-07-15/16, on-device tests from PC)

- 60 s segments rotate exactly, NTP timestamps, 0 dropped bytes; autostart OK.
- Clip during continuous: returns clip WAV, continuous resumes on new segment.
- Periodic photos land in /img; capture honors configured size (HD verified
  mid-rolling-video — camctl switches the sensor and back).
- Speaker: 880 Hz tone at 160× background via mic loopback; streamed speech
  WAV played; pad chirps confirmed the same way (1568 Hz photo chime, 231×).
- Pads/actions: /pad/sim → worker → photo on SD + /events entry; remap via
  /config works; tap/hold chirps; LED blinks with 0 dropped audio bytes.
- Rolling audio window: 59 s WAV saved containing a tone played 12 s BEFORE
  the save request (Goertzel at correct offset); mic survives rec/clip stops.
- Rolling video: 10.0 fps steady; ring dump + 8 s manual clip both produce
  structurally exact AVIs (RIFF sizes, idx1, 640×480 MJPEG); stream pauses
  rolling and it resumes after; 3-min soak (audio+window+video) heap/PSRAM
  flat, 0 drops.
- Device-side Gemini: full TLS (cert bundle) + photo upload verified — a
  wrong key returned Gemini's "API key not valid" into /events. **Live
  describe/read/TTS still needs the user's real key pushed from the app.**

## Known limitations / next steps

- **User action pending**: open the app → Settings → "Send AI key to
  glasses", then tap D5 to hear the first live description. Physical
  tap/hold feel (chirp timing) also only user-verifiable.
- Journal summaries still require the app open (browser does those AI calls).
- Gemini free tier: device + app requests share the key's rate limits.
- SPEAK_STATUS speaks via Gemini TTS → silent (error buzz) when offline; a
  canned offline voice would need pre-rendered PCM on SD (not built).
- Rolling video ring is byte-bound: ~10 s of detailed scenes, up to ~25 s of
  static ones. AVI has no audio track (WAVs are separate).
- iPhone end-to-end checklist still pending (hotspot, Add to Home Screen).
- No SD space management yet (30 GB card ≈ weeks of audio).
- SoftAP credential portal offered, not requested; `server/` BLIP obsolete.
- Device DHCP address moves (.101→.100) — always use `aura.local` from
  phones; PC curl can use the IP but verify with /health first.
- User's own device config (rec_auto=false, framesize HD, quality 5, and now
  vid_en=true from testing) — respect these; don't silently revert.
- Workflow: **commit each verified milestone to main** (user asked 2026-07-16
  to auto-approve and commit as we go).

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
4. **Session 3 (2026-07-16)**: User replaced the pad buttons with TTP223
   touch sensors and asked for: configurable pad actions incl. "capture and
   explain what's in front of me" for blind navigation, LED feedback, photo
   access/download from the phone, manual video clips, rolling 10 s video
   window, rolling 1-3 min audio window. Decisions (AskUserQuestion): AI runs
   ON THE DEVICE (works phone-locked), balanced PSRAM budget (VGA 10 fps
   video + 60-120 s audio window), assist-focused pad defaults. Built in 4
   flashed+verified phases: (1) pads/actions/events/LED + /pad/sim,
   (2) audio retention window, (3) camctl + video ring + AVI muxer + manual
   rec + SD gallery, (4) gemini.cpp device-side vision+TTS + /apikey +
   /assist. Hardware-debugged: task-watchdog reboot from non-yielding SD dump
   loop (quirk 11), stale File.size() breaking RIFF sizes (quirk 12), SD
   begin retries. User also said "approve all automatically and commit
   changes to git" → committed per phase. Pending: user pushes their Gemini
   key, then live end-to-end assist test.
