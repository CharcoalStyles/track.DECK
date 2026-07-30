# track.DECK firmware

ESP-IDF firmware for a dual-purpose bedside device built on the Waveshare
ESP32-S3-ePaper-1.54 V2 (non-touch board, ESP32-S3-PICO-1 SiP, Octal PSRAM,
8MB flash).

The device does two things on one physical board:

1. **Push-to-talk voice input** — press a button, speak, release; audio
   uploads to the backend fire-and-forget (no reply is ever read over HTTP).
2. **Periodic deep-sleep sync + eink display** — wakes on an RTC alarm, pulls
   a 24h snapshot (check-ins, reminders, calendar events, weather) from the
   backend, renders it to the eink display, goes back to sleep.
3. **Exact-time reminder wake + chime** — the device also wakes itself for a
   reminder's exact `due_at` (independent of the regular poll interval) and
   plays a short chime, using a compact reminder cache refreshed at the last
   full sync so this wake costs no wifi/backend round-trip.

For check-ins and calendar events this device is a display/input surface,
not the delivery mechanism — the backend's own scheduler and phone push
notifications are what actually fire those. Reminders are the exception: the
device wakes and chimes for them directly, in addition to whatever the
backend/phone side does. A sync failure or stale display is a degraded UX,
never a missed reminder — firmware must never let a retry loop, crash, or
sync failure burn battery or brick the device.

The backend this firmware talks to (FastAPI + LangGraph) lives in a separate
repository and isn't part of this codebase.

## Reference documents

- **`ESP32-S3-ePaper-hardware-reference.md`** — board identity, pin
  assignments, peripheral drivers, ESP-IDF configuration, extracted from
  Waveshare's official example repo.
- **`ESP32_FIRMWARE_SPEC.md`** — the network/behavioral contract this
  firmware implements against the backend (endpoints, headers, sync
  cadence, error handling).
- **`PROJECT_PLAN.md`** — historical record of the initial buildout
  (environment setup, bring-up test, F1-F9 feature list with test criteria).
  The roadmap it describes is complete; it's kept for bring-up rationale,
  not as a live backlog.
- **`CLAUDE.md`** — instructions for AI-assisted work in this repo,
  including the firmware-versioning policy below.

## Hardware summary

- Target `esp32s3`, ESP-IDF 5.5.1, 8MB flash QIO, Octal SPI PSRAM
  (V2-specific — not Quad), single-factory partition table with no OTA slot.
- E-paper: SSD1681/SSD1680-family, custom bespoke driver (`port_bsp`), not
  GxEPD2.
- Audio: single ES8311 codec for both mic and speaker, via
  `espressif/esp_codec_dev` + the `codec_board` abstraction.
- SD card: SDMMC 1-bit mode.
- Deep sleep: EXT1 wake on BOOT/PWR/RTC-INT pins; RTC alarms via the
  PCF85063 (`waveshare/pcf85063a`), not the ESP32's own RTC timer.
- No OTA, no multi-device/fleet concept, no offline queueing or delta sync,
  no synthesized TTS reply.

See `ESP32-S3-ePaper-hardware-reference.md` for the full pin map and
peripheral details.

## Repository layout

```
main/adhi-firmware.cpp     app_main, wifi, sync, eink render, deep sleep
main/secrets.h(.example)   BACKEND_BASE_URL, API_TOKEN, WIFI_NETWORKS (gitignored)
components/port_bsp/       board support: display, codec, i2c, sdcard, power, adc, shtc3
components/sync_proto/     sync snapshot parsing, backoff, TZ apply (+ host unit tests)
components/codec_board/    ES8311 codec board abstraction (espressif/esp_codec_dev)
components/my_button/      button wrapper around espressif/button
partitions.csv              single factory app partition, no OTA slot
sdkconfig.defaults          board-specific config, each entry documented inline
```

## Building and flashing

Requires ESP-IDF 5.5.1 with its environment sourced (`. $IDF_PATH/export.sh`).

```sh
cp main/secrets.h.example main/secrets.h   # fill in real backend URL/token/wifi
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

`main/secrets.h` is gitignored — it holds `BACKEND_BASE_URL`, `API_TOKEN`,
and the compiled-in `WIFI_NETWORKS` scan-then-match list. There is no
runtime provisioning path; all of these are rotated only by reflashing.

## Testing

`components/sync_proto` has host-native unit tests (Catch2, via ESP-IDF's
Linux target) for snapshot parsing, backoff, and timezone application:

```sh
cd components/sync_proto/test_apps/host
idf.py --preview set-target linux
idf.py build
./build/sync_proto_test.elf
```

## Firmware versioning

`FIRMWARE_VERSION` (`main/adhi-firmware.cpp`) is sent on every `/device/sync`
and `/device/error` request. **Bump it in the same commit as any change to
`main/` or `components/` source** — it's the only way to correlate
backend-reported telemetry/errors with what code was actually running on
the device. See `CLAUDE.md` for the full policy.
