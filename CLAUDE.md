# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project plan

`PROJECT_PLAN.md` is a historical record of this project's initial buildout — environment
setup, a bring-up flash test, then the F1-F9 feature list with test criteria and backend-side
notes per feature. That roadmap is complete; it's kept for reference (hardware bring-up
decisions, rationale behind existing features) but isn't a live backlog. New work from here is
ad-hoc — no need to check it before starting work or add entries to it.

## Repository state

The ESP-IDF project is built out and the F1-F9 feature roadmap in `PROJECT_PLAN.md` is
complete (see `README.md` for build/flash/test commands). Two reference docs remain
authoritative for anything not obvious from the code itself:

1. **`ESP32-S3-ePaper-hardware-reference.md`** — board identity, pin assignments, peripheral
   drivers, and ESP-IDF configuration, extracted directly from Waveshare's official example
   repo (`waveshare/ESP32-S3-ePaper-1.54`, `02_Example/ESP-IDF/V2/`). Consult before touching
   pin assignments or peripheral init code.
2. **`ESP32_FIRMWARE_SPEC.md`** — the network/behavioral contract this firmware implements
   against its backend (a separate FastAPI + LangGraph project, not in this repo).

Almost all firmware logic lives in `main/adhi-firmware.cpp`, with board support split out into
`components/port_bsp` (display, codec, i2c, sdcard, power, adc, shtc3), `components/sync_proto`
(snapshot parsing, backoff, TZ apply — has host-native unit tests), `components/codec_board`
(ES8311 abstraction), and `components/my_button`.

## What this device is

A dual-purpose device, one physical board (Waveshare ESP32-S3-ePaper-1.54 V2, non-touch):

1. **Push-to-talk voice input** — press a button, speak, release; audio streams straight to
   an SD-card file as it's captured (not buffered in PSRAM — recordings can run several
   minutes) and uploads to the backend fire-and-forget (no reply over HTTP, ever). A recording
   whose upload fails (no wifi, or the backend unreachable) stays queued on the SD card and is
   retried automatically on a later wake — see the non-goals section below for its bounds.
2. **Periodic deep-sleep sync + eink display** — wakes on an RTC alarm, pulls a 24h snapshot
   (check-ins, reminders, calendar events, weather) from the backend, renders it to the eink
   display, goes back to sleep.
3. **Exact-time reminder wake + chime** (F9) — the device also wakes itself for a reminder's
   exact `due_at`, independent of the regular poll interval, and plays a short chime through
   the speaker. This wake never performs a network sync — it works entirely from a compact
   reminder cache refreshed at the last full sync, so it costs no wifi/backend round-trip.

**Priority framing**: for check-ins and calendar events, this device is a display/input
surface, not the delivery mechanism — the backend's own scheduler and push notifications (to
the user's phone) are what actually fire those, regardless of whether this device is online.
Reminders are the one exception: this device wakes and chimes for them directly (F9), in
addition to whatever the backend/phone side does. For everything else, a sync failure or
stale display is a degraded UX, never a missed reminder. Firmware must never let a retry
loop, crash, or sync failure burn battery or brick the device.

## Backend network contract (see ESP32_FIRMWARE_SPEC.md for full detail)

- Base URL is a real HTTPS domain (`BACKEND_BASE_URL` constant) — use
  `esp_crt_bundle_attach` for cert validation, no self-signed handling needed.
- Every request carries a header literally named `auth: <API_TOKEN>` — **not**
  `Authorization`. This is a deliberate backend convention; do not "correct" it. `API_TOKEN`
  is a hardcoded firmware constant, rotated only by reflashing.
- Three endpoints, all requiring the `auth` header:
  - `POST /device/sync` — main sync call, every wake. Sends optional telemetry (battery_mv,
    wake_reason, firmware_version, rssi_dbm, time_awake_ms, reset_reason), receives a full
    24h snapshot (no delta/cursor concept — always a complete replace). Apply
    `timezone.posix` via `setenv("TZ", ...); tzset();`, correct RTC from `now`, schedule next
    wake at `now + poll_interval_seconds` (server-controlled, always use the freshest value).
  - `POST /voice` — multipart audio upload (16kHz/mono/16-bit PCM WAV recommended). Response
    is `202` with an **empty body** — fire-and-forget by design. Never send `one_shot`/`sync`
    form fields (dashboard-testing-only; `sync` would hold the connection open for the full
    pipeline duration, defeating the design). No code path should wait for a reply. Upload
    timeout is sized dynamically from payload length, not a flat constant (a fixed timeout
    proved too short for longer recordings — see `voice_upload_timeout_ms()` in
    `main/adhi-firmware.cpp`). Still only one upload attempt per recording per wake cycle (no
    bounded-retry loop within a single wake, unlike `/device/sync` below) — a failed attempt is
    instead persisted to `/sdcard/pending_voice/` and retried, one attempt each, on every later
    wake that has wifi. See the "Explicit non-goals for v1" section for the queue's bounds.
  - `POST /device/checkin/{checkin_id}/skip` — dismiss a live check-in. Note: this is
    distinct from `POST /checkin/{checkin_id}/skip` (no `/device` prefix), which is a
    different backend flow that doesn't accept the device auth token.
- Error handling: bounded retry with backoff (2-3 attempts) per wake cycle on sync failure,
  then sleep for last-known-good `poll_interval_seconds` (or a hardcoded fallback). Never
  retry indefinitely or busy-loop. Malformed/partial JSON should degrade only the affected
  display section, not abort the cycle.

## Firmware versioning

`firmware_version` (sent on every `/device/sync` request, and on `/device/error` reports) must
be bumped at least once before every commit that changes `main/` or any `components/` source —
currently a hardcoded literal string in `device_sync()`'s request-body construction
(`main/adhi-firmware.cpp`). In practice it has drifted badly: it was last bumped
(`0.1.0-bringup` → `0.2.0-f1`) at the Phase 1→Phase 2 transition and never touched since,
despite many feature commits landing after it — which makes it useless for correlating
backend-reported telemetry/errors with what code was actually running on the device at the
time. The bumped value must be included in the same commit as the change it describes, not a
separate follow-up commit.

## Hardware summary (see ESP32-S3-ePaper-hardware-reference.md for full detail)

- Target `esp32s3`, ESP-IDF 5.5.1, 8MB flash QIO (this physical unit is an ESP32-S3-PICO-1
  SiP with 8MB embedded flash — the hardware doc's 4MB figure doesn't apply here), **Octal**
  SPI PSRAM (V2-specific — not Quad), single-factory partition table with **no OTA slot**.
- E-paper: SSD1681/SSD1680-family, custom bespoke driver (not GxEPD2) — copy from
  `02_Example/ESP-IDF/V2/11_FactoryProgram/components/port_bsp/port_display.{h,cpp}`. Use
  `EPD_DisplayPart()` for routine partial refreshes, `EPD_Display()` full refresh ~daily to
  clear ghosting.
- Audio: single ES8311 codec for both mic and speaker (not discrete mic/amp ICs), via
  `espressif/esp_codec_dev` + the `codec_board` abstraction (board id `S3_ePaper_1_54`).
- SD card: SDMMC 1-bit mode (not SPI, despite some SPI-style pin naming in headers).
- Deep sleep: EXT1 wake (not EXT0) on BOOT/PWR/RTC-INT pins; must explicitly
  `rtc_gpio_hold_en` the VBAT rail (GPIO17) or the board browns out during sleep. RTC alarms
  via the PCF85063 (`waveshare/pcf85063a` component), not the ESP32's own RTC timer.
- Extra buttons: use the `espressif/button` component (wrapped in this repo's
  `my_button/button.{h,cc}` pattern), not hand-rolled GPIO polling.
- Full pin map, reserved/strapping GPIOs to avoid, and component-manager dependency list are
  in the hardware doc — don't guess these values.

## Explicit non-goals for v1

- No OTA updates (no OTA partition slot — reflash only).
- No multi-device/fleet concept (backend assumes exactly one device).
- No offline queueing or delta sync **for `/device/sync`** (every sync is a fresh full 24h
  snapshot — no delta/cursor concept). **Exception:** failed push-to-talk voice-note uploads
  (`/voice`, see the backend network contract above) *are* queued to the SD card
  (`/sdcard/pending_voice/`, `flush_pending_voice_notes()` in `main/adhi-firmware.cpp`) and
  retried automatically. This is a deliberately narrow, bounded exception: max 15 pending notes
  (oldest evicted FIFO past that), exactly one retry attempt per note per wake (no per-note
  backoff loop, no reuse of `sync_backoff.c/h`), a 5-minute total connected-upload-time budget
  per wake across all queued notes, and no expiry by age or attempt count. It does not apply to
  `/device/sync`, which remains fully stateless with no delta/offline-queue concept at all.
- No synthesized TTS reply (`/voice` has no response content at all).
