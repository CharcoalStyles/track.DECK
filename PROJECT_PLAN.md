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
- [x] **Secrets handling**: `main/secrets.h` (gitignored) defining `BACKEND_BASE_URL` and
      `API_TOKEN` as compile-time constants, matching the spec's "hardcoded, rotated by
      reflashing" model. Committed `main/secrets.h.example` template with placeholders;
      `secrets.h` itself currently holds the same placeholders (not real credentials yet) —
      fill in real values before Phase 1's actual backend sync call.
- [x] **Host-based unit tests** for the pure-logic pieces that don't need hardware — JSON
      parsing of the `/device/sync` response (`sync_snapshot.{h,c}`, using `cJSON`,
      degrading each section independently per spec §5), POSIX-TZ application logic
      (`tz_apply.{h,c}`), and the sync retry/backoff state machine (`sync_backoff.{h,c}`)
      live in `components/sync_proto`. Test app is under
      `components/sync_proto/test_apps/host/`, targeting ESP-IDF's `linux` target.
      **Correction from the original plan**: ESP-IDF 5.5.1's actual convention for
      `linux`-target host tests is **Catch2**, not Unity — confirmed by checking the
      installed IDF's own bundled examples (e.g. `components/log/host_test/log_test`),
      which all use `espressif/catch2` + a FreeRTOS mock (`tools/mocks/freertos/`) +
      `set(COMPONENTS main)` to keep the build minimal, with Catch2 providing `main()`
      instead of the normal ESP-IDF entrypoint. Exact invocation:
      `idf.py --preview -C components/sync_proto/test_apps/host set-target linux`, then
      `build`, then run `build/sync_proto_test.elf` directly (not `idf.py monitor`, which
      doesn't apply to a host binary). Requires two system packages not preinstalled on
      this machine: `ruby` (FreeRTOS mock codegen) and `libbsd-dev` (linux-target
      `sys/cdefs.h`) — both installed manually via `sudo apt-get install`. 20 test cases /
      78 assertions passing.

**Phase 0 verification**: ✅ `idf.py build` succeeds (`main/adhi-firmware.c` currently holds
an e-ink + audio smoke test, not just a placeholder — see below). ✅ The host-based Catch2
test binary (`components/sync_proto/test_apps/host`) builds and runs: 20 test cases / 78
assertions passing.

**Ahead of Phase 1**: rather than wait for the full bring-up test, did a minimal real-hardware
smoke test early to validate the build→flash→run loop end to end: `main/adhi-firmware.c`
powers on the EPD + audio rails, draws a checkerboard via `EPD_Display()` full refresh, and
plays a generated 440Hz tone through the ES8311 codec. Flashed to the real board (ESP32-S3
PICO-1, 8MB flash/8MB PSRAM — note our `sdkconfig.defaults` targets the documented 4MB flash
size, which is fine/conservative but leaves 4MB unused) via `idf.py -p /dev/ttyACM0 flash`.
Confirmed over serial log: e-ink pattern displayed, codec initialized, tone played, no
errors. One flashing gotcha: this board's native USB-Serial/JTAG interface doesn't reliably
auto-reset out of ROM download mode via software (RTS/DTR toggling) — needed a physical
USB unplug/replug to boot the flashed app. `idf.py monitor` also doesn't work in this
non-interactive environment (needs a real TTY); reading `/dev/ttyACM0` directly via a
pyserial script (without touching DTR/RTS) works as a substitute.

---

## Phase 1 — Bring-up flash test

One firmware image, flashed once, that exercises every subsystem in a single boot — not the
final app behavior (that's Phase 2), just a smoke test proving the toolchain, drivers, and
backend reachability all work together on real hardware. All implemented in
`main/adhi-firmware.cpp`.

- [x] Wifi STA connect (`06_WIFI_STA` pattern). **Deviation from spec**: the home AP runs
      WPA2/WPA3 mixed ("transition") mode, and the station always prefers WPA3-SAE when an
      AP advertises both — `wifi_config.sta.threshold.authmode` does **not** override this
      (it's a minimum floor, not a cap; confirmed against Espressif's own docs). The SAE
      handshake against this specific AP failed all retries within a 30s connect window.
      Fixed by disabling SAE support entirely at compile time
      (`CONFIG_ESP_WIFI_ENABLE_WPA3_SAE=n` in `sdkconfig.defaults`), forcing WPA2-PSK. Now
      connects in ~1-4s reliably.
- [x] `POST /device/sync` against the **real** backend with a hardcoded/boot-state-derived
      telemetry body (`wake_reason`/`reset_reason` real, computed at boot;
      `rssi_dbm`/`time_awake_ms` real, cheap to compute; `battery_mv` omitted — reading the
      real ADC is its own later bullet per the checklist's own ordering). **Deviation from
      spec**: plain HTTP, not HTTPS — `BACKEND_BASE_URL` is presently a LAN IP
      (`main/secrets.h`, gitignored), not the HTTPS domain the spec eventually assumes, so
      no `esp_crt_bundle_attach` needed yet. `auth` header set from `secrets.h`. Bounded
      retry/backoff via `components/sync_proto`'s `sync_backoff` on failure. Full response
      parsed via `sync_snapshot_parse`, every top-level field logged. Confirmed against the
      real backend — full snapshot including a real live check-in.
- [x] Apply `timezone.posix` (`setenv`/`tzset` via `sync_tz_apply`), set the PCF85063 RTC
      from `now` (stored as UTC on the chip; POSIX TZ applied separately, never baked into
      what's stored — see `rtc_set_time_utc`). Confirmed via log:
      `RTC set from server \`now\` (UTC): 2026-07-27 00:52:30`.
- [x] Read battery ADC (`ADC1_CHANNEL_3`), log `battery_mv`. Confirmed: `4.114V (99%)`.
- [x] Read SHTC3 temp/humidity. Confirmed: real temp/humidity logged every cycle.
- [x] E-ink smoke test: full refresh (checkerboard) then partial refresh (marker box).
      Confirmed both visually and via log.
- [x] Audio loopback smoke test: record 3s via the ES8311, play it back through the
      speaker. Confirmed audible, full clip plays through.
- [x] SD card mount (SDMMC 1-bit) + write/read a test file. Confirmed by pulling the card
      and reading `bringup_test.txt` directly — exact expected content, exact expected
      size.
- [x] Deep sleep: hold VBAT (GPIO17), enable EXT1 wake on BOOT/PWR/RTC-INT,
      `esp_deep_sleep_start()`. Confirmed — USB drops (native USB-JTAG loses power in deep
      sleep) and the backend's next-recorded `reset_reason` reads `deep_sleep_wake`.
- [x] On wake: log `esp_sleep_get_wakeup_cause()` / `esp_reset_reason()`. Reset-reason
      mapping and **button wake** confirmed repeatedly (backend recorded
      `wake_reason: "button"`, `reset_reason: "deep_sleep_wake"`). **RTC-alarm (timer) wake
      not yet independently confirmed** — every wake exercised so far has been a manual
      BOOT/PWR press; the alarm is scheduled correctly (`rtc_schedule_alarm`, confirmed via
      log) but a genuine unprompted timer wake still needs to be observed (leave the device
      alone for a full `poll_interval_seconds` cycle and check the next `wake_reason`).

**Four real, unrelated bugs found and fixed during bring-up** (all in `sdkconfig.defaults`
unless noted):
1. **Stack overflow** (the main crash chased at length): `sync_snapshot_t` (~8.4KB) was
   declared as a stack local in `app_main`, whose task stack is only 3.5KB
   (`CONFIG_ESP_MAIN_TASK_STACK_SIZE`, default 3584). Silently overflowed and corrupted
   nearby memory, manifesting as a completely unrelated-looking crash deep inside
   `nvs_flash_init`'s cross-core IPC path — exact symptom (`StoreProhibited` / `Cache
   error` / `LoadProhibited` / `abort`) varied by build depending on what happened to sit
   next to the overflow. Fixed at the call site in `adhi-firmware.cpp` by
   heap/PSRAM-allocating the struct instead; stack bumped to 8192 as defense in depth.
2. **WPA3-SAE vs. this AP's transition mode** — see the wifi bullet above.
3. **Interrupt watchdog too aggressive for the audio codec's playback-cleanup path** —
   default 300ms `CONFIG_ESP_INT_WDT_TIMEOUT_MS` was silently resetting the device (no
   panic/backtrace printed at all) right after `Codec_PlaybackData()`, even though the
   audio audibly played in full. Loosened to 2000ms (and task WDT to 15s) — confirmed fix
   by finally seeing `"audio loopback done"` print reliably.
4. **FATFS 8.3-filename-only** (`CONFIG_FATFS_LFN_NONE` is the ESP-IDF default) rejected
   `bringup_test.txt` (12-char base name, over the 8-char 8.3 limit) with
   `ESP_ERR_NOT_FOUND` from `fopen()`, even though the card mounted fine. Fixed with
   `CONFIG_FATFS_LFN_HEAP=y`, matching what the original Waveshare reference examples
   already enable for their own SD card usage.

Also incidentally corrected (found while investigating the above, not fixes for any of
them, but genuine doc/hardware mismatches worth keeping): flash size is actually 8MB on
this physical unit (ESP32-S3-PICO-1 SiP), not the documented 4MB; and this SiP + Octal
PSRAM needs a 64-byte data cache line
(`CONFIG_ESP32S3_DATA_CACHE_LINE_64B`) per a documented TRM mismatch (Octal PSRAM DDR mode
uses 64-byte wrap bursts) — see `github.com/espressif/arduino-esp32` issue #12480.

**Phase 1 verification**: ✅ Confirmed over serial + the backend's own recorded telemetry
that every step above logs/behaves correctly. ✅ Backend actually received real sync calls
(`GET /debug/device-state`, `adhi-backend/main.py:593`). ✅ E-ink updates confirmed
visually. ✅ Audio loopback confirmed audible. ✅ SD card write/read confirmed by direct
inspection. ✅ BOOT-button wake confirmed repeatedly. ⬜ RTC-alarm wake — not yet observed
un-prompted; still needs a real hands-off cycle.

**Flashing note for this board**: its native USB-Serial/JTAG interface doesn't reliably
auto-exit ROM download mode via software (RTS/DTR toggling) after `idf.py flash` — it
often needs a physical USB unplug/replug to actually boot the app. The reverse is also
useful: to force entry into (and stay in) download mode reliably for flashing — instead of
racing the brief post-wake awake-window before the device returns to deep sleep — hold the
BOOT button, unplug/replug USB while still holding it, then release BOOT after replugging.
`idf.py monitor` doesn't work in a non-interactive shell (needs a real TTY); reading
`/dev/ttyACM0` directly via a small pyserial script (without touching DTR/RTS, with
auto-reconnect on transient read errors) works as a substitute.

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
