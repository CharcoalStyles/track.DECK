# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project plan

`PROJECT_PLAN.md` is the working roadmap from empty repo to full feature set — environment
setup, a bring-up flash test, then an ordered feature list with test criteria and possible
backend-side changes noted per feature. It's a checklist meant to be worked through
incrementally across sessions — check current progress there before starting work, and
check items off as they're completed.

## Repository state

This repo currently contains **no firmware source code** — only two spec documents. The
ESP-IDF project (`main/`, `CMakeLists.txt`, `sdkconfig.defaults`, components, etc.) has not
been created yet. Read both docs in full before writing any code:

1. **`ESP32-S3-ePaper-hardware-reference.md`** — read this first. Authoritative reference for
   board identity, pin assignments, peripheral drivers, and ESP-IDF configuration, extracted
   directly from Waveshare's official example repo (`waveshare/ESP32-S3-ePaper-1.54`,
   `02_Example/ESP-IDF/V2/`).
2. **`ESP32_FIRMWARE_SPEC.md`** — the network/behavioral contract this firmware must implement
   against its backend (a separate FastAPI + LangGraph project, not in this repo). Builds on
   top of the hardware doc rather than repeating it.

Once the ESP-IDF project exists, this file should be updated with the actual build/flash/test
commands used in that project.

## What this device is

A dual-purpose device, one physical board (Waveshare ESP32-S3-ePaper-1.54 V2, non-touch):

1. **Push-to-talk voice input** — press a button, speak, release; audio uploads to the
   backend fire-and-forget (no reply over HTTP, ever).
2. **Periodic deep-sleep sync + eink display** — wakes on an RTC alarm, pulls a 24h snapshot
   (check-ins, reminders, calendar events, weather) from the backend, renders it to the eink
   display, goes back to sleep.

**Priority framing**: this device is a display/input surface, not the delivery mechanism.
The backend's own scheduler and push notifications (to the user's phone) are what actually
fire reminders — regardless of whether this device is online. A sync failure or stale
display is a degraded UX, never a missed reminder. Firmware must never let a retry loop,
crash, or sync failure burn battery or brick the device.

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
    pipeline duration, defeating the design). No code path should wait for a reply.
  - `POST /device/checkin/{checkin_id}/skip` — dismiss a live check-in. Note: this is
    distinct from `POST /checkin/{checkin_id}/skip` (no `/device` prefix), which is a
    different backend flow that doesn't accept the device auth token.
- Error handling: bounded retry with backoff (2-3 attempts) per wake cycle on sync failure,
  then sleep for last-known-good `poll_interval_seconds` (or a hardcoded fallback). Never
  retry indefinitely or busy-loop. Malformed/partial JSON should degrade only the affected
  display section, not abort the cycle.

## Hardware summary (see ESP32-S3-ePaper-hardware-reference.md for full detail)

- Target `esp32s3`, ESP-IDF 5.5.1, 4MB flash QIO, **Octal** SPI PSRAM (V2-specific — not
  Quad), single-factory partition table with **no OTA slot**.
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
- No offline queueing or delta sync (every sync is a fresh full snapshot).
- No synthesized TTS reply (`/voice` has no response content at all).
