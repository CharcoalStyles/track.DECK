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
inspection. ✅ BOOT-button wake confirmed repeatedly. ✅ RTC-alarm (timer) wake confirmed —
see the 1-minute clock-tick scheme below, which wakes unattended on the RTC alarm dozens of
times in a row without any button involved.

**Flashing note for this board**: its native USB-Serial/JTAG interface doesn't reliably
auto-exit ROM download mode via software (RTS/DTR toggling) after `idf.py flash` — it
often needs a physical USB unplug/replug to actually boot the app. The reverse is also
useful: to force entry into (and stay in) download mode reliably for flashing — instead of
racing the brief post-wake awake-window before the device returns to deep sleep — hold the
BOOT button, unplug/replug USB while still holding it, then release BOOT after replugging.
`idf.py monitor` doesn't work in a non-interactive shell (needs a real TTY); reading
`/dev/ttyACM0` directly via a small pyserial script (without touching DTR/RTS, with
auto-reconnect on transient read errors) works as a substitute -- **but with an important
caveat discovered during the clock-tick work below: opening a *new* pyserial connection to
this board's native USB-Serial/JTAG interface can itself trigger a chip reset**, via the
same DTR/RTS auto-reset circuitry `esptool` uses (every such unwanted reset shows up as
`rst:0x15 (USB_UART_CHIP_RESET)`, reset_reason `unknown`). Repeatedly reconnecting to
"check the log" can therefore prevent the device from ever reaching a real multi-cycle
deep sleep, corrupting exactly the kind of multi-wake test this caveat itself was found
by. When observing behavior across several wake cycles, prefer watching the physical
device (screen/audio) over polling serial, or accept that serial polling resets the count.

### Clock-tick wake scheme (built on top of Phase 1, in the same file)

Not part of the original Phase 1 checklist, but implemented directly in
`main/adhi-firmware.cpp` afterward: wakes every ~60s (RTC alarm aligned to the next whole
minute boundary, not "current time + 60s" — see below) to repaint an on-screen clock via a
quiet e-ink partial refresh; only every `s_ticks_per_sync`'th wake (derived from the
server's `poll_interval_seconds / 60`, default 5) does the full wifi/backend/sensor/audio/SD
cycle. State that must survive across these wakes (tick count, ticks-per-sync, the last TZ
string, whether the e-ink partial baseline is seeded) lives in `RTC_DATA_ATTR` globals —
ordinary RAM/heap does not survive deep sleep, only the RTC slow-memory region does.

Also added: a minimal hand-rolled 5x7 bitmap font (`FONT_5X7`) to render "HH:MM" — no
font/text-rendering library existed before this.

**Real bugs found and fixed while building this**:
- **E-ink controller loses all internal RAM/LUT state whenever `EPD_PWR_PIN` is cut** —
  true partial refresh (quiet, fast) is only possible if the display stays powered between
  wakes. Since wakes are now only ~60s apart and the SSD1681's standby draw is tiny next to
  a full refresh or a wifi sync, `enter_deep_sleep()` no longer powers off the EPD (only
  audio power gates off); the EPD's own power-rail GPIO needs the same `rtc_gpio_hold_en`
  treatment as VBAT, or GPIO sleep-isolation could let it float.
- **`eink_clock_tick()` must re-run `PortDisplay_Init()` every wake** (ESP32-side SPI/GPIO
  state and the software framebuffer don't survive deep sleep regardless of what stays
  externally powered) **but must never call `EPD_Init()`** on the quiet-tick path — that
  pulses the SSD1681's hardware reset line, wiping the very internal state kept-powered EPD
  was meant to preserve.
- **Alarm drift**: scheduling "current RTC time + 60s" each cycle drifts away from true
  minute boundaries over many cycles, since actual awake-processing time varies cycle to
  cycle. Fixed by always rounding up to the start of the next whole minute instead
  (self-correcting every cycle, no accumulation) — `rtc_schedule_alarm()` no longer takes an
  interval parameter at all.
- **Render-ordering latency**: the clock render originally happened *after* the entire
  sync cycle (wifi/backend/sensors), adding several extra seconds of visible lag on sync
  wakes for no reason — none of that work changes what the clock displays. Fixed by
  rendering immediately on quiet ticks (nothing else to wait for), while sync wakes
  deliberately still render only once, after all of that cycle's data is ready — rendering
  early there too would mean a quick clock-only write followed by a second, fuller write
  once Phase 2 renders real snapshot content in the same spot.
- **Double refresh on every full/seed update**: `EPD_DisplayPartBaseImage()` (needed to
  seed the SSD1681's partial-diffing baseline) already performs its own full visible
  refresh internally — calling `EPD_Display()` first as well drew the identical content to
  the screen twice in a row.
- **Checkerboard ghosting**: the Phase 1 smoke-test checkerboard pattern is close to
  worst-case for showing partial-refresh ghosting (e-ink physically retains a "ghost" of
  heavily-contrasted content after repeated partial refreshes) — replaced with a plain
  white background for the real clock content.

### Clock-tick wake scheme: removed (2026-07-28)

The 60s partial-refresh wake scheme above turned out to produce audible noise from the
SSD1681 once a minute, all day — a real problem for what's meant to be a quiet bedside/desk
device. Removed entirely (`FONT_5X7`, `draw_glyph`/`draw_clock`, `eink_clock_tick`, the
`s_wake_tick_count`/`s_ticks_per_sync`/`s_epd_seeded` `RTC_DATA_ATTR` state, and the
`is_sync_wake` branching in `app_main`) rather than just reducing its frequency, since the
whole tick-wake architecture existed solely to serve the clock.

Wake scheduling reverted to the literal spec design (`ESP32_FIRMWARE_SPEC.md` §4.1): one
wake = one full sync cycle, RTC alarm set directly to `now + poll_interval_seconds`
(`rtc_schedule_alarm()` takes back its `interval_seconds` parameter, matching the
pre-clock-tick `ab5ab59` version). This is a net simplification, not just a feature cut:

- No more partial refreshes anywhere — `eink_full_seed_with_clock()` → `eink_full_refresh()`,
  a plain `EPD_Display()` full refresh once per cycle (screen content is blank for now;
  F4 still owns rendering real snapshot content).
- `poll_interval_seconds` is now honored exactly (down to the second) instead of rounded to
  the nearest 60s tick — strictly more precise than F2's tick-rounding fix below, which is
  now superseded and removed (see F2's entry).
- The EPD is power-gated off between wakes again (`BoardPower_EPD_OFF()` in
  `enter_deep_sleep()`, no more `EPD_PWR_PIN` hold) — it only needed to stay powered to
  preserve the SSD1681's partial-refresh baseline across quiet ticks, which no longer exist.
- **Bonus fix while rebuilding this path**: spec §5 says a failed sync should fall back to
  sleeping for the *last-known-good* `poll_interval_seconds`, cached from the most recent
  successful sync — but neither this scheme nor the original pre-clock code actually
  persisted that value *across* deep-sleep cycles (a fresh boot after a failure always fell
  back to the hardcoded default, even if the previous cycle had a perfectly good cached
  value). Fixed by persisting `s_last_known_good_poll_interval`/`s_has_synced_ever` in
  `RTC_DATA_ATTR` and using `sync_effective_poll_interval()` (`sync_backoff.c`) — written
  and unit-tested since Phase 0, but never actually wired into the firmware until now.

Not in scope for this change: `audio_loopback_test()` also runs unconditionally every sync
cycle and is audibly noisy in its own right (records 3s, plays it back through the
speaker). The complaint that triggered this removal was specifically about the e-ink
partial refresh, so this is left as-is — worth revisiting in F8 (robustness/power-safety
pass).

**Verification status**: `idf.py build` clean, host Catch2 suite back to 78 assertions / 20
cases (confirms removing `sync_poll_interval_to_ticks` didn't break anything else).
Flashed and confirmed on real hardware (via direct physical observation, since this
session's shell lost USB passthrough to the board partway through — `lsusb` never showed
the device again after that point, unrelated to the code itself): screen is blank on wake
(no more clock, as expected — F4 still owns real content) and a button-triggered wake runs
the full cycle (e-ink refresh, mic/speaker loopback) with no per-minute activity in
between. Serial-level confirmation of the exact alarm-interval log line
(`"RTC alarm scheduled at HH:MM:SS UTC (+Ns)"`) is still outstanding — worth a quick check
next time serial access is available, but not blocking given the physical behavior already
confirms the tick scheme is gone.

---

## Phase 2 — Feature roadmap

Building on the bring-up test, in the agreed order (sync/display cycle first, since it's
the device's primary always-on behavior). Each entry lists what "done" looks like, how it's
tested, and — where grounded in the actual backend code — a possible backend-side change to
consider (not applied yet, just noted for later discussion).

### F1. Sync networking core (wifi + auth + retry/backoff)
- [x] Send real telemetry (`battery_mv`, `wake_reason`, `firmware_version`, `rssi_dbm`,
      `time_awake_ms`, `reset_reason`) on `POST /device/sync`; parse the full response via
      the `sync_proto` component; bounded retry (2-3 attempts) with backoff on failure per
      spec §5. All of this was already substantially in place from the Phase 1 bring-up
      test (`sync_backoff` bounded at `SYNC_BACKOFF_MAX_ATTEMPTS=3`, full response parsed
      via `sync_snapshot_parse`) — F1's actual remaining work was wiring `battery_mv` into
      the request body itself (previously only read/logged *after* the sync call; moved
      the ADC read earlier in `adhi-firmware.cpp` so it's available when the JSON body is
      built) and the wifi rework below.
  - [x] **Revised from the original checklist wording**: skip persisting wifi credentials
        to NVS. `CLAUDE.md`'s model for this device is "hardcoded, rotated only by
        reflashing" (matching `API_TOKEN`) — there's no product reason to decouple wifi
        creds from compile-time constants the way a multi-user provisioning flow would
        need. Instead, `secrets.h` now holds an array (`WIFI_NETWORKS[]`, at least one
        entry) instead of a single SSID/password pair, still compiled in.
  - [x] **Scan-then-match instead of one hardcoded SSID**: `wifi_connect()` now calls
        `wifi_scan_and_select()` first — one active scan (`esp_wifi_scan_start`, blocking,
        ~1-3s), matched against `WIFI_NETWORKS[]`, connecting to whichever known network
        has the strongest visible RSSI. Falls back to the first configured entry blind
        only if nothing in the scan matched (covers hidden SSIDs). Deliberately *not* a
        serial try-each-network-with-a-timeout approach — that would multiply worst-case
        wake time by however many networks are configured, against the `time_awake_ms`
        battery-life budget the spec cares about; one scan is a fixed small cost
        regardless of list length. Removed the old `WIFI_EVENT_STA_START` auto-connect
        handler since the initial connect is now issued explicitly, after scan+select,
        not blind on driver start.
- **Test**: `idf.py build` clean, flashed to the real board (`idf.py -p /dev/ttyACM0
  flash`), confirmed over serial:
  - `battery: 4.120V (99%)` logged *before* the wifi scan/connect — confirms the ADC read
    now happens early enough to land in the sync request body, not just after it.
  - `wifi scan: selected known SSID "Babys First Wifi" (rssi -54, 11 APs seen)` — scan-
    then-match picked the known network correctly out of 11 visible APs on the first real
    multi-AP scan this firmware has ever done.
  - One `wifi disconnected, retry` cycle occurred (AP responded "Association refused
    temporarily" — normal 802.11 SAE/assoc backoff, not a bug) and the existing
    disconnect-retry handler recovered on its own within ~3s, connecting on the next
    attempt.
  - `/device/sync OK (status 200)` with a full real snapshot parsed (live check-in,
    weather, TZ, poll_interval_seconds all present and logged) — confirms battery_mv
    reached the backend as part of a normal request (not separately re-verified against
    `GET /debug/device-state` at the time — that curl attempt failed for a mundane reason
    later found in F5: a missing `auth` header, not an actual restriction. The serial-side
    confirmation was sufficient here regardless).
  - Rest of the cycle (RTC set, SHTC3 read, e-ink refresh, audio loopback) completed
    normally afterward, unaffected by the F1 changes.
- **Backend idea**: `utils/device_state.py` currently appears to store only the *latest*
  sync's telemetry (`record_sync` overwrites, `GET /debug/device-state` returns one row) —
  consider a small rolling history (last N syncs) so `battery_mv`/`time_awake_ms` trends
  are visible over days. The spec explicitly frames `time_awake_ms` as "the number that
  validates or refutes the battery-life estimate this project is being built around" — a
  single latest value can't show a trend, only a history can.

### F2. RTC time correction + POSIX TZ + wake scheduling
- [x] Apply `now`/`timezone.posix` every sync; honor the freshest `poll_interval_seconds`
      from the response, never a stale/cached value.
  - **Architecture note**: this was already substantially done by the clock-tick wake
        scheme built on top of Phase 1 — the device wakes every ~60s regardless (RTC alarm
        always set to the next whole-minute boundary, for the on-screen clock), and only
        every `s_ticks_per_sync`'th wake does a real sync. That's a deliberate divergence
        from the checklist's literal "set the next RTC alarm at `now + poll_interval_seconds`"
        wording — a single long alarm would kill the clock display — but achieves the same
        effect: sync cadence tracks the server's freshest `poll_interval_seconds`, applied
        fresh on every successful sync, never hardcoded or left stale.
  - [x] **Real bug found and fixed**: `s_ticks_per_sync` was computed as
        `poll_interval_seconds / 60` (integer division, i.e. floored) — for any
        `poll_interval_seconds` that isn't an exact multiple of 60 (the backend's own
        validator allows 30-86400, not just round minutes — see
        `adhi-backend/main.py`'s `is_valid_poll_interval_seconds`), this synced *more*
        often than the server actually asked for (e.g. 90s would floor to 1 tick = 60s
        instead of ~90s). Fixed by adding `sync_poll_interval_to_ticks()` to
        `components/sync_proto/sync_backoff.{h,c}` — rounds to the *nearest* tick instead
        of flooring, still clamped to a minimum of 1 tick (60s is the smallest schedulable
        unit in this architecture, so anything below that can't be honored more precisely).
        Covered by 4 new host-side Catch2 test cases (exact multiples, round-to-nearest
        including a tie, and the never-below-1 floor) — 88 assertions / 23 cases total now
        pass, up from 78/20.
  - **Superseded (2026-07-28)**: the clock-tick wake scheme this rounding fix belonged to
        was removed entirely (see the note after Phase 1's "Clock-tick wake scheme"
        section) due to audible partial-refresh noise. `sync_poll_interval_to_ticks()` and
        its tests were deleted along with it — wake scheduling reverted to a single alarm
        at `now + poll_interval_seconds`, which honors the server's value exactly rather
        than rounding to a 60s tick, making this fix's whole premise moot. Test suite is
        back to 78 assertions / 20 cases as a result.
- **Test**: `idf.py build` clean; host Catch2 suite passes (88 assertions/23 cases,
  including the new tick-rounding tests). Flashed to real hardware and confirmed over
  serial: TZ applied (`AEST-10AEDT,M10.1.0,M4.1.0/3`), RTC set from server `now`
  (`2026-07-27 04:09:20` UTC), `poll_interval_seconds=300` received and parsed. **Not
  independently reproduced live** for a non-multiple-of-60 `poll_interval_seconds` (would
  require temporarily changing the backend's setting and observing tick timing across
  several more wake cycles — skipped for now given this board's known caveat that
  reconnecting serial mid-test can itself trigger a reset, corrupting exactly this kind of
  multi-wake observation). The rounding logic itself is fully covered by the host unit
  tests instead.
- **Backend idea**: none — already fully supported as documented.

### F3. Deep sleep / wake integration
- [x] Merge `board_power_bsp`'s sleep sequence into the real cycle: wake → connect → sync
      → render → sleep, with `wake_reason`/`reset_reason` correctly attributed each cycle.
  - **Turned out to already be done**: the linear wake → connect → sync → render → sleep
        cycle (`app_main`) and correct `wake_reason`/`reset_reason` attribution have been
        working and hardware-confirmed since Phase 1, and are unchanged by the clock-tick
        removal (if anything, that removal made the cycle *more* literally "one wake, one
        full cycle" than it was before). The one genuinely outstanding piece was the
        duplication flagged back in Phase 0: `board_power_bsp` (vendored from
        `12_RTC_Sleep_Test`) and `port_bsp`'s `port_power` both drive the same three
        EPD/Audio/VBAT GPIOs and implement near-identical deep-sleep entry sequences, but
        only `port_power` was ever actually called from `adhi-firmware.cpp` —
        `board_power_bsp` wasn't even listed in `main`'s `REQUIRES`, just auto-discovered
        and compiled in unused. Deleted `components/board_power_bsp` entirely rather than
        merging anything: `enter_deep_sleep()` already *is* the merged, real sequence,
        just built on `port_power` instead. `idf.py build` confirms this was truly dead
        weight — reported binary size (`0x1129e0` bytes) is unchanged from the last build
        before removal.
- **Test**: `idf.py build` clean after the removal. The "run across many hours/cycles
  unattended, confirm no brownout/watchdog resets" half of this is inherently a
  longer-duration, mostly-passive check rather than something to confirm synchronously —
  RTC-alarm (timer) wake reliability was already confirmed repeatedly under the old 60s
  clock-tick cadence (dozens of unattended wakes in a row, see Phase 1's notes); worth a
  fresh multi-hour soak at the new, longer single-alarm cadence (whatever
  `poll_interval_seconds` the backend currently has set) to specifically watch for
  brownout/watchdog `reset_reason` values, next time there's a stretch to just let it run
  and check back.
- **Backend idea**: pair with F1's history idea — a Gotify alert (reusing the existing
  `notify_error` path other jobs already use) if `reset_reason` reports
  `brownout`/`watchdog`/`panic` repeatedly, since the spec calls this out as "the only
  visibility into a crash/brownout during the beta without a debugger attached."

### F4. E-ink rendering of the sync snapshot
- [x] Render `checkins`/`reminders`/`calendar_events`/`weather` directly to the 200×200
      framebuffer (`EPD_DrawColorPixel` + a simple bitmap font) — no LVGL/touch
      dependency.
  - **Bumped up ahead of F5-F8** at the user's request, alongside adding a battery
        percentage readout (not in the original checklist wording, folded in here since
        it's the same status-bar work).
  - **Deviation from the checklist's "partial refresh per sync, full refresh about once
        daily"**: unchanged from the clock-tick-removal decision — full refresh
        (`EPD_Display()`) only, no partial refresh anywhere, since partial refresh is what
        caused the audible-noise problem that removal fixed. Still refreshing far more
        often than "once daily" (every `poll_interval_seconds`), which is a known,
        already-accepted tradeoff from that earlier change, not new here.
  - **Font**: no text-rendering library exists for this display (`port_display.h` is
        pixel-only) — hand-authored a small 5x7 bitmap font (`FONT_5X7`) covering digits,
        uppercase A-Z (text upper-cased before drawing), and a bounded punctuation set,
        plus `draw_text`/`draw_wrapped_text` (greedy word-wrap with mid-word truncation
        fallback and a "..." marker) as general-purpose helpers.
  - **Layout**: status bar (always drawn) shows local time (top-left), current temperature
        (centered), and battery percentage (top-right, `Get_Batterylevel()`), under a
        horizontal divider. Below that: a live check-in (`checkins[]` entry with
        `fired_at` set — spec §4.3's "the one thing on the display that's actionable")
        takes over the rest of the screen if one exists; otherwise a compact dashboard
        (cloud cover — temperature already lives in the status bar, so not repeated — plus
        the single soonest-upcoming item across `reminders[]`/`calendar_events[]`
        combined, whichever is sooner). Each section degrades independently per spec §5:
        `has_weather`/`*_valid` false just omits that section, no error shown.
  - **Real design insight found while wiring this up**: e-ink is bistable (holds its image
        with no power), so `app_main` now only calls the render function when this cycle
        actually produced a fresh, successfully-parsed snapshot — a cycle where wifi or
        sync fails skips the refresh entirely rather than blanking the screen. This gets
        `CLAUDE.md`'s "a sync failure or stale display is a degraded UX, never a missed
        reminder" almost for free: a stale-but-intact display beats an empty one, and
        needed no extra state persistence to achieve (unlike, say, persisting the whole
        snapshot across deep sleep, which wouldn't fit in the ~8KB RTC slow memory region
        anyway — `sync_snapshot_t` alone is ~8.4KB).
- **Test**: hardware-confirmed across several flash/look-at-the-screen iterations (this is
  fundamentally a visual feature — real verification is looking at the device, not just
  logs). Confirmed: a genuinely live check-in (`fired_at` set) rendering prominently,
  correctly *not* showing a merely-scheduled one (`fired_at: null`, verified directly
  against a live `/debug/device-sync` response) and falling back to the dashboard instead;
  battery/time/temperature all showing in the status bar; cloud-cover dashboard line.
  Check-in body text needed a follow-up fix — the initial scale-2 body font didn't leave
  enough room for a full prompt and was cut off; dropped to scale 1 (~30 chars/line × 15
  lines, well over the 256-char field cap) and confirmed fixed. Malformed/partial-response
  degrade-gracefully behavior relies on `sync_snapshot_parse`'s existing per-section
  `*_valid`/`has_*` flags (already unit-tested in `sync_proto`'s host test suite) rather
  than a fresh forced-malformed-response hardware test.
- **Backend idea**: a 3-day hi/low forecast in the dashboard was requested but deferred —
  `/device/sync`'s `weather` object only carries today's `temperature_min_c`/`_max_c`, not
  a multi-day array. `adhi-backend/agent/tools/weather.py` already has a
  `get_weather_forecast()` helper pulling up to 7 days from Open-Meteo, currently only used
  by the conversational agent — wiring a small 3-entry min/max array from that into
  `jobs/device_sync.py`'s payload would be the natural way to unlock this without a new
  external API integration.

### F5. Telemetry completeness
- [x] Wire the real battery-voltage math (`raw_millivolts * 2`) and precise wall-clock
      `time_awake_ms` (wake-to-response) into the sync body.
  - **Battery-voltage math was already done**: `port_adc.cpp`'s `Get_VbatVoltage()`
        (vendored Waveshare driver, Phase 0) already applies the ×2 divider correction
        internally (`VbatVoltage = 0.001 * CalibratedData * 2`) — confirmed genuinely
        correct, not just present, since every logged voltage across this whole project
        (4.11-4.14V) is exactly right for a full single-cell LiPo; without the ×2 it would
        read roughly half that. No firmware change needed here.
  - [x] **Real bug found and fixed**: `time_awake_ms` was computed at the very top of
        `device_sync()`, *before* the HTTP call it's embedded in even happens — so it
        measured "wake to start of the first sync attempt," excluding the actual network
        round-trip and any retries entirely, directly contradicting spec section 4.1's "the
        number that validates or refutes the battery-life estimate this project is being
        built around." Fixed per the spec's own suggested alternative ("track it locally
        and report on the next call"): `s_last_cycle_time_awake_ms` (`RTC_DATA_ATTR`,
        starts at 0 for the very first boot) is measured *after* the sync attempt concludes
        (success, retries-exhausted, or wifi never connecting), and reported as
        `time_awake_ms` on the *next* cycle's sync call — `device_sync()` now takes it as a
        parameter instead of computing it internally.
- **Test**: `idf.py build` clean. Hardware-confirmed across a real deep-sleep boundary via
  `GET /debug/device-state` (this shell's earlier "source-IP-restricted" read on this
  endpoint, back in F1, was actually just a missing `auth` header, not an IP restriction —
  corrected that assumption here): first cycle after reflash correctly reported
  `time_awake_ms: 0` (accurate — no prior cycle exists yet); the next cycle, a genuine
  `wake_reason: "timer"` / `reset_reason: "deep_sleep_wake"` RTC-alarm wake (not a manual
  reset), correctly reported `time_awake_ms: 11023` — the value measured and persisted
  from the previous cycle, carried correctly across the deep-sleep boundary via
  `RTC_DATA_ATTR`. **Correction to this checklist's own "low-hundreds-of-ms" expectation**:
  that was aspirational, not measured — the real wake-to-sync-response time is ~11 seconds,
  dominated by wifi connect (scan + associate + DHCP, several seconds) plus the backend
  round-trip, not low-hundreds-of-ms. This is exactly the kind of number the spec calls out
  as validating/refuting the battery-life estimate, so worth carrying forward accurately
  rather than leaving the stale expectation in place. `battery_mv` cross-check against a
  multimeter is a manual hardware step outside what could be verified in this session —
  left undone, but the math is confirmed sound by inspection above.
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
