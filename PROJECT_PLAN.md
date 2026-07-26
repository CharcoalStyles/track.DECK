# adhi-firmware: Project Plan

This is the working roadmap for building this firmware from an empty repo to a full
feature set. It's meant to be worked through **incrementally, across separate sessions** —
check off items as they're completed, and a fresh session can read this file plus
`CLAUDE.md` to know exactly where things stand without re-deriving context.

## Context

`adhi-firmware` started as just two spec documents (`ESP32_FIRMWARE_SPEC.md`,
`ESP32-S3-ePaper-hardware-reference.md`) and a `CLAUDE.md` — no ESP-IDF project existed yet.
This device is the hardware half of a personal-assistant system whose backend
(`~/Code/adhi-backend`, FastAPI + LangGraph) is already built and deployed. The plan goes
from empty repo to working, tested firmware in three stages: (1) stand up the toolchain and
repo skeleton, (2) a single bring-up flash test that exercises every peripheral once against
the real backend, (3) an ordered feature roadmap that builds the actual device behavior out
from there, noting backend-side changes worth considering along the way.

Confirmed during planning (2026-07-27):
- **No ESP-IDF installed** (no `idf.py`, no `IDF_PATH`). PlatformIO is present but only has
  the Arduino framework for esp32 — not usable here (spec requires ESP-IDF).
- **Waveshare's official example repo is already cloned** at
  `~/Code/ESP32-S3-ePaper-1.54` (git repo, up to date with origin/main) — this is the exact
  source the hardware doc was extracted from, and the copy-source for driver code.
- **Backend is deployed and reachable** — `BACKEND_BASE_URL`/`API_TOKEN` can be obtained for
  real end-to-end testing (not a mock).
- **Backend repo is available** at `~/Code/adhi-backend` — read directly (`main.py`,
  `voice.py`, `jobs/device_sync.py`, `jobs/checkin.py`, `utils/device_state.py`,
  `utils/checkins_store.py`) to ground the feature roadmap's backend-change suggestions in
  real code rather than speculation.
- **Physical board is in hand** and can be plugged in for flash testing.
- **Important nuance on the bring-up base**: `11_FactoryProgram` (the combined example) has
  **no deep-sleep code at all** — grep across it for `deep_sleep`/`EnableDeepLowPowerMode`
  returns nothing. It's an always-on LVGL touch-UI demo (`components/app/user_app.cpp`
  drives an interactive touch loop via `port_ft6336`/`externlib/ui`/`externlib/ui_res` —
  none of which applies to this non-touch, deep-sleep device). The deep-sleep + RTC-alarm
  pattern this device actually needs lives in a **different** example,
  `12_RTC_Sleep_Test`, with a differently-named component set
  (`board_power_bsp::EnableDeepLowPowerMode()` at
  `12_RTC_Sleep_Test/components/board_power_bsp/board_power_bsp.cpp:63-72`, called from
  `components/user_app/user_app.cpp:102`). So "adapt `11_FactoryProgram`" means: take its
  peripheral *drivers* (e-paper, audio codec, button wrapper), but source the
  deep-sleep/wake state machine from `12_RTC_Sleep_Test`, and write our own app layer — the
  touch/LVGL UI code from both is reference-only, not something to carry forward.

---

## Phase 0 — Environment & repo setup

- [x] **Install ESP-IDF v5.5.1 natively** (matches the hardware doc's tested version):
  ```
  git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
  ~/esp/esp-idf/install.sh esp32s3
  . ~/esp/esp-idf/export.sh   # source this in each new shell before using idf.py
  ```
- [x] **`git init`** in `adhi-firmware`, with a `.gitignore` covering `build/`, `sdkconfig`,
      `sdkconfig.old`, `managed_components/`, `dependencies.lock`, and `main/secrets.h`.
      Commit the existing spec docs + `CLAUDE.md` + this plan as the first commit.
- [x] **Project skeleton** at the repo root (not a subdirectory — this repo *is* the
      firmware project): `idf.py create-project adhi-firmware`,
      `idf.py set-target esp32s3`, then merge in `sdkconfig.defaults` from the hardware doc
      (4MB QIO flash, **Octal** PSRAM, single factory partition table with no OTA slot —
      copy the exact CSV from `ESP32-S3-ePaper-hardware-reference.md`).
- [x] **Vendor the driver components**, copied from the local Waveshare clone
      (`~/Code/ESP32-S3-ePaper-1.54/02_Example/ESP-IDF/V2/`) rather than written from scratch:
  - [x] `11_FactoryProgram/components/port_bsp/port_display.{h,cpp}` — e-paper driver
        (SSD1681 opcodes + LUT tables, copy verbatim, don't regenerate).
  - [x] `11_FactoryProgram/components/port_bsp/port_adc.{h,cpp}`, `port_i2c.{h,cpp}` —
        battery ADC and shared I2C bus init (RTC + SHTC3 + codec). Also vendored
        `port_power.{h,cpp}` (EPD/audio/VBAT power gating), `port_sdcard.{h,cpp}`,
        `port_shtc3.{h,cpp}`, `port_codec.{h,cpp}`, and `epaper_config.h` from the same
        directory — all needed for the Phase 1 bring-up test and not called out
        individually above. Dropped the dead `canon.pcm`-embed extern declarations in
        `port_codec.cpp` (unused demo playback data, not part of the real record/playback
        path) rather than vendoring the asset.
  - [x] `11_FactoryProgram/components/externlib/codec_board/` — ES8311 audio abstraction.
  - [x] `11_FactoryProgram/components/externlib/my_button/button.{h,cc}` — debounced
        button wrapper (Espressif `button` component underneath).
  - [x] **Explicitly drop**: `port_ft6336.{h,cpp}` (touch — this board has none),
        `port_lvgl.{h,cpp}`, `externlib/ui/`, `externlib/ui_res/` (Squareline/LVGL touch UI
        + fonts — not applicable to a non-interactive, deep-sleep display).
  - [x] `12_RTC_Sleep_Test/components/board_power_bsp/board_power_bsp.{h,cpp}` — deep
        sleep entry point (`EnableDeepLowPowerMode()`), VBAT rail hold, EXT1 wake mask
        setup. Copied as-is (sleep/wake sequence order preserved — VBAT hold before
        `esp_deep_sleep_start()`); note it duplicates `port_power`'s EPD/Audio/VBAT GPIO
        control on the same three pins — the app-integration step (F3) should pick one
        API and drop the other rather than keeping both.
  - [x] `waveshare/pcf85063a` component (via `idf_component.yml`, registry) for RTC alarm
        set/read — used to schedule the next wake at `now + poll_interval_seconds`.
        Resolves and builds cleanly; no RTC application code written yet (that's F2).
  - [x] `idf_component.yml` dependencies: `espressif/button`, `waveshare/pcf85063a^1.1.1`
        added to `main/idf_component.yml`. `espressif/esp_codec_dev==1.5.4` is already
        pinned transitively via the vendored `codec_board/idf_component.yml`, so not
        duplicated. Deliberately **skipped** `pedrominatel/shtc3` — the vendored
        `port_shtc3.cpp` is a fully self-contained hand-rolled I2C driver that never
        references that package; the original example's manifest listed it but no source
        file in `port_bsp` actually calls into it.
- [ ] **Secrets handling**: `main/secrets.h` (gitignored) defining `BACKEND_BASE_URL` and
      `API_TOKEN` as compile-time constants, matching the spec's "hardcoded, rotated by
      reflashing" model. Commit a `main/secrets.h.example` template with placeholders.
- [ ] **Host-based unit tests** for the pure-logic pieces that don't need hardware — put
      JSON parsing of the `/device/sync` response, POSIX-TZ application logic, and the
      sync retry/backoff state machine in their own component (e.g.
      `components/sync_proto`, using `cJSON`), with a Unity test app under
      `components/sync_proto/test/` built for ESP-IDF's `linux` target
      (`idf.py --preview set-target linux build`, then run the resulting host binary
      directly — confirm exact invocation against the installed 5.5.1 docs during setup,
      since host-target flags have shifted across ESP-IDF versions).

**Phase 0 verification**: `idf.py build` succeeds for a trivial `app_main` that logs
"hello" over serial; the host-based Unity test binary builds and runs (even with a
placeholder passing test).

---

## Phase 1 — Bring-up flash test

One firmware image, flashed once, that exercises every subsystem in a single boot — not the
final app behavior (that's Phase 2), just a smoke test proving the toolchain, drivers, and
backend reachability all work together on real hardware:

- [ ] Wifi STA connect (`06_WIFI_STA` pattern).
- [ ] `POST /device/sync` against the **real** backend with a hardcoded telemetry body,
      over HTTPS with `esp_crt_bundle_attach`, `auth` header set from `secrets.h`. Parse
      the JSON response and log every top-level field.
- [ ] Apply `timezone.posix` (`setenv`/`tzset`), set the PCF85063 RTC from `now`.
- [ ] Read battery ADC (`ADC1_CHANNEL_3`), log `battery_mv`.
- [ ] Read SHTC3 temp/humidity (bonus sensor, not in the spec, but wired and easy to
      smoke-test here since the I2C bus is already up).
- [ ] E-ink smoke test: `EPD_Init()` + `EPD_Clear()` + `EPD_Display()` full refresh with a
      test pattern, then `EPD_Init_Partial()` + `EPD_DisplayPart()` for a partial update.
- [ ] Audio loopback smoke test: record a few seconds via the ES8311, play it back through
      the speaker (PA enable GPIO46) — validates mic and speaker independent of the
      backend `/voice` path.
- [ ] SD card mount (SDMMC 1-bit) + write/read a test file.
- [ ] Deep sleep: hold VBAT (GPIO17), enable EXT1 wake on BOOT/PWR/RTC-INT,
      `esp_deep_sleep_start()`.
- [ ] On wake: log `esp_sleep_get_wakeup_cause()` / `esp_reset_reason()`, confirming the
      reset-reason mapping from the spec (§3.1's field table) resolves correctly across a
      real timer-wake and a real button-wake.

**Phase 1 verification**: `idf.py -p <port> flash monitor`. Confirm over serial that each
step above logs success. Confirm the backend actually received the sync call — check
`GET /debug/device-state` on the backend (`adhi-backend/main.py:593`) or its logs. Visually
confirm the e-ink updated. Confirm audio loopback is audible. Physically confirm both an
RTC-alarm wake and a BOOT-button wake work after a deep sleep cycle.

---

## Phase 2 — Feature roadmap

Building on the bring-up test, in the agreed order (sync/display cycle first, since it's
the device's primary always-on behavior). Each entry lists what "done" looks like, how it's
tested, and — where grounded in the actual backend code — a possible backend-side change to
consider (not applied yet, just noted for later discussion).

### F1. Sync networking core (wifi + auth + retry/backoff)
- [ ] Persist wifi credentials in NVS; send real telemetry (`battery_mv`, `wake_reason`,
      `firmware_version`, `rssi_dbm`, `time_awake_ms`, `reset_reason`) on
      `POST /device/sync`; parse the full response via the `sync_proto` component; bounded
      retry (2-3 attempts) with backoff on failure per spec §5.
- **Test**: trigger a sync via button press, compare parsed fields against
  `GET /debug/device-sync`'s preview payload (`adhi-backend/main.py:583` — returns the
  exact shape `/device/sync` would send, without recording telemetry). Host-based Unity
  tests cover the backoff state machine and JSON parsing directly.
- **Backend idea**: `utils/device_state.py` currently appears to store only the *latest*
  sync's telemetry (`record_sync` overwrites, `GET /debug/device-state` returns one row) —
  consider a small rolling history (last N syncs) so `battery_mv`/`time_awake_ms` trends
  are visible over days. The spec explicitly frames `time_awake_ms` as "the number that
  validates or refutes the battery-life estimate this project is being built around" — a
  single latest value can't show a trend, only a history can.

### F2. RTC time correction + POSIX TZ + wake scheduling
- [ ] Apply `now`/`timezone.posix` every sync; set the next RTC alarm at
      `now + poll_interval_seconds` from the freshest response, never a stale/cached value.
- **Test**: change `device_poll_interval_seconds` via the backend's settings endpoint
  (`main.py`, field defined ~line 388, applied ~line 464) mid-testing and confirm the
  device's next wake timing follows the new value on its very next sync.
- **Backend idea**: none — already fully supported as documented.

### F3. Deep sleep / wake integration
- [ ] Merge `board_power_bsp`'s sleep sequence into the real cycle: wake → connect → sync
      → render → sleep, with `wake_reason`/`reset_reason` correctly attributed each cycle.
- **Test**: run across many hours/cycles unattended; confirm no brownout/watchdog resets
  show up in reported `reset_reason`, and RTC-alarm wakes fire reliably at the expected
  cadence.
- **Backend idea**: pair with F1's history idea — a Gotify alert (reusing the existing
  `notify_error` path other jobs already use) if `reset_reason` reports
  `brownout`/`watchdog`/`panic` repeatedly, since the spec calls this out as "the only
  visibility into a crash/brownout during the beta without a debugger attached."

### F4. E-ink rendering of the sync snapshot
- [ ] Render `checkins`/`reminders`/`calendar_events`/`weather` directly to the 200×200
      framebuffer (`EPD_DrawColorPixel` + a simple bitmap font) — no LVGL/touch
      dependency. Partial refresh per sync, full refresh about once daily to clear
      ghosting.
- **Test**: visually confirm each field renders; force a malformed/partial backend
  response and confirm only the affected section is skipped (spec §5's
  degrade-gracefully requirement) rather than the whole cycle aborting.
- **Backend idea**: none.

### F5. Telemetry completeness
- [ ] Wire the real battery-voltage math (`raw_millivolts * 2`) and precise wall-clock
      `time_awake_ms` (wake-to-response) into the sync body.
- **Test**: cross-check `battery_mv` against a multimeter once; confirm `time_awake_ms`
  stays in the low-hundreds-of-ms range expected for the battery-life goal.
- **Backend idea**: none beyond F1/F3.

### F6. Push-to-talk voice cycle
- [ ] Button press (BOOT reused, unless a dedicated free GPIO is preferred) → record via
      ES8311 (16kHz/mono/16-bit PCM WAV) → `POST /voice` multipart → return to idle
      immediately, no reply wait, ever.
- **Test**: record a short phrase, confirm an immediate `202` with empty body, then
  confirm the backend actually processed it via Gotify notification or backend logs
  (`voice.py`'s background pipeline). Explicitly verify `one_shot`/`sync` form fields are
  never sent.
- **Backend idea**: none — `voice.py`'s contract already matches the spec precisely.

### F7. Check-in display + reply/skip flow
- [ ] Detect a `checkins[]` entry with `fired_at` set, display `prompt_text` prominently,
      support voice-reply (`POST /voice` with `checkin_id`) and skip
      (`POST /device/checkin/{id}/skip`).
- **Test**: trigger a test check-in via the backend's `POST /debug/checkin`
  (`main.py:535-543`), confirm it appears on the next sync, and confirm both the
  reply-by-voice and skip paths resolve it correctly.
- **Backend idea**: none — built for exactly this.

### F8. Robustness / power-safety hardening pass
- [ ] Verify bounded retry+backoff under real failure (backend down, wifi unreachable,
      malformed JSON); confirm peripherals are power-gated when unused (GPIO42 audio
      circuit, GPIO6 EPD power, both active-low) rather than left powered through sleep.
- **Test**: stop the backend mid-cycle, confirm the device falls back to the
  last-known-good `poll_interval_seconds` and sleeps rather than retrying indefinitely;
  confirm current draw drops when peripherals are gated.
- **Backend idea**: a second, distinct alert alongside F3's — "no successful sync in over
  `poll_interval_seconds * 3`" — since a silently-dead/offline device and a noisy
  crash-loop are different failure signatures worth telling apart.

**Explicit non-goals throughout** (already agreed in `CLAUDE.md`): no OTA, no
multi-device/fleet concept, no offline queueing/delta sync, no synthesized TTS reply.

---

## Critical files / reference paths

- `~/Code/ESP32-S3-ePaper-1.54/02_Example/ESP-IDF/V2/11_FactoryProgram/` — driver
  copy-source (display, audio, button, ADC, I2C), touch/LVGL-UI parts excluded.
- `~/Code/ESP32-S3-ePaper-1.54/02_Example/ESP-IDF/V2/12_RTC_Sleep_Test/components/board_power_bsp/` —
  deep-sleep copy-source.
- `~/Code/adhi-backend/main.py` — all device-facing routes; `jobs/device_sync.py` — sync
  payload assembly; `voice.py` — voice upload contract; `jobs/checkin.py` /
  `utils/checkins_store.py` — check-in state machine; `utils/device_state.py` — telemetry
  storage (the thing F1's backend idea would extend).
- `ESP32_FIRMWARE_SPEC.md` / `ESP32-S3-ePaper-hardware-reference.md` (this repo) — the
  authoritative contract/pinout docs everything above is built against.
