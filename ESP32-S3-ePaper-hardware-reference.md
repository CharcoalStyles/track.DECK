# ESP32-S3-ePaper-1.54 — Hardware Reference for ESP-IDF

This document is a ground-truth reference for building firmware on the **Waveshare ESP32-S3-ePaper-1.54 (V2, non-touch)** dev board using **ESP-IDF** (not Arduino). It was extracted directly from Waveshare's official example repo for this board (`waveshare/ESP32-S3-ePaper-1.54`), specifically the `02_Example/ESP-IDF/V2/` example projects, which are working, tested ESP-IDF code — not derived from datasheets or guesswork. Pin numbers, chip identities, and API calls below were verified by reading the actual source.

If you are a fresh Claude Code session building a new project against this board, **treat this document as authoritative** for pin assignments and chip identity. Where the source repo is available alongside this doc, its `02_Example/ESP-IDF/V2/` examples are the best copy-source for working code; `01_Arduino_Libraries/` and `02_Example/Arduino/` are a separate Arduino-only track and should be ignored unless this doc says otherwise.

## Board identity

- **Board**: Waveshare ESP32-S3-ePaper-1.54, **V2 revision, non-touch**.
- **Target**: `esp32s3` (`idf.py set-target esp32s3`)
- **Flash**: 4MB, QIO mode
  ```
  CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
  CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
  ```
- **PSRAM**: Octal SPI PSRAM (this is the V2-specific setting — do not use Quad mode)
  ```
  CONFIG_SPIRAM=y
  CONFIG_SPIRAM_MODE_OCT=y
  CONFIG_SPIRAM_SPEED_80M=y
  ```
  (Note: an older V1 revision of this board exists with Quad PSRAM instead — pinout is identical, only this PSRAM mode config differs. If boot fails or PSRAM isn't detected, that's the first thing to check.)
- **ESP-IDF version this was built/tested against**: 5.5.1
- **Partition table** — single factory app, no OTA slot:
  ```csv
  # Name,   Type, SubType, Offset,  Size
  nvs,      data, nvs,     0x9000,  0x6000
  phy_init, data, phy,     0xf000,  0x1000
  factory,  app,  factory, 0x10000, 0x3F0000
  ```

## Pin map (confirmed)

| Function | GPIO(s) | Notes |
|---|---|---|
| E-paper DC / CS / SCK / MOSI / RST / BUSY | 10 / 11 / 12 / 13 / 9 / 8 | `SPI2_HOST`, 40 MHz, SPI mode 0, write-only (no MISO) |
| E-paper panel power enable | 6 | **active-low**: `gpio_set_level(6, 0)` = ON |
| I2C bus SDA / SCL | 47 / 48 | `I2C_NUM_0`, 400kHz — shared by RTC and SHTC3 |
| PCF85063 RTC (I2C) | addr `0x51` | INT/alarm line on GPIO5 |
| SHTC3 temp/humidity (I2C) | addr `0x70` | — |
| I2S audio (ES8311 codec) BCLK / WS / DOUT / DIN / MCLK | 15 / 38 / 45 / 16 / 14 | codec also uses the shared I2C bus (SDA47/SCL48) for control registers |
| Audio PA (amplifier) enable | 46 | codec_board `pa` pin |
| Audio circuit power enable | 42 | **active-low** ON |
| SD card CLK / CMD / D0 | 39 / 41 / 40 | **SDMMC 1-bit mode** — not SPI mode, uses the dedicated SDMMC peripheral |
| Battery rail enable/hold | 17 | `VBAT_PWR_PIN` — held via `rtc_gpio_hold_en` during deep sleep so the rail stays up |
| Battery voltage ADC | 4 | `ADC1_CHANNEL_3`, 12-bit, `ADC_ATTEN_DB_12`; measured voltage ≈ `raw_millivolts * 2` (resistor divider) |
| BOOT button | 0 | active-low; also a deep-sleep EXT1 wake source |
| PWR button | 18 | active-low; also a deep-sleep EXT1 wake source |
| RTC alarm/INT line | 5 | active-low, open-drain; also a deep-sleep EXT1 wake source |
| LED | 3 | simple output |

**Touch note**: this board revision has no touch controller. The touch variant (ESP32-S3-Touch-ePaper-1.54) adds an FT6336 capacitive touch IC on GPIO7 (reset) / GPIO21 (interrupt) / I2C addr `0x38` on the same shared I2C bus — irrelevant for this board, included here only so it isn't confused with a free GPIO if you ever see it mentioned elsewhere.

**Reserved/unverified GPIOs — do not assume free for new buttons/IO without checking**: GPIO19/20 (USB D-/D+, used for USB CDC), GPIO43/44 (default UART0 TX/RX, used for flashing and console logs), GPIO26-32 (SPI flash), and likely GPIO33-37 (extra lines needed for Octal PSRAM on this V2 board). Any GPIO not listed in the table above and not in this reserved list is a reasonable candidate for extra button/GPIO input, but double-check against the ESP32-S3 datasheet's strapping-pin list before wiring, since getting a strapping pin wrong can prevent boot.

## Component manager dependencies

The official examples pull these via `idf_component.yml` (component registry) rather than vendoring — do the same in the new project instead of copying source:

```yaml
dependencies:
  idf:
    version: '>=4.1.0'
  espressif/button: "*"
  espressif/esp_codec_dev: "==1.5.4"
  pedrominatel/shtc3: "^1.4.1"
  waveshare/pcf85063a: "^1.1.1"
```

`lvgl/lvgl` is an additional dependency only if you want a GUI toolkit on top of the raw e-paper framebuffer (see below) — omit it if you're driving the display directly.

## E-ink display

- **Controller**: SSD1681/SSD1680-family command set (200×200px, 1.54"), driven directly by opcode — this is **not** a port of GxEPD2 or any named open-source library; it's a bespoke driver in this repo.
- **Source to copy**: `02_Example/ESP-IDF/V2/11_FactoryProgram/components/port_bsp/port_display.{h,cpp}` (or any other V2 example's `epaper_driver_bsp.{h,cpp}` — same driver duplicated per example).
- **Framebuffer**: 1bpp, allocated in PSRAM (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`).
- **API** (class `epaper_driver_display`):
  - `epaper_driver_display(int width, int height, custom_lcd_spi_t spi_cfg)` — sets up SPI bus/GPIO, allocates framebuffer
  - `EPD_Init()` — full-refresh init, loads the `WF_Full_1IN54` LUT waveform table
  - `EPD_Clear()` — fills buffer white
  - `EPD_Display()` — pushes buffer to controller RAM and does a full refresh
  - `EPD_Init_Partial()` — re-inits with the `WF_PARTIAL_1IN54_0` LUT for partial refresh
  - `EPD_DisplayPartBaseImage()` — writes buffer to both RAM banks to set the partial-refresh base image (call once before the first partial update)
  - `EPD_DisplayPart()` — pushes buffer and does a partial refresh (fast, less flicker, use for most updates)
  - `EPD_DrawColorPixel(x, y, color)` — sets/clears a bit in the framebuffer (`DRIVER_COLOR_WHITE` / `DRIVER_COLOR_BLACK`)
- **Important**: the LUT waveform byte tables (`WF_Full_1IN54[159]`, `WF_PARTIAL_1IN54_0[159]`) are hand-tuned and baked into the driver source — copy them verbatim, don't try to regenerate or guess them.
- Remember to toggle the panel power pin (GPIO6, active-low) on before using the display and off when done/sleeping.
- **Optional GUI layer**: LVGL v8 or v9 can sit on top via a `flush_cb` that thresholds LVGL's RGB565 output to 1bpp and calls `EPD_DrawColorPixel` + `EPD_DisplayPart` (`full_refresh` must be forced to 1). See `09_LVGL_V8_Test` / `10_LVGL_V9_Test` / `port_lvgl.cpp` if you want this; otherwise draw directly to the framebuffer with your own graphics/font code.

## WiFi

Standard ESP-IDF WiFi, no board-specific pins involved.

- **STA (connect to a network)**: reference `02_Example/ESP-IDF/V2/06_WIFI_STA/` — `esp_wifi_init`, `esp_wifi_set_mode(WIFI_MODE_STA)`, `esp_wifi_set_config`, `esp_wifi_start`, standard `esp_netif`/`nvs_flash_init` boilerplate.
- **AP (host a network)**: reference `02_Example/ESP-IDF/V2/05_WIFI_AP/` — `esp_wifi_set_mode(WIFI_MODE_AP)`, station list via `esp_wifi_ap_get_sta_list`.

## Microphone + speaker

Both mic input and speaker output go through a **single ES8311 codec chip** (not separate discrete mic/amp ICs — don't assume an INMP441 or MAX98357A, this board doesn't have either).

- **I2S pins**: BCLK=15, WS/LRCLK=38, DOUT(to codec)=45, DIN(from codec)=16, MCLK=14
- **Codec control**: over the shared I2C bus (SDA47/SCL48)
- **PA enable**: GPIO46 (must be enabled to hear speaker output)
- **Audio circuit power**: GPIO42, active-low ON — gate this to save power when audio isn't in use
- **Driver**: use the `espressif/esp_codec_dev` managed component (pulled via `idf_component.yml` above), with the `codec_board` abstraction layer that maps a board id (`S3_ePaper_1_54`) to these exact pins — reference `02_Example/ESP-IDF/V2/08_Audio_Test/` and the `codec_board` component under `components/externlib/codec_board/` for the board config entry and usage pattern (`audio_bsp_init()`, playback/record calls).

## SD card

**SDMMC 1-bit mode** — uses the ESP32-S3's dedicated SDMMC peripheral, not the SPI peripheral (the pin names in some headers use SPI-style naming like `SD_MOSI_CMD_PIN` but this is misleading — it's genuinely SDMMC).

- Pins: CLK=39, CMD=41, D0=40
- `slot_config.width = 1`, `SDMMC_HOST_DEFAULT()`, `SDMMC_FREQ_HIGHSPEED`
- Mount point: `/sdcard`, via `esp_vfs_fat_sdmmc_mount`
- Reference: `02_Example/ESP-IDF/V2/04_SD_Card/` — `sdcard_init()`, plus example read/write file functions.

## Deep sleep / wake

Reference: `02_Example/ESP-IDF/V2/12_RTC_Sleep_Test/` and `components/board_power_bsp/board_power_bsp.cpp` (`EnableDeepLowPowerMode()`).

Pattern (use this, don't improvise a different one — this board needs the VBAT rail explicitly held or it will brown out during sleep):

```c
esp_sleep_pd_config(ESP_PD_DOMAIN_MAX, ESP_PD_OPTION_AUTO);
esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

const uint64_t wake_mask =
    (1ULL << GPIO_NUM_0)  |   // BOOT button
    (1ULL << GPIO_NUM_5)  |   // PCF85063 RTC alarm/INT (open-drain, active-low)
    (1ULL << GPIO_NUM_18);    // PWR button
esp_sleep_enable_ext1_wakeup_io(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);   // EXT1, not EXT0

rtc_gpio_pullup_en(GPIO_NUM_5);          // RTC INT line needs a pull-up to idle high
rtc_gpio_hold_en(VBAT_PWR_PIN);          // GPIO17 — keep the battery rail enabled through sleep

esp_deep_sleep_start();
```

On wake, check the cause and which pin fired:

```c
esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    uint64_t status = esp_sleep_get_ext1_wakeup_status();
    // bit for GPIO_NUM_18 set => woken by PWR button, etc.
}
```

Scheduled/timed wake (in addition to or instead of button wake) is done via the PCF85063 RTC alarm (`waveshare/pcf85063a` component) rather than the ESP32's own RTC timer — set an alarm before sleeping, since the RTC INT line (GPIO5) is already wired into the wake mask above.

## Extra GPIO / button inputs

For any additional simple buttons or digital inputs beyond BOOT/PWR, **reuse the same pattern the official examples use** rather than hand-rolling GPIO polling — it's the official Espressif `button` component (`espressif/button` in `idf_component.yml` above), wrapped in a small C++ convenience class in this repo at `02_Example/ESP-IDF/V2/11_FactoryProgram/components/externlib/my_button/button.{h,cc}`. It gives you debounce, click/double-click/long-press/multi-click events, and optional light-sleep wake support for free.

Usage pattern (from `components/app/user_app.cpp`):

```cpp
#include "button.h"

Button *my_button = new Button(GPIO_NUM_xx /* pick a free pin from the table above */,
                                /*active_high=*/false); // buttons on this board are active-low with internal pull-up

my_button->OnClick([]() {
    // handle short press
});
my_button->OnLongPress([]() {
    // handle long press
});
```

For a simple non-debounced digital input (e.g. a switch or sensor, not a human button), plain `driver/gpio.h` is fine: `gpio_set_direction(pin, GPIO_MODE_INPUT)`, `gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY)` (or `GPIO_PULLDOWN_ONLY` depending on wiring), `gpio_get_level(pin)` — no need to pull in the button component for that case.

## Project bootstrap checklist

1. `idf.py create-project <name>`, `idf.py set-target esp32s3`.
2. Copy the `sdkconfig.defaults` settings above (flash, octal PSRAM, partition table) into the new project.
3. Add the `idf_component.yml` dependencies listed above to `main/` (or a dedicated `board` component).
4. For each peripheral you need, copy the relevant `components/*_bsp` folder(s) out of `02_Example/ESP-IDF/V2/11_FactoryProgram/components/port_bsp/` (the most complete single integration — combines display, audio, SD, sensors, and power/sleep) or the smaller single-purpose example directories listed above if you want less to adapt at once. Register each as a component via `idf_component_register` in its `CMakeLists.txt`.
5. Do not pull anything from `01_Arduino_Libraries/` or `02_Example/Arduino/` — separate Arduino-only track, not needed for ESP-IDF.

## Things NOT to assume

- No GxEPD2 or Waveshare e-Paper library dependency — the display driver here is custom, copy it directly.
- SD card is SDMMC, not SPI mode.
- Audio is one ES8311 codec handling both mic and speaker, not separate mic/amp chips.
- No datasheets or schematics exist in the source repo — the pin values in this document, taken directly from the working example source, are the most authoritative reference available.
- No battery-charging-status GPIO is exposed anywhere in the source examples — only battery voltage (via ADC) and a rail enable/hold pin exist.
