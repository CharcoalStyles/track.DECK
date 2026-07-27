// Phase 1 bring-up test: exercises every subsystem once in a single boot,
// against the real backend, per PROJECT_PLAN.md's Phase 1 checklist. Not the
// final app behavior (that's Phase 2) -- just a smoke test proving the
// toolchain, drivers, and backend reachability all work together on real
// hardware.

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <sys/time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_log.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_attr.h>
#include <nvs_flash.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <esp_http_client.h>
#include <driver/rtc_io.h>

#include "cJSON.h"

#include "port_power.h"
#include "port_display.h"
#include "port_codec.h"
#include "port_adc.h"
#include "port_i2c.h"
#include "port_shtc3.h"
#include "port_sdcard.h"
#include "epaper_config.h"

#include "pcf85063a.h"

#include "sync_snapshot.h"
#include "tz_apply.h"
#include "sync_backoff.h"

#include "secrets.h"

static const char *TAG = "bringup";

// Not in epaper_config.h (which only lists EPD/power/i2c/sdcard/button
// pins) -- from the hardware reference doc's pin table ("RTC alarm/INT
// line | GPIO5").
#define RTC_INT_PIN GPIO_NUM_5

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
// ~3-4s per auth attempt observed against this AP; 10 covers the 30s
// connect timeout below instead of exhausting around 15-20s.
#define WIFI_MAX_RETRY 10
#define FALLBACK_POLL_INTERVAL_SECONDS 300

static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_count = 0;
static int64_t s_boot_time_us;

// ---------------------------------------------------------------------
// Persistent across deep sleep (RTC slow memory) -- reset to these
// initializers only on a real power-on, not on a deep-sleep wake. Backs
// the "wake every 1 minute for a clock tick, sync every Nth wake" scheme:
// only the sync wake talks to wifi/the backend; the rest just repaint
// the clock. TZ has to be re-applied every wake too, since setenv/tzset
// state lives in normal RAM and does not survive deep sleep either.
// ---------------------------------------------------------------------
static RTC_DATA_ATTR uint32_t s_wake_tick_count = 0;
static RTC_DATA_ATTR int s_ticks_per_sync = 5; // derived from poll_interval_seconds/60
static RTC_DATA_ATTR char s_tz_posix[64] = "UTC0";
static RTC_DATA_ATTR bool s_epd_seeded = false; // SSD1681 RAM holds a valid base image

// ---------------------------------------------------------------------
// Reset reason / wake reason (spec section 3.1's field table)
// ---------------------------------------------------------------------

static const char *reset_reason_to_string(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "power_on";
        case ESP_RST_DEEPSLEEP: return "deep_sleep_wake";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_PANIC: return "panic";
        default: return "unknown";
    }
}

static const char *wake_reason_to_string(void) {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_EXT1) {
        uint64_t status = esp_sleep_get_ext1_wakeup_status();
        if (status & (1ULL << RTC_INT_PIN)) {
            return "timer";
        }
        return "button";
    }
    return "power_on";
}

// ---------------------------------------------------------------------
// Wifi STA connect (06_WIFI_STA pattern)
// ---------------------------------------------------------------------

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry_count < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_wifi_retry_count++;
            ESP_LOGW(TAG, "wifi disconnected, retry %d/%d", s_wifi_retry_count, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_connect(void) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
    // Explicit "PMF capable, not required" -- the AP's WPA2/WPA3 transition
    // mode broadcasts PMF-optional, but leaving this unset relies on
    // whatever the library default happens to be, which is a known source
    // of slow/flaky SAE handshakes against exactly this kind of mixed-mode AP.
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    // threshold.authmode is a *minimum* acceptable security floor for
    // filtering scan results, not a cap -- setting it to WPA2_PSK alone
    // does NOT stop the driver from choosing WPA3-SAE against a
    // WPA2/WPA3-transition-mode AP (confirmed against Espressif's own
    // wifi-security docs). It's set here anyway as a floor against
    // accidentally connecting to something weaker (e.g. WEP/open).
    // The actual fix for the flaky SAE handshake is the
    // CONFIG_ESP_WIFI_ENABLE_WPA3_SAE=n Kconfig option (sdkconfig.defaults),
    // which removes SAE from consideration at compile time.
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to wifi SSID \"%s\"...", WIFI_SSID);
    // 30s, not the more typical 10-15s: this AP's WPA2/WPA3-transition-mode
    // SAE handshake has been observed taking 15-20s across several auth
    // retries before succeeding (see PROJECT_PLAN.md's Phase 1 notes).
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    bool connected = (bits & WIFI_CONNECTED_BIT) != 0;
    ESP_LOGI(TAG, "%s", connected ? "wifi connected" : "wifi connect failed/timed out");
    return connected;
}

static int get_rssi_dbm(void) {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

// ---------------------------------------------------------------------
// POST /device/sync
// ---------------------------------------------------------------------

struct http_response_buf_t {
    char *data;
    size_t capacity;
    size_t written;
};

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        auto *buf = static_cast<http_response_buf_t *>(evt->user_data);
        if (buf && buf->data && buf->written + 1 < buf->capacity) {
            size_t copy_len = evt->data_len;
            if (copy_len > buf->capacity - 1 - buf->written) {
                copy_len = buf->capacity - 1 - buf->written;
            }
            memcpy(buf->data + buf->written, evt->data, copy_len);
            buf->written += copy_len;
            buf->data[buf->written] = '\0';
        }
    }
    return ESP_OK;
}

// One attempt at POST /device/sync. Returns true on a 2xx response.
static bool device_sync_attempt(const char *request_body, http_response_buf_t *resp, int *out_status) {
    resp->written = 0;
    resp->data[0] = '\0';

    char url[256];
    snprintf(url, sizeof(url), "%s/device/sync", BACKEND_BASE_URL);

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.event_handler = http_event_handler;
    config.user_data = resp;
    config.timeout_ms = 10000;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "auth", API_TOKEN);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, request_body, strlen(request_body));

    esp_err_t err = esp_http_client_perform(client);
    bool ok = false;
    if (err == ESP_OK) {
        *out_status = esp_http_client_get_status_code(client);
        ok = (*out_status >= 200 && *out_status < 300);
    } else {
        ESP_LOGE(TAG, "http_client_perform failed: %s", esp_err_to_name(err));
        *out_status = -1;
    }
    esp_http_client_cleanup(client);
    return ok;
}

// Bounded retry with backoff per spec section 5. Telemetry body here is
// mostly hardcoded/derived-from-boot-state, per the Phase 1 checklist --
// wiring in live sensor readings (battery_mv etc.) is Phase 2 (F1/F5).
static bool device_sync(const char *wake_reason, const char *reset_reason, http_response_buf_t *resp, int *out_status) {
    int64_t time_awake_ms = (esp_timer_get_time() - s_boot_time_us) / 1000;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "wake_reason", wake_reason);
    cJSON_AddStringToObject(root, "firmware_version", "0.1.0-bringup");
    cJSON_AddNumberToObject(root, "rssi_dbm", get_rssi_dbm());
    cJSON_AddNumberToObject(root, "time_awake_ms", (double)time_awake_ms);
    cJSON_AddStringToObject(root, "reset_reason", reset_reason);
    char *body = cJSON_PrintUnformatted(root);

    sync_backoff_t backoff;
    sync_backoff_reset(&backoff);
    bool ok = false;
    while (sync_backoff_should_retry(&backoff)) {
        sync_backoff_record_attempt(&backoff);
        ok = device_sync_attempt(body, resp, out_status);
        if (ok) {
            break;
        }
        if (sync_backoff_should_retry(&backoff)) {
            uint32_t delay_ms = sync_backoff_delay_ms(&backoff);
            ESP_LOGW(TAG, "/device/sync attempt failed (status %d), retrying in %ums", *out_status, (unsigned)delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }

    cJSON_free(body);
    cJSON_Delete(root);
    return ok;
}

static void log_snapshot(const sync_snapshot_t &snap) {
    ESP_LOGI(TAG, "snapshot.now = %s%lld", snap.has_now ? "" : "(missing) ", (long long)snap.now);
    ESP_LOGI(TAG, "snapshot.timezone.posix = %s", snap.has_timezone_posix ? snap.timezone_posix : "(missing)");
    ESP_LOGI(TAG, "snapshot.poll_interval_seconds = %s%d", snap.has_poll_interval_seconds ? "" : "(missing) ", snap.poll_interval_seconds);
    ESP_LOGI(TAG, "snapshot.bedtime = %s", snap.has_bedtime ? snap.bedtime : "(missing)");
    ESP_LOGI(TAG, "snapshot.next_wake_at = %s%lld", snap.has_next_wake_at ? "" : "(missing) ", (long long)snap.next_wake_at);
    ESP_LOGI(TAG, "snapshot.checkins: valid=%d count=%d", snap.checkins_valid, snap.checkins_count);
    ESP_LOGI(TAG, "snapshot.reminders: valid=%d count=%d", snap.reminders_valid, snap.reminders_count);
    ESP_LOGI(TAG, "snapshot.calendar_events: valid=%d count=%d", snap.calendar_events_valid, snap.calendar_events_count);
    if (snap.has_weather) {
        ESP_LOGI(TAG, "snapshot.weather: temp=%.1fC cloud=%d%% sunrise=%lld sunset=%lld",
                 snap.weather.temperature_c, snap.weather.cloud_cover_pct,
                 (long long)snap.weather.sunrise, (long long)snap.weather.sunset);
    } else {
        ESP_LOGI(TAG, "snapshot.weather = (missing/null)");
    }
}

// ---------------------------------------------------------------------
// RTC (PCF85063A): set from server `now`, schedule the next wake alarm.
// The RTC always stores UTC; POSIX TZ is applied separately for local
// wall-clock rendering, never baked into what's stored on the chip.
// ---------------------------------------------------------------------

static void rtc_set_time_utc(pcf85063a_dev_t *rtc, int64_t now_epoch) {
    time_t t = (time_t)now_epoch;
    struct tm utc_tm;
    gmtime_r(&t, &utc_tm);

    pcf85063a_datetime_t dt = {};
    dt.year = utc_tm.tm_year + 1900;
    dt.month = utc_tm.tm_mon + 1;
    dt.day = utc_tm.tm_mday;
    dt.dotw = utc_tm.tm_wday;
    dt.hour = utc_tm.tm_hour;
    dt.min = utc_tm.tm_min;
    dt.sec = utc_tm.tm_sec;
    pcf85063a_set_time_date(rtc, dt);
    ESP_LOGI(TAG, "RTC set from server `now` (UTC): %04d-%02d-%02d %02d:%02d:%02d",
             dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec);
}

// Schedules the alarm for the start of the *next whole minute* (sec=0),
// not "current time + 60s" -- the latter drifts away from true minute
// boundaries over many cycles, since how many seconds each wake actually
// takes (wifi, e-ink, audio...) varies slightly cycle to cycle. Rounding
// up to the next minute boundary instead is self-correcting: it's
// recomputed fresh from the current RTC reading every time, so there's
// no accumulating drift, and the on-screen clock updates right at :00
// seconds of each minute rather than a few seconds late. Pure
// seconds-of-day arithmetic: the PCF85063 alarm only matches sec/min/hour
// (day and weekday are always disabled by pcf85063a_set_alarm), so no
// need to round-trip through epoch time or worry about TZ/DST here.
static void rtc_schedule_alarm(pcf85063a_dev_t *rtc) {
    pcf85063a_datetime_t cur = {};
    pcf85063a_get_time_date(rtc, &cur);

    long seconds_of_day = (long)cur.hour * 3600 + (long)cur.min * 60 + cur.sec;
    long alarm_seconds_of_day = (((seconds_of_day / 60) + 1) * 60) % 86400;

    pcf85063a_datetime_t alarm_dt = {};
    alarm_dt.hour = (uint8_t)(alarm_seconds_of_day / 3600);
    alarm_dt.min = (uint8_t)((alarm_seconds_of_day % 3600) / 60);
    alarm_dt.sec = 0;

    pcf85063a_set_alarm(rtc, alarm_dt);
    pcf85063a_enable_alarm(rtc); // also clears any stale alarm flag
    ESP_LOGI(TAG, "RTC alarm scheduled at %02d:%02d:%02d UTC (next minute boundary)",
             alarm_dt.hour, alarm_dt.min, alarm_dt.sec);
}

// ---------------------------------------------------------------------
// Minimal 5x7 bitmap font -- digits 0-9 and a colon, just enough to
// render "HH:MM". No font/text-rendering library exists yet; this is a
// deliberately tiny hand-encoded table, not a general-purpose one.
// ---------------------------------------------------------------------

// clang-format off
static const uint8_t FONT_5X7[11][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
    {0x0E,0x11,0x01,0x0E,0x10,0x10,0x1F}, // 2
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, // 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
    {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}, // 10 = ':'
};
// clang-format on

#define CLOCK_GLYPH_SCALE 4
#define CLOCK_GLYPH_W (5 * CLOCK_GLYPH_SCALE + CLOCK_GLYPH_SCALE) // 5 cols + 1 col gap
#define CLOCK_GLYPH_H (7 * CLOCK_GLYPH_SCALE)
#define CLOCK_X0 20
#define CLOCK_Y0 20
#define CLOCK_W (5 * CLOCK_GLYPH_W) // "HH:MM" = 5 glyphs
#define CLOCK_H CLOCK_GLYPH_H

static void draw_glyph(int glyph_index, int x0, int y0, uint8_t color) {
    for (int row = 0; row < 7; row++) {
        uint8_t bits = FONT_5X7[glyph_index][row];
        for (int col = 0; col < 5; col++) {
            if (!((bits >> (4 - col)) & 0x1)) {
                continue;
            }
            for (int sy = 0; sy < CLOCK_GLYPH_SCALE; sy++) {
                for (int sx = 0; sx < CLOCK_GLYPH_SCALE; sx++) {
                    EPD_DrawColorPixel(x0 + col * CLOCK_GLYPH_SCALE + sx, y0 + row * CLOCK_GLYPH_SCALE + sy, color);
                }
            }
        }
    }
}

// Clears just the clock's own bounding box (not the whole screen) and
// draws "HH:MM" into it -- used by both the full-seed and the quiet
// partial-tick paths, so the clock area itself is always redrawn from
// scratch (the software framebuffer doesn't survive deep sleep either).
static void draw_clock(int hour, int minute) {
    for (int y = CLOCK_Y0; y < CLOCK_Y0 + CLOCK_H; y++) {
        for (int x = CLOCK_X0; x < CLOCK_X0 + CLOCK_W; x++) {
            EPD_DrawColorPixel(x, y, DRIVER_COLOR_WHITE);
        }
    }
    int glyphs[5] = {hour / 10, hour % 10, 10, minute / 10, minute % 10};
    for (int i = 0; i < 5; i++) {
        draw_glyph(glyphs[i], CLOCK_X0 + i * CLOCK_GLYPH_W, CLOCK_Y0, DRIVER_COLOR_BLACK);
    }
}

// ---------------------------------------------------------------------
// E-ink: the SSD1681 loses all internal RAM/LUT state whenever
// EPD_PWR_PIN is cut, so a genuinely cheap+quiet partial update is only
// possible if the display stays powered between wakes (see
// enter_deep_sleep -- it deliberately does NOT power off the EPD).
// eink_full_seed_with_clock does one full-style refresh (visible flash)
// that also seeds both of the chip's internal RAM copies, establishing
// the baseline that later quiet partial ticks diff against.
// eink_clock_tick assumes that baseline is already valid and does a
// fast, quiet partial update of just the clock digits.
// ---------------------------------------------------------------------

static void eink_full_seed_with_clock(int hour, int minute) {
    PortDisplay_Init();
    EPD_Init();
    // Plain white background, not the old Phase-1 checkerboard smoke-test
    // pattern -- a high-contrast alternating pattern is close to
    // worst-case for showing partial-refresh ghosting, and this content
    // now gets redrawn for real every few minutes rather than once as a
    // one-off test.
    EPD_Clear();
    draw_clock(hour, minute);
    // EPD_DisplayPartBaseImage() already performs its own full-quality
    // visible refresh (via EPD_TurnOnDisplay internally) *and* seeds both
    // of the chip's internal RAM copies needed for partial diffing --
    // calling EPD_Display() first as well would refresh the identical
    // content to the screen twice in a row for no reason.
    EPD_DisplayPartBaseImage();
    ESP_LOGI(TAG, "e-ink full refresh done (clock)");

    EPD_Init_Partial();
    s_epd_seeded = true;
    ESP_LOGI(TAG, "e-ink partial mode seeded");
}

static void eink_clock_tick(int hour, int minute) {
    // Deep sleep wipes the ESP32's own SPI/GPIO state and the software
    // framebuffer every wake regardless of what stays externally powered,
    // so PortDisplay_Init() (ESP32-side re-init only) is still required
    // here. EPD_Init() must NOT be called, though -- it pulses the
    // SSD1681's hardware RST line, which would wipe the chip's own
    // internal RAM/LUT state that we kept it powered specifically to
    // preserve. The partial LUT loaded by the last EPD_Init_Partial()
    // (during the last full-seed) is still active in the chip.
    PortDisplay_Init();
    draw_clock(hour, minute);
    EPD_DisplayPart();
    ESP_LOGI(TAG, "e-ink clock tick done (%02d:%02d)", hour, minute);
}

// ---------------------------------------------------------------------
// Audio: real loopback -- record from the mic, play it back.
// ---------------------------------------------------------------------

static void audio_loopback_test(void) {
    BoardPower_Audio_ON();
    Codec_StartInit();

    const int sample_rate = 16000;
    const int channels = 2;
    const int seconds = 3;
    const size_t bytes = (size_t)sample_rate * channels * sizeof(int16_t) * seconds;

    auto *buf = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    if (!buf) {
        ESP_LOGE(TAG, "audio loopback: failed to allocate %u byte buffer", (unsigned)bytes);
        return;
    }

    ESP_LOGI(TAG, "recording %ds...", seconds);
    Codec_RecordData(buf, bytes);
    ESP_LOGI(TAG, "recording done, playing back...");
    Codec_PlaybackData(buf, bytes);
    ESP_LOGI(TAG, "audio loopback done");

    heap_caps_free(buf);
}

// ---------------------------------------------------------------------
// SD card: mount, write, read back, verify.
// ---------------------------------------------------------------------

static void sdcard_test(void) {
    if (!Sdcard_Init()) {
        ESP_LOGE(TAG, "SD card mount failed");
        return;
    }
    const char *path = SDlist "/bringup_test.txt";
    const char *content = "adhi-firmware Phase 1 bring-up test\n";

    esp_err_t err = Sdcard_WriteFile(path, (char *)content);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD card write failed: %s", esp_err_to_name(err));
        return;
    }

    char readback[128] = {0};
    uint32_t read_len = 0;
    err = Sdcard_ReadFile(path, readback, &read_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD card read failed: %s", esp_err_to_name(err));
        return;
    }

    bool matches = (read_len == strlen(content)) && (memcmp(readback, content, read_len) == 0);
    ESP_LOGI(TAG, "SD card write/read %s (%u bytes)", matches ? "OK" : "MISMATCH", (unsigned)read_len);
}

// ---------------------------------------------------------------------
// Deep sleep: hold VBAT, EXT1 wake on BOOT/PWR/RTC-INT.
// ---------------------------------------------------------------------

static void enter_deep_sleep(void) {
    ESP_LOGI(TAG, "entering deep sleep now");

    esp_sleep_pd_config(ESP_PD_DOMAIN_MAX, ESP_PD_OPTION_AUTO);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    const uint64_t ext1_mask = (1ULL << BOOT_BUTTON_PIN) | (1ULL << PWR_BUTTON_PIN) | (1ULL << RTC_INT_PIN);
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(ext1_mask, ESP_EXT1_WAKEUP_ANY_LOW));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(RTC_INT_PIN));
    ESP_ERROR_CHECK(rtc_gpio_pullup_en(RTC_INT_PIN));

    // Deliberately NOT powering off the EPD here (no BoardPower_EPD_OFF):
    // wakes are only 60s apart now, and the SSD1681 loses all internal
    // RAM/LUT state the instant it loses power, which would force a full
    // visible-flash reseed on every single clock tick. Its standby draw
    // is tiny next to a full refresh or a wifi sync, so leaving it
    // powered between these short sleeps is the right tradeoff -- but
    // the EPD power-rail GPIO itself needs the same rtc_gpio_hold_en
    // treatment as VBAT, or GPIO sleep-isolation could let it float.
    BoardPower_Audio_OFF();
    ESP_ERROR_CHECK(rtc_gpio_hold_en((gpio_num_t)VBAT_PWR_PIN));
    ESP_ERROR_CHECK(rtc_gpio_hold_en((gpio_num_t)EPD_PWR_PIN));

    esp_deep_sleep_start();
}

// ---------------------------------------------------------------------

extern "C" void app_main(void) {
    s_boot_time_us = esp_timer_get_time();

    // NVS init first, per standard ESP-IDF convention -- required before
    // wifi, and independent of every other subsystem below.
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    // VBAT and EPD power-rail holds both survive deep sleep; release
    // before we can drive those GPIOs again this cycle.
    rtc_gpio_hold_dis((gpio_num_t)VBAT_PWR_PIN);
    rtc_gpio_hold_dis((gpio_num_t)EPD_PWR_PIN);

    const char *reset_reason = reset_reason_to_string(esp_reset_reason());
    const char *wake_reason = wake_reason_to_string();

    // Wakes every ~60s; only every s_ticks_per_sync'th one talks to wifi
    // and the backend. Always sync on the very first-ever wake (real
    // power-on) so there's real data immediately instead of waiting.
    s_wake_tick_count++;
    bool is_sync_wake = (s_wake_tick_count == 1) || (s_wake_tick_count % (uint32_t)s_ticks_per_sync == 0);
    ESP_LOGI(TAG, "boot: reset_reason=%s wake_reason=%s tick=%u sync_wake=%d",
             reset_reason, wake_reason, (unsigned)s_wake_tick_count, is_sync_wake);

    BoardPower_Init();
    BoardPower_VBAT_ON();
    BoardPower_EPD_ON();
    vTaskDelay(pdMS_TO_TICKS(100));

    // TZ env state lives in normal RAM and does not survive deep sleep;
    // re-apply the last-known value every wake so localtime() stays
    // correct on the lightweight ticks in between real syncs.
    sync_tz_apply(s_tz_posix, "UTC0");

    // On a quick tick there's nothing else to wait for, so render right
    // away to minimize wake-to-visible-update latency. A sync wake
    // deliberately does NOT render yet -- once Phase 2 renders real
    // snapshot content (checkins/reminders/etc.) here too, rendering
    // before sync completes would mean a quick clock-only write followed
    // by a second, fuller write moments later. Render once, after
    // everything for this cycle is ready, instead.
    if (!is_sync_wake) {
        time_t now_epoch = time(nullptr);
        struct tm local_tm;
        localtime_r(&now_epoch, &local_tm);
        if (s_epd_seeded) {
            eink_clock_tick(local_tm.tm_hour, local_tm.tm_min);
        } else {
            eink_full_seed_with_clock(local_tm.tm_hour, local_tm.tm_min);
        }
    }

    // I2C bus is shared by the RTC and the SHTC3 sensor (the audio codec
    // manages its own separate I2C init internally).
    I2cMasterBus *i2c_bus = I2cMasterBus::requestInstance(ESP32_I2C_SCL_PIN, ESP32_I2C_SDA_PIN, ESP32_I2C_DEV_NUM);
    pcf85063a_dev_t rtc_dev;
    ESP_ERROR_CHECK(pcf85063a_init(&rtc_dev, i2c_bus->Get_I2cBusHandle(), PCF85063A_ADDRESS));

    if (is_sync_wake) {
        Shtc3_Init(i2c_bus);

        bool wifi_ok = wifi_connect();

        if (wifi_ok) {
            const size_t resp_capacity = 8192;
            http_response_buf_t resp;
            resp.data = static_cast<char *>(heap_caps_malloc(resp_capacity, MALLOC_CAP_SPIRAM));
            resp.capacity = resp_capacity;
            resp.written = 0;

            int http_status = 0;
            bool sync_ok = device_sync(wake_reason, reset_reason, &resp, &http_status);

            if (sync_ok) {
                ESP_LOGI(TAG, "/device/sync OK (status %d)", http_status);
                // sync_snapshot_t is ~8.4KB -- far too large for the 3.5KB
                // main task stack (CONFIG_ESP_MAIN_TASK_STACK_SIZE), so it
                // must live in heap/PSRAM, not as a stack local.
                auto *snap = static_cast<sync_snapshot_t *>(heap_caps_malloc(sizeof(sync_snapshot_t), MALLOC_CAP_SPIRAM));
                if (sync_snapshot_parse(resp.data, snap)) {
                    log_snapshot(*snap);
                    if (snap->has_timezone_posix) {
                        strncpy(s_tz_posix, snap->timezone_posix, sizeof(s_tz_posix) - 1);
                        s_tz_posix[sizeof(s_tz_posix) - 1] = '\0';
                    }
                    const char *applied_tz = sync_tz_apply(s_tz_posix, "UTC0");
                    ESP_LOGI(TAG, "TZ applied: %s", applied_tz ? applied_tz : "(none)");

                    if (snap->has_now) {
                        struct timeval tv = {};
                        tv.tv_sec = (time_t)snap->now;
                        settimeofday(&tv, nullptr);
                        rtc_set_time_utc(&rtc_dev, snap->now);
                    }
                    // Sync cadence in wake-ticks, derived from the
                    // server's freshest poll_interval_seconds -- not the
                    // sleep duration anymore (that's always 60s now).
                    int poll_interval = snap->has_poll_interval_seconds ? snap->poll_interval_seconds : FALLBACK_POLL_INTERVAL_SECONDS;
                    s_ticks_per_sync = (poll_interval / 60 > 0) ? (poll_interval / 60) : 1;
                } else {
                    ESP_LOGE(TAG, "/device/sync response failed to parse as JSON at all");
                }
                heap_caps_free(snap);
            } else {
                ESP_LOGE(TAG, "/device/sync failed after retries (last status %d)", http_status);
            }

            heap_caps_free(resp.data);
        } else {
            ESP_LOGE(TAG, "skipping /device/sync -- wifi never connected");
        }
        // s_ticks_per_sync is deliberately left untouched on any failure
        // above -- it already holds the last-known-good cadence (or its
        // own default), so a failed cycle just retries at the next
        // regularly scheduled sync-wake rather than changing behavior.

        BoardAdc_Init();
        float vbat = Get_VbatVoltage();
        ESP_LOGI(TAG, "battery: %.3fV (%.0f%%)", vbat, (double)Get_Batterylevel());

        float temp_c = 0, humidity_pct = 0;
        Shtc3_ReadTempHumi(&temp_c, &humidity_pct);
        ESP_LOGI(TAG, "SHTC3: temp=%.1fC humidity=%.1f%%", temp_c, humidity_pct);

        // Single render for this cycle, now that sync (if it succeeded)
        // has already updated the TZ/RTC above -- see the comment above
        // the quick-tick branch for why this waits until here.
        time_t now_epoch = time(nullptr);
        struct tm local_tm;
        localtime_r(&now_epoch, &local_tm);
        eink_full_seed_with_clock(local_tm.tm_hour, local_tm.tm_min);

        audio_loopback_test();
        sdcard_test();
    }

    rtc_schedule_alarm(&rtc_dev);
    enter_deep_sleep();
}
