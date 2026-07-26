# ESP32-S3 Assistant Device — Firmware Spec

This document specifies the network/behavioral contract between the ESP32-S3 firmware and
the backend it talks to (a self-hosted personal-assistant server, FastAPI + LangGraph). It
is written for a fresh Claude Code session building the firmware, in a separate project from
the backend this doc came from.

**Read `ESP32-S3-ePaper-hardware-reference.md` first** (should sit alongside this file, or
ask for it if it doesn't) — that document is the authoritative source for board identity,
pin assignments, and chip identity (Waveshare ESP32-S3-ePaper-1.54 V2, non-touch, ESP-IDF).
This document does not repeat any of that; it builds on top of it. Where this doc references
a peripheral (RTC alarm, battery ADC, audio codec, buttons), the hardware doc has the pins
and driver APIs.

## 1. What this device is

A dual-purpose device, one physical board:

1. **Push-to-talk voice input** — press a button, speak, release; audio is uploaded and
   transcribed/processed by the backend's LLM agent. Fire-and-forget: the device does not
   wait for or receive a reply over HTTP (see §4.2).
2. **Periodic deep-sleep sync + eink display** — wakes on a schedule (RTC alarm), connects
   to wifi, pulls a 24-hour snapshot of what's coming up (check-ins, reminders, calendar
   events, weather), renders it to the eink display, and goes back to sleep.

**Important framing**: this device is a *display and input surface*, not the delivery
mechanism. The backend's own scheduler (APScheduler) and push notifications (Gotify, to the
user's phone) are what actually fire reminders/check-ins/bedtime nudges — that happens
regardless of whether this device is online, charged, or even exists. If firmware fails to
sync, or the display shows stale data, that's a degraded UX, not a missed reminder. Build
firmware with that priority: never let a sync failure, retry loop, or crash burn battery or
brick the device — worst case is just "the screen is out of date."

## 2. Network & auth

- **Base URL**: the backend is reachable over the public internet via a domain — define a
  `BACKEND_BASE_URL` constant (e.g. `https://YOUR_DOMAIN_HERE`) for the user to fill in
  before building. **HTTPS is required, not optional** — a long-lived bearer credential
  (below) goes over the wire on every request. Use ESP-IDF's standard CA trust store
  (`esp_crt_bundle_attach`) for certificate validation; no self-signed cert handling should
  be needed since this is a real domain with a normal cert.
- **Auth header**: every device-facing request must carry a header literally named `auth`
  (**not** `Authorization`, not `Authorization: Bearer ...`) — this is a deliberate backend
  convention, not an oversight, so don't "correct" it to the standard shape:
  ```
  auth: <API_TOKEN>
  ```
  `API_TOKEN` is a long, random shared secret. Hardcode it as a firmware constant, filled in
  by the user before building (matches the backend's own assumption that this token "lives
  in ESP32 firmware" and is rotated by reflashing, not by any runtime provisioning flow).
  This is the *only* credential the device needs — there's no login flow, no session, no
  per-device identity.
- A request with a missing/wrong `auth` header gets back `401 Unauthorized` from every
  endpoint below.

## 3. Endpoints

Three endpoints, all under `BACKEND_BASE_URL`, all requiring the `auth` header above.

### 3.1 `POST /device/sync` — the main sync call

Call this every wake (scheduled or incidental). Request body is optional telemetry (send
`{}` if you have nothing to report yet — every field is nullable):

```json
{
  "battery_mv": 3985,
  "wake_reason": "timer",
  "firmware_version": "0.1.0",
  "rssi_dbm": -61,
  "time_awake_ms": 2340,
  "reset_reason": "deep_sleep_wake"
}
```

Field meanings (all optional, all firmware-authored — there's no fixed enum enforced
server-side, but use these conventions so the values are meaningful over time):

| Field | Type | Meaning |
|---|---|---|
| `battery_mv` | int | Battery voltage in millivolts. On this board: read `ADC1_CHANNEL_3` (GPIO4), actual voltage ≈ `raw_millivolts * 2` (resistor divider) — see hardware doc. |
| `wake_reason` | string | Why the device woke up. Suggested values: `"timer"` (RTC alarm), `"button"` (BOOT/PWR EXT1 wake), `"power_on"` (cold boot). |
| `firmware_version` | string | Whatever version string firmware wants to self-report — a compile-time constant. |
| `rssi_dbm` | int | Current wifi signal strength. |
| `time_awake_ms` | int | **Milliseconds from wake to the moment the `/device/sync` HTTP response is received.** This is the number that actually validates or refutes the battery-life estimate this project is being built around — measure it precisely, not loosely. |
| `reset_reason` | string | ESP-IDF's `esp_reset_reason()`, stringified. Suggested mapping: `ESP_RST_POWERON`→`"power_on"`, `ESP_RST_DEEPSLEEP`→`"deep_sleep_wake"`, `ESP_RST_BROWNOUT`→`"brownout"`, `ESP_RST_INT_WDT`/`ESP_RST_TASK_WDT`/`ESP_RST_WDT`→`"watchdog"`, `ESP_RST_PANIC`→`"panic"`, anything else→`"unknown"`. This is the only visibility into a crash/brownout during the beta without a debugger attached — worth getting right. |

**Response** — a full 24-hour snapshot, rebuilt from scratch on every call (no delta/cursor
concept — just replace whatever the device had cached with this). `now`, `timezone`,
`poll_interval_seconds`, `bedtime`, `next_wake_at`, `checkins`, `reminders`, and `weather`
below are a real captured example (a check-in and a reminder were live when this was
captured, so their shapes are visible rather than just empty arrays); `calendar_events` is
hand-constructed for illustration since this dev environment has no calendar configured —
the field shape (`uid`/`summary`/`start`/`end`) is accurate, the values are not a live
capture:

```json
{
  "now": 1785096505,
  "timezone": {
    "iana": "Australia/Canberra",
    "posix": "AEST-10AEDT,M10.1.0,M4.1.0/3"
  },
  "poll_interval_seconds": 300,
  "bedtime": "21:20",
  "next_wake_at": 1785099600,
  "checkins": [
    {
      "id": "ae5e8021-05d2-4ad5-96c8-e8c62fe75020",
      "category": "low",
      "prompt_text": "Find the furthest object you can see and focus on it for a few seconds. How do your eyes feel now?",
      "scheduled_at": 1785096504,
      "fired_at": 1785096504
    }
  ],
  "reminders": [
    {
      "id": "8bbcd662-a4c7-4dae-a6c5-e1fd033abd12",
      "message": "Test reminder from the dashboard",
      "due_at": 1785096515,
      "event_uid": null
    }
  ],
  "calendar_events": [
    {
      "uid": "a1b2c3d4-...",
      "summary": "Dentist appointment",
      "start": 1785171600,
      "end": 1785175200
    }
  ],
  "weather": {
    "temperature_c": 0.5,
    "cloud_cover_pct": 100,
    "precipitation_mm": 0.0,
    "temperature_min_c": 0.2,
    "temperature_max_c": 11.6,
    "sunrise": 1785099720,
    "sunset": 1785136620
  }
}
```

Field meanings:

| Field | Type | Meaning |
|---|---|---|
| `now` | epoch seconds | Server's current time. Use this to correct the device's own RTC drift every sync — deep-sleep clocks drift over days. |
| `timezone.iana` | string | IANA zone name (e.g. `"Australia/Canberra"`), informational. |
| `timezone.posix` | string \| null | A **POSIX TZ string** (e.g. `"AEST-10AEDT,M10.1.0,M4.1.0/3"`), extracted from the backend's compiled timezone database — this is what firmware should actually use: `setenv("TZ", timezone_posix, 1); tzset();` gives correct local time/date rendering across DST changes without hardcoding an offset that goes stale twice a year. Nullable — have a fallback (e.g. treat as UTC, or keep the last-known-good value) if it's ever `null`. |
| `poll_interval_seconds` | int | How often (seconds) the device should wake and call this endpoint again. Server-controlled and can change between syncs (it's a dashboard setting on the backend) — always use the freshest value received, don't hardcode it in firmware. Set the next RTC alarm to `now + poll_interval_seconds`. |
| `bedtime` | "HH:MM" string | Local time the user's wind-down period starts — informational, for display if useful. |
| `next_wake_at` | epoch seconds | When the next check-in is expected to fire (or tomorrow's earliest check-in window if none are queued today). Informational only — **do not** use this instead of `poll_interval_seconds` for scheduling the next wake; the flat poll interval is the actual cadence this device runs on. |
| `checkins[]` | array | Pending check-ins due within 24h. `fired_at` is `null` until the check-in has actually fired (i.e. is "live"/awaiting a response) — see §3.3/§4.3 for what to do when it's set. |
| `reminders[]` | array | Pending reminders (both manually-set and calendar-derived) due within 24h. `event_uid` is non-null only for calendar-derived reminders — informational, no action needed based on it. |
| `calendar_events[]` | array | Raw calendar agenda for the next 24h, independent of whether an event has a reminder attached — for display context (e.g. "3pm: Dentist"), not actionable. `end` may be `null` for events with no end time. |
| `weather` | object \| null | Current conditions + today's min/max/sunrise/sunset for the user's configured location. `null` if the weather service was unreachable this cycle — treat as "no weather data this cycle," not an error. `sunrise`/`sunset` are epoch seconds. |

**If a field is missing or the response doesn't parse**: degrade gracefully. Skip rendering
that one section of the display and keep going — don't crash-loop, don't treat it as fatal
to the whole sync cycle.

### 3.2 `POST /voice` — push-to-talk audio upload

Multipart form upload:

| Field | Type | Required | Meaning |
|---|---|---|---|
| `file` | audio file | yes | The recorded audio. **Recommended format: 16kHz, mono, 16-bit PCM WAV** — matches the ES8311 codec's natural I2S output and is the most reliable format for the backend's transcription pipeline (faster-whisper) without depending on its ffmpeg-based format-sniffing for anything more exotic. |
| `checkin_id` | string | only when replying to a displayed check-in | See §3.3/§4.3 below. |

**This call is fire-and-forget by design, and this is the single most important behavioral
constraint in this whole spec:**
- Response is an immediate `202` with an **empty body**. There is no reply, no
  transcription, no confirmation text — nothing — sent back over HTTP, ever, in production.
- Firmware must upload the file and immediately return to idle/sleep. Do not wait for,
  poll for, or expect any response content. There is currently no mechanism by which the
  device finds out what the assistant said or did in response to a voice command — the
  user finds out via a push notification to their phone (outside this device entirely), or
  by asking again later.
- Do not set the `one_shot` or `sync` form fields some other backend testing tools use —
  those are dashboard-testing-only conveniences and must never be sent by real hardware
  (`sync` in particular would make the connection hang open for as long as the full
  transcribe+agent pipeline takes, defeating the entire point of the fire-and-forget design
  and burning battery for no benefit).

### 3.3 `POST /device/checkin/{checkin_id}/skip` — dismiss a check-in

No request body. Used when the device is displaying a check-in prompt (see §4.3) and the
user dismisses it rather than replying by voice. `{checkin_id}` is the `id` field from the
`checkins[]` entry being displayed.

**Do not confuse this with `POST /checkin/{checkin_id}/skip`** (no `/device` prefix) — that
is a *different* endpoint used by the backend's own magic-link phone/browser flow, which
does not accept the `auth` device token at all. Only the `/device/checkin/{id}/skip` path
(with `/device`) works from firmware.

## 4. Device behavior / state machine

### 4.1 Sync cycle (the main loop)

1. Wake (RTC alarm fired, or BOOT/PWR button pressed — see hardware doc's EXT1 wake pattern).
2. Connect to wifi.
3. `POST /device/sync` with current telemetry (§3.1). Time this call precisely for
   `time_awake_ms` on the *next* sync (see below).
4. On success: apply `timezone.posix` (`setenv`/`tzset`), correct the RTC from `now`,
   render the display from `checkins`/`reminders`/`calendar_events`/`weather`, and schedule
   the next RTC alarm at `now + poll_interval_seconds` (from *this* response — always use
   the freshest value, it can change between syncs).
5. On failure: see §5.
6. Deep sleep.

`time_awake_ms` should be measured as wall-clock milliseconds from the moment the device
wakes (or at minimum, from wifi-connect-start) to the moment the `/device/sync` HTTP
response is received — i.e., it's reporting *last cycle's* awake duration on *this* sync
call (or track it locally and report on the next call, whichever is simpler in your
architecture — precision matters more than which cycle it's attributed to).

### 4.2 Push-to-talk cycle

1. Button press (mapping unspecified — pick a sensible GPIO from the hardware doc's free-pin
   list, or reuse BOOT/PWR; this is a firmware design decision, not dictated by the backend).
2. Record audio via the ES8311 codec.
3. `POST /voice` with the recorded file (§3.2).
4. Return to idle/sleep immediately — no reply wait (§3.2's fire-and-forget warning applies).
5. Resume the normal sync-cycle schedule; this doesn't need to trigger an out-of-band
   `/device/sync` call, though doing so is harmless if convenient.

### 4.3 Check-in display and reply

A `checkins[]` entry with `fired_at` set (non-null) is **currently awaiting a response** —
this is the one thing on the display that's actionable, not just informational. When one is
present:

- Show its `prompt_text` prominently (this is a short reflective mental-health check-in
  prompt, e.g. "How are you feeling right now?" — display it as the primary content, not a
  small notification).
- User can respond two ways:
  - **Reply by voice**: record and `POST /voice` with `checkin_id` set to that entry's `id`
    (§3.2). The reply gets routed to that specific check-in automatically by the backend.
  - **Skip**: `POST /device/checkin/{id}/skip` (§3.3), no recording needed.
- A `checkins[]` entry with `fired_at: null` is scheduled but not yet live — informational
  only ("next check-in around HH:MM"), not something to prompt a reply for yet.

## 5. Error handling / retry policy

- On `/device/sync` failure (network error, timeout, non-2xx status): retry a bounded
  number of times with backoff (e.g. 2-3 attempts) within the same wake cycle, then give up
  for this cycle.
- After giving up: sleep for the **last-known-good** `poll_interval_seconds` (cached from
  the most recent successful sync), or a hardcoded fallback (e.g. 300s) if no successful
  sync has ever happened. Never retry indefinitely or busy-loop — that burns battery for
  exactly the failure mode this project is trying to avoid.
- A malformed or partially-missing JSON response should degrade the affected display
  section only, not abort the whole cycle or crash.
- None of this is data-loss-sensitive: every sync is a stateless full snapshot (§3.1), so a
  missed cycle just means a stale display until the next successful one, not lost
  information.

## 6. Display guidance

- Use partial refresh (`EPD_DisplayPart`) for routine per-sync updates — faster, less
  flicker, better for battery.
- Do a full refresh (`EPD_Display`) periodically (e.g. once per day) to clear e-ink
  ghosting that partial refreshes accumulate over time.
- There is no synthesized voice reply to announce (§3.2) — the eink display and the user's
  phone (via Gotify push, entirely outside this device) are the only two feedback channels
  that exist today.

## 7. Explicit non-goals for v1

- **No OTA updates** — the board's partition table has no OTA slot (see hardware doc);
  firmware updates are via physical reflash only.
- **No multi-device/fleet concept** — the backend assumes exactly one physical device
  (single-row telemetry storage). Don't build in a device-identity/registration concept.
- **No offline queueing or delta sync** — every `/device/sync` call is a fresh, complete
  24-hour snapshot; there's no "catch up on what I missed" concept to implement.
- **No synthesized TTS reply** — `/voice` truly has no response content (§3.2); don't build
  a code path waiting for one.
