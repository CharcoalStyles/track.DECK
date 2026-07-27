// Phase 1 bring-up test: exercises every subsystem once in a single boot,
// against the real backend, per PROJECT_PLAN.md's Phase 1 checklist. Not the
// final app behavior (that's Phase 2) -- just a smoke test proving the
// toolchain, drivers, and backend reachability all work together on real
// hardware.

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <cstdarg>
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
#include <driver/gpio.h>

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
#include "button.h"

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
// initializers only on a real power-on, not on a deep-sleep wake.
// s_tz_posix is re-applied every wake since setenv/tzset state lives in
// normal RAM and does not survive deep sleep; this keeps localtime()/log
// timestamps correct even on a cycle whose own sync attempt fails.
// s_last_known_good_poll_interval/s_has_synced_ever back spec section 5's
// "sleep for the last-known-good poll_interval_seconds" requirement --
// this has to survive across cycles, not just across retries within one
// cycle, since a fresh boot after a failed sync should still remember
// what the last *successful* sync said.
// ---------------------------------------------------------------------
static RTC_DATA_ATTR char s_tz_posix[64] = "UTC0";
static RTC_DATA_ATTR int s_last_known_good_poll_interval = FALLBACK_POLL_INTERVAL_SECONDS;
static RTC_DATA_ATTR bool s_has_synced_ever = false;
// F5 (PROJECT_PLAN.md): spec section 4.1 wants time_awake_ms measured from
// wake to the moment the /device/sync response is received -- which can't
// be known *before* sending the very request that reports it. Per the
// spec's own suggested alternative ("track it locally and report on the
// next call"), this holds the *previous* cycle's measured awake duration,
// reported on the current cycle's sync call. Starts at 0 for the very
// first-ever boot, which is accurate (there was no prior cycle).
static RTC_DATA_ATTR int64_t s_last_cycle_time_awake_ms = 0;

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

// F6 (PROJECT_PLAN.md): BOOT is dedicated to push-to-talk, diverging from
// PWR/RTC-alarm wakes (which still run the normal sync cycle). Checked
// once at the very top of app_main, before any of the normal-cycle setup,
// to decide which top-level path to take -- not part of
// wake_reason_to_string() above, since PTT wakes never call device_sync()
// and so never need a wake_reason value for the backend at all. If BOOT
// and RTC_INT both happen to be set (alarm firing at the same instant as
// a press), PTT wins: a live human interaction should preempt a
// background sync, which just happens again next interval regardless.
static bool wake_was_boot_button(void) {
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) {
        return false;
    }
    uint64_t status = esp_sleep_get_ext1_wakeup_status();
    return (status & (1ULL << BOOT_BUTTON_PIN)) != 0;
}

// ---------------------------------------------------------------------
// Wifi STA connect (06_WIFI_STA pattern)
// ---------------------------------------------------------------------

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
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
    // No WIFI_EVENT_STA_START handling here (deliberately) -- the initial
    // connect is now issued explicitly from wifi_connect() itself, once a
    // network has been selected via scan-then-match, rather than firing
    // blind as soon as the driver starts.
}

// Active-scans for all visible APs, then matches against the compiled-in
// WIFI_NETWORKS[] list (secrets.h) -- picks the strongest-RSSI match
// rather than trying each known network serially. A serial try-then-wait
// approach would multiply worst-case wake time (each failed attempt costs
// up to the full connect timeout below) against the time_awake_ms
// battery-life budget the spec cares about; one scan is a fixed, small
// cost (~1-3s) regardless of how many networks are configured. Returns
// true and fills out_ssid/out_password if a known network was seen in the
// scan; false if none of the visible APs matched anything we know.
static bool wifi_scan_and_select(char *out_ssid, size_t out_ssid_len, char *out_password, size_t out_password_len) {
    wifi_scan_config_t scan_config = {};
    esp_err_t err = esp_wifi_scan_start(&scan_config, true /* block until done */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi scan failed: %s", esp_err_to_name(err));
        return false;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        ESP_LOGW(TAG, "wifi scan found no APs");
        return false;
    }

    auto *ap_records = static_cast<wifi_ap_record_t *>(heap_caps_malloc(sizeof(wifi_ap_record_t) * ap_count, MALLOC_CAP_SPIRAM));
    if (!ap_records) {
        ESP_LOGE(TAG, "wifi scan: failed to allocate %u AP records", (unsigned)ap_count);
        return false;
    }
    esp_wifi_scan_get_ap_records(&ap_count, ap_records);

    int best_known_index = -1;
    int8_t best_rssi = INT8_MIN;
    for (int i = 0; i < (int)ap_count; i++) {
        for (size_t k = 0; k < WIFI_NETWORKS_COUNT; k++) {
            if (strcmp((const char *)ap_records[i].ssid, WIFI_NETWORKS[k].ssid) == 0 &&
                (best_known_index < 0 || ap_records[i].rssi > best_rssi)) {
                best_known_index = (int)k;
                best_rssi = ap_records[i].rssi;
            }
        }
    }

    bool found = (best_known_index >= 0);
    if (found) {
        strncpy(out_ssid, WIFI_NETWORKS[best_known_index].ssid, out_ssid_len - 1);
        out_ssid[out_ssid_len - 1] = '\0';
        strncpy(out_password, WIFI_NETWORKS[best_known_index].password, out_password_len - 1);
        out_password[out_password_len - 1] = '\0';
        ESP_LOGI(TAG, "wifi scan: selected known SSID \"%s\" (rssi %d, %u APs seen)",
                 out_ssid, best_rssi, (unsigned)ap_count);
    } else {
        ESP_LOGW(TAG, "wifi scan: none of %u visible APs matched a known network", (unsigned)ap_count);
    }

    heap_caps_free(ap_records);
    return found;
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

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // wifi_config.sta.ssid/password are 32/64 bytes; +1 for the NUL here.
    char selected_ssid[33] = {0};
    char selected_password[65] = {0};
    if (!wifi_scan_and_select(selected_ssid, sizeof(selected_ssid), selected_password, sizeof(selected_password))) {
        // Fall back to the first configured network, blind -- covers the
        // hidden-SSID case (scan results won't list it by name) at the
        // cost of one full connect-timeout if it's actually unreachable.
        // WIFI_NETWORKS_COUNT is required to be >= 1 (see secrets.h).
        strncpy(selected_ssid, WIFI_NETWORKS[0].ssid, sizeof(selected_ssid) - 1);
        strncpy(selected_password, WIFI_NETWORKS[0].password, sizeof(selected_password) - 1);
        ESP_LOGW(TAG, "wifi: falling back to first known network \"%s\" blind (unseen in scan / hidden SSID)", selected_ssid);
    }

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, selected_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, selected_password, sizeof(wifi_config.sta.password) - 1);
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

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_LOGI(TAG, "connecting to wifi SSID \"%s\"...", selected_ssid);
    esp_err_t connect_err = esp_wifi_connect();
    if (connect_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed immediately: %s", esp_err_to_name(connect_err));
        return false;
    }

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

// Bounded retry with backoff per spec section 5. F1 (PROJECT_PLAN.md):
// full real telemetry, including battery_mv (caller reads the ADC before
// wifi connects, since sync needs it in the request body up front).
// time_awake_ms is the *previous* cycle's measured awake duration (see
// s_last_cycle_time_awake_ms) -- it can't be this cycle's own duration,
// since that isn't known until after this very request completes.
static bool device_sync(const char *wake_reason, const char *reset_reason, int battery_mv, int64_t time_awake_ms, http_response_buf_t *resp, int *out_status) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "wake_reason", wake_reason);
    cJSON_AddStringToObject(root, "firmware_version", "0.2.0-f1");
    cJSON_AddNumberToObject(root, "battery_mv", battery_mv);
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

// Schedules the alarm at (current RTC time + interval_seconds), per spec
// section 4.1 ("now + poll_interval_seconds"). Pure seconds-of-day
// arithmetic: the PCF85063 alarm only matches sec/min/hour (day and
// weekday are always disabled by pcf85063a_set_alarm), so no need to
// round-trip through epoch time or worry about TZ/DST here -- just wrap
// at a day boundary (spec allows poll_interval_seconds up to 86400).
static void rtc_schedule_alarm(pcf85063a_dev_t *rtc, int interval_seconds) {
    pcf85063a_datetime_t cur = {};
    pcf85063a_get_time_date(rtc, &cur);

    long seconds_of_day = (long)cur.hour * 3600 + (long)cur.min * 60 + cur.sec;
    long alarm_seconds_of_day = (seconds_of_day + interval_seconds) % 86400;

    pcf85063a_datetime_t alarm_dt = {};
    alarm_dt.hour = (uint8_t)(alarm_seconds_of_day / 3600);
    alarm_dt.min = (uint8_t)((alarm_seconds_of_day % 3600) / 60);
    alarm_dt.sec = (uint8_t)(alarm_seconds_of_day % 60);

    pcf85063a_set_alarm(rtc, alarm_dt);
    pcf85063a_enable_alarm(rtc); // also clears any stale alarm flag
    ESP_LOGI(TAG, "RTC alarm scheduled at %02d:%02d:%02d UTC (+%ds)",
             alarm_dt.hour, alarm_dt.min, alarm_dt.sec, interval_seconds);
}

// ---------------------------------------------------------------------
// Small hand-authored 5x7 bitmap font -- digits, uppercase A-Z (text is
// upper-cased before drawing), and a bounded set of punctuation actually
// needed for snapshot content. No text-rendering library exists for this
// display (EPD_DrawColorPixel is pixel-only, per port_display.h), and
// this deliberately trades completeness (no lowercase, no full ASCII)
// for a small, hand-authored/verifiable glyph table.
// ---------------------------------------------------------------------

struct font_glyph_t {
    char ch;
    uint8_t rows[7];
};

// clang-format off
static const font_glyph_t FONT_5X7[] = {
    {' ', {0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
    {'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2', {0x0E,0x11,0x01,0x0E,0x10,0x10,0x1F}},
    {'3', {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
    {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5', {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6', {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9', {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {':', {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}},
    {'.', {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
    {',', {0x00,0x00,0x00,0x00,0x00,0x0C,0x08}},
    {'-', {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    {'/', {0x01,0x01,0x02,0x04,0x08,0x10,0x10}},
    {'%', {0x19,0x1A,0x04,0x04,0x04,0x0B,0x13}},
    {'?', {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}},
    {'!', {0x04,0x04,0x04,0x04,0x04,0x00,0x04}},
    {'\'', {0x0C,0x04,0x08,0x00,0x00,0x00,0x00}},
    {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'D', {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
    {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G', {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}},
    {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {'J', {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},
    {'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
    {'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'Q', {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
    {'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'W', {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}},
    {'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y', {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
    {'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
};
// clang-format on

#define FONT_GLYPH_W 5
#define FONT_GLYPH_H 7

static const uint8_t *font_lookup(char c) {
    char uc = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    for (size_t i = 0; i < sizeof(FONT_5X7) / sizeof(FONT_5X7[0]); i++) {
        if (FONT_5X7[i].ch == uc) {
            return FONT_5X7[i].rows;
        }
    }
    return FONT_5X7[0].rows; // unsupported char -> blank space
}

// Draws one glyph at (x0,y0) and returns the pixel width to advance by
// (including a 1-column gap), so callers can chain draw_char() calls.
static int draw_char(char c, int x0, int y0, int scale, uint8_t color) {
    const uint8_t *rows = font_lookup(c);
    for (int row = 0; row < FONT_GLYPH_H; row++) {
        uint8_t bits = rows[row];
        for (int col = 0; col < FONT_GLYPH_W; col++) {
            if (!((bits >> (4 - col)) & 0x1)) {
                continue;
            }
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    EPD_DrawColorPixel(x0 + col * scale + sx, y0 + row * scale + sy, color);
                }
            }
        }
    }
    return (FONT_GLYPH_W + 1) * scale;
}

// Draws a single line of text, returns the total pixel width drawn.
static int draw_text(const char *s, int x0, int y0, int scale, uint8_t color) {
    int x = x0;
    for (const char *p = s; *p; p++) {
        x += draw_char(*p, x, y0, scale, color);
    }
    return x - x0;
}

// Greedy word-wrap: draws `text` starting at (x0,y0), breaking at word
// boundaries to stay within max_width_px, up to max_lines lines: the
// last line gets "..." appended if text remains beyond it. Returns the
// y-coordinate just below the last line drawn, for stacking further
// content underneath.
static int draw_wrapped_text(const char *text, int x0, int y0, int max_width_px, int scale, uint8_t color, int max_lines) {
    int char_w = (FONT_GLYPH_W + 1) * scale;
    int line_h = (FONT_GLYPH_H + 2) * scale;
    int max_chars_per_line = max_width_px / char_w;
    if (max_chars_per_line < 1) {
        max_chars_per_line = 1;
    }

    size_t len = strlen(text);
    size_t pos = 0;
    int y = y0;

    for (int line = 0; pos < len && line < max_lines; line++) {
        size_t line_len = 0;
        size_t last_space = 0;
        bool have_space = false;
        while (pos + line_len < len && (int)line_len < max_chars_per_line) {
            if (text[pos + line_len] == ' ') {
                last_space = line_len;
                have_space = true;
            }
            line_len++;
        }
        bool truncated_mid_word = (pos + line_len < len) && text[pos + line_len] != ' ';
        if (truncated_mid_word && have_space) {
            line_len = last_space; // break at the last word boundary instead
        }

        bool more_text_remains = (pos + line_len < len);
        bool is_last_line = (line == max_lines - 1);

        char line_buf[64];
        size_t copy_len = line_len < sizeof(line_buf) - 4 ? line_len : sizeof(line_buf) - 4;
        memcpy(line_buf, text + pos, copy_len);
        line_buf[copy_len] = '\0';
        if (is_last_line && more_text_remains) {
            strcat(line_buf, "...");
        }

        draw_text(line_buf, x0, y, scale, color);
        y += line_h;
        pos += line_len;
        while (pos < len && text[pos] == ' ') {
            pos++; // skip the space we broke the line on
        }
    }
    return y;
}

// ---------------------------------------------------------------------
// F4 (PROJECT_PLAN.md): render the sync snapshot. A live check-in (spec
// section 4.3: "the one thing on the display that's actionable") takes
// over the whole screen; otherwise a compact dashboard (weather +
// soonest upcoming reminder/calendar event). Battery percentage and
// local time are always shown in the status bar. Each content section
// degrades independently per spec section 5 -- a missing/invalid field
// is just omitted, not flagged as an error on-screen.
// ---------------------------------------------------------------------

static void draw_status_bar(const sync_snapshot_t &snap, int battery_pct) {
    char battery_buf[8];
    snprintf(battery_buf, sizeof(battery_buf), "%d%%", battery_pct);
    int battery_w = (int)strlen(battery_buf) * (FONT_GLYPH_W + 1) * 2;
    draw_text(battery_buf, EPD_WIDTH - battery_w - 4, 4, 2, DRIVER_COLOR_BLACK);

    time_t now_epoch = time(nullptr);
    struct tm local_tm;
    localtime_r(&now_epoch, &local_tm);
    char time_buf[6];
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d", local_tm.tm_hour, local_tm.tm_min);
    draw_text(time_buf, 4, 4, 2, DRIVER_COLOR_BLACK);

    // Centered between time (left) and battery (right) -- always shown
    // here regardless of whether a check-in or the dashboard follows
    // below, since it's status-bar-level info either way.
    if (snap.has_weather) {
        char temp_buf[8];
        snprintf(temp_buf, sizeof(temp_buf), "%.0fC", snap.weather.temperature_c);
        int temp_w = (int)strlen(temp_buf) * (FONT_GLYPH_W + 1) * 2;
        draw_text(temp_buf, (EPD_WIDTH - temp_w) / 2, 4, 2, DRIVER_COLOR_BLACK);
    }

    for (int x = 4; x < EPD_WIDTH - 4; x++) {
        EPD_DrawColorPixel(x, 22, DRIVER_COLOR_BLACK);
    }
}

static const sync_checkin_t *find_live_checkin(const sync_snapshot_t &snap) {
    if (!snap.checkins_valid) {
        return nullptr;
    }
    for (int i = 0; i < snap.checkins_count; i++) {
        if (snap.checkins[i].has_fired_at) {
            return &snap.checkins[i];
        }
    }
    return nullptr;
}

static void draw_checkin(const sync_checkin_t &checkin) {
    draw_text("CHECK-IN", 8, 30, 2, DRIVER_COLOR_BLACK);
    // Body text at scale 1 (not 2) -- prompt_text can run to a full
    // sentence or two, and scale 2 didn't leave enough room to fit it
    // without truncating. Scale 1 fits ~30 chars/line x 15 lines (450
    // chars) well over the 256-char field cap, so truncation shouldn't
    // happen in practice anymore.
    draw_wrapped_text(checkin.prompt_text, 8, 50, EPD_WIDTH - 16, 1, DRIVER_COLOR_BLACK, 15);
}

static void format_local_hhmm(int64_t epoch, char *buf, size_t buf_len) {
    time_t t = (time_t)epoch;
    struct tm local_tm;
    localtime_r(&t, &local_tm);
    snprintf(buf, buf_len, "%02d:%02d", local_tm.tm_hour, local_tm.tm_min);
}

static void draw_dashboard(const sync_snapshot_t &snap) {
    int y = 30;

    if (snap.has_weather) {
        // Temperature is already in the status bar above -- just cloud
        // cover here, not repeating it.
        char buf[16];
        snprintf(buf, sizeof(buf), "CLOUD %d%%", snap.weather.cloud_cover_pct);
        draw_text(buf, 8, y, 1, DRIVER_COLOR_BLACK);
        y += 14;
    }

    // Soonest upcoming reminder or calendar event, whichever is sooner --
    // a single combined "next thing" rather than two separate lists,
    // since there's limited vertical space to spend on it.
    bool have_next = false;
    int64_t next_at = 0;
    char next_label[SYNC_STR_TEXT_LEN] = {0};
    bool next_is_event = false;

    if (snap.reminders_valid) {
        for (int i = 0; i < snap.reminders_count; i++) {
            if (!have_next || snap.reminders[i].due_at < next_at) {
                have_next = true;
                next_at = snap.reminders[i].due_at;
                strncpy(next_label, snap.reminders[i].message, sizeof(next_label) - 1);
                next_label[sizeof(next_label) - 1] = '\0';
                next_is_event = false;
            }
        }
    }
    if (snap.calendar_events_valid) {
        for (int i = 0; i < snap.calendar_events_count; i++) {
            if (!have_next || snap.calendar_events[i].start < next_at) {
                have_next = true;
                next_at = snap.calendar_events[i].start;
                strncpy(next_label, snap.calendar_events[i].summary, sizeof(next_label) - 1);
                next_label[sizeof(next_label) - 1] = '\0';
                next_is_event = true;
            }
        }
    }

    if (have_next) {
        char time_buf[6];
        format_local_hhmm(next_at, time_buf, sizeof(time_buf));
        char header[24];
        snprintf(header, sizeof(header), "%s %s:", next_is_event ? "EVENT" : "NEXT", time_buf);
        draw_text(header, 8, y, 1, DRIVER_COLOR_BLACK);
        y += 14;
        draw_wrapped_text(next_label, 8, y, EPD_WIDTH - 16, 2, DRIVER_COLOR_BLACK, 4);
    }
}

// PortDisplay_Init() unconditionally calls spi_bus_initialize() with
// ESP_ERROR_CHECK() right after (port_display.cpp) -- calling it twice
// in the same boot aborts with "SPI bus already initialized" (found via
// F6 hardware testing: the push-to-talk cycle draws a "RECORDING..."
// message, then later a "SENT"/"UPLOAD FAILED" one, and the second call
// crashed the device outright, showing up as reset_reason=panic on the
// next boot). Shared by every e-ink entry point below so PortDisplay_Init()/
// EPD_Init() only ever run once per boot, however many times the screen
// gets updated within that same cycle.
static bool s_eink_initialized_this_boot = false;

static void eink_ensure_initialized(void) {
    if (!s_eink_initialized_this_boot) {
        PortDisplay_Init();
        EPD_Init();
        s_eink_initialized_this_boot = true;
    }
}

static void eink_render(const sync_snapshot_t &snap, int battery_pct) {
    eink_ensure_initialized();
    EPD_Clear();

    draw_status_bar(snap, battery_pct);

    const sync_checkin_t *live_checkin = find_live_checkin(snap);
    if (live_checkin) {
        draw_checkin(*live_checkin);
    } else {
        draw_dashboard(snap);
    }

    EPD_Display();
    ESP_LOGI(TAG, "e-ink render done (%s)", live_checkin ? "check-in" : "dashboard");
}

// F6 (PROJECT_PLAN.md): simple full-refresh status message, reusing the
// same font/wrap helpers as the snapshot render above -- used by the
// push-to-talk cycle ("RECORDING...", "SENT", "UPLOAD FAILED"), which
// doesn't have a fresh snapshot to render (it deliberately skips wifi
// until after recording finishes).
static void eink_show_message(const char *message) {
    eink_ensure_initialized();
    EPD_Clear();
    draw_wrapped_text(message, 8, 80, EPD_WIDTH - 16, 2, DRIVER_COLOR_BLACK, 4);
    EPD_Display();
    ESP_LOGI(TAG, "e-ink message shown: %s", message);
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
// SD card: mount, write, read back, verify. Also backs debug logging
// below -- ensure_sdcard_mounted() is shared so the two don't fight over
// double-mounting the same card.
// ---------------------------------------------------------------------

static bool s_sdcard_mounting_in_progress = false;
static bool s_sdcard_mounted = false;

// Retries on every call until it actually succeeds -- needed because the
// SD card can't be mounted until BoardPower_VBAT_ON() has powered its
// rail, but sd_log_vprintf() below (and its early install point in
// app_main, before power-on) means log calls can arrive before that.
// s_sdcard_mounting_in_progress guards against reentrancy instead: if
// Sdcard_Init() itself logs (e.g. a mount failure, via
// ESP_ERROR_CHECK_WITHOUT_ABORT) and that line re-enters here through
// sd_log_vprintf(), the reentrant call sees a mount already underway and
// just reports "not mounted yet" instead of recursing into another
// attempt.
static bool ensure_sdcard_mounted(void) {
    if (s_sdcard_mounted) {
        return true;
    }
    if (s_sdcard_mounting_in_progress) {
        return false;
    }
    s_sdcard_mounting_in_progress = true;
    s_sdcard_mounted = Sdcard_Init();
    s_sdcard_mounting_in_progress = false;
    return s_sdcard_mounted;
}

// Debugging aid: this board's native USB-Serial/JTAG only stays up while
// awake, and reconnecting to it mid-test can itself trigger a reset (see
// PROJECT_PLAN.md's flashing notes) -- so catching a log for a cycle
// that runs and sleeps again quickly is unreliable over serial alone.
// Mirrors every ESP_LOG* line to /sdcard/debug.log (append mode) in
// addition to the normal console output, installed once at the very top
// of app_main so even the earliest boot lines are captured. Pull the SD
// card to read it directly, no serial connection needed. Not
// log-rotated -- fine for a debugging session, would need capping for
// indefinite/production use.
static int sd_log_vprintf(const char *fmt, va_list args) {
    int ret = vprintf(fmt, args);

    if (ensure_sdcard_mounted()) {
        FILE *f = fopen(SDlist "/debug.log", "a");
        if (f) {
            va_list args_copy;
            va_copy(args_copy, args);
            vfprintf(f, fmt, args_copy);
            va_end(args_copy);
            fclose(f);
        }
    }
    return ret;
}

static void sdcard_test(void) {
    if (!ensure_sdcard_mounted()) {
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

    BoardPower_EPD_OFF();
    BoardPower_Audio_OFF();
    ESP_ERROR_CHECK(rtc_gpio_hold_en((gpio_num_t)VBAT_PWR_PIN));

    esp_deep_sleep_start();
}

// ---------------------------------------------------------------------
// F6 (PROJECT_PLAN.md): push-to-talk. BOOT wakes the device, recording
// starts immediately (no wifi yet -- that only happens after recording
// finishes, right before upload), stops on silence or a second BOOT
// press or a hard duration cap, then uploads and goes straight back to
// sleep. Deliberately skips the entire normal sync cycle (wifi-connect
// beforehand, /device/sync, snapshot render, audio_loopback_test,
// sdcard_test, RTC-alarm rescheduling) -- spec section 4.2 says a sync
// afterward is optional, not required, and keeping this path lean keeps
// the whole interaction fast.
// ---------------------------------------------------------------------

#define PTT_SAMPLE_RATE_HZ 16000
#define PTT_CHANNELS 2 // mic hardware is wired stereo (fixed I2S TDM config
                        // in codec_board); downmixed to mono at upload time.
#define PTT_BYTES_PER_SAMPLE 2
#define PTT_CHUNK_MS 200
#define PTT_MAX_RECORD_MS 60000  // hard safety cap regardless of the two below
#define PTT_SILENCE_WARMUP_MS 1500 // grace period before silence counts, so
                                    // the user has a moment to start talking
#define PTT_SILENCE_STOP_MS 2000   // auto-stop after this much continuous quiet
// Starting guess, not calibrated against the real mic/room -- likely needs a
// real-world tuning pass once tested on hardware (see PROJECT_PLAN.md's F6
// notes). Chunk-level RMS is logged at ESP_LOGD to make that tuning possible
// without needing to re-instrument anything.
#define PTT_SILENCE_RMS_THRESHOLD 600

// ---------------------------------------------------------------------
// WAV header: minimal 44-byte RIFF/WAVE header for 16-bit PCM. No
// existing WAV-writing code anywhere in this repo or the vendored
// Waveshare reference tree -- small, fixed-layout, well-documented
// format, hand-built here rather than pulled in as a dependency.
// ---------------------------------------------------------------------

#pragma pack(push, 1)
struct wav_header_t {
    char riff_tag[4];      // "RIFF"
    uint32_t riff_length;  // total file length - 8
    char wave_tag[4];      // "WAVE"
    char fmt_tag[4];       // "fmt "
    uint32_t fmt_length;   // 16 for PCM
    uint16_t audio_format; // 1 = PCM
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate; // sample_rate * num_channels * bits_per_sample/8
    uint16_t block_align; // num_channels * bits_per_sample/8
    uint16_t bits_per_sample;
    char data_tag[4]; // "data"
    uint32_t data_length; // number of PCM bytes that follow this header
};
#pragma pack(pop)

static wav_header_t build_wav_header(uint32_t sample_rate, uint16_t num_channels, uint16_t bits_per_sample, uint32_t data_length) {
    wav_header_t hdr = {};
    memcpy(hdr.riff_tag, "RIFF", 4);
    hdr.riff_length = data_length + (uint32_t)sizeof(wav_header_t) - 8;
    memcpy(hdr.wave_tag, "WAVE", 4);
    memcpy(hdr.fmt_tag, "fmt ", 4);
    hdr.fmt_length = 16;
    hdr.audio_format = 1;
    hdr.num_channels = num_channels;
    hdr.sample_rate = sample_rate;
    hdr.bits_per_sample = bits_per_sample;
    hdr.block_align = (uint16_t)(num_channels * (bits_per_sample / 8));
    hdr.byte_rate = sample_rate * hdr.block_align;
    memcpy(hdr.data_tag, "data", 4);
    hdr.data_length = data_length;
    return hdr;
}

// Records into buf (capacity max_bytes, stereo/16-bit PCM) in ~200ms
// chunks, stopping on whichever of three independent conditions fires
// first: a second BOOT press, ~2s of continuous silence (after an
// initial warm-up grace period), or the hard 60s cap. Returns the number
// of bytes actually recorded.
static size_t record_until_stopped(uint8_t *buf, size_t max_bytes) {
    const size_t chunk_bytes = (size_t)PTT_SAMPLE_RATE_HZ * PTT_CHANNELS * PTT_BYTES_PER_SAMPLE * PTT_CHUNK_MS / 1000;

    // Guard against misreading the tail of the original wake-press as an
    // immediate stop signal: don't construct the button object (which
    // reconfigures the pin and starts its own debounce state machine)
    // until the pin genuinely reads released. A normal tap has almost
    // always already released by this point (boot takes 300-700ms), so
    // this loop typically exits on its very first check.
    int waited_ms = 0;
    while (gpio_get_level(BOOT_BUTTON_PIN) == 0 && waited_ms < 500) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited_ms += 20;
    }

    // BOOT_BUTTON_PIN was configured as an EXT1 (RTC-domain) wake source
    // by the *previous* cycle's enter_deep_sleep() call -- this is the
    // first feature that ever needs it back as a normal digital-matrix
    // GPIO with real interrupts while awake (every previous use was only
    // ever a one-shot RTC-domain level read at wake). Release it from
    // RTC-GPIO mode before iot_button configures it, or its interrupt
    // registration silently doesn't take effect (found via hardware
    // testing: OnClick never fired at all without this).
    rtc_gpio_deinit(BOOT_BUTTON_PIN);

    volatile bool stop_requested = false;
    Button stop_button(BOOT_BUTTON_PIN, /*active_high=*/false);
    stop_button.OnClick([&stop_requested]() {
        ESP_LOGI(TAG, "ptt stop button: click detected");
        stop_requested = true;
    });

    size_t recorded_bytes = 0;
    int elapsed_ms = 0;
    int silence_ms = 0;

    while (recorded_bytes + chunk_bytes <= max_bytes) {
        Codec_RecordData(buf + recorded_bytes, chunk_bytes);
        recorded_bytes += chunk_bytes;
        elapsed_ms += PTT_CHUNK_MS;

        const int16_t *samples = reinterpret_cast<const int16_t *>(buf + recorded_bytes - chunk_bytes);
        size_t sample_count = chunk_bytes / sizeof(int16_t);
        int64_t sum_sq = 0;
        for (size_t i = 0; i < sample_count; i++) {
            sum_sq += (int64_t)samples[i] * samples[i];
        }
        float rms = sqrtf((float)sum_sq / (float)sample_count);
        ESP_LOGD(TAG, "ptt chunk rms=%.1f elapsed_ms=%d silence_ms=%d", (double)rms, elapsed_ms, silence_ms);

        if (elapsed_ms > PTT_SILENCE_WARMUP_MS) {
            silence_ms = (rms < PTT_SILENCE_RMS_THRESHOLD) ? (silence_ms + PTT_CHUNK_MS) : 0;
        }

        if (stop_requested) {
            ESP_LOGI(TAG, "ptt recording stopped: button press");
            break;
        }
        if (silence_ms >= PTT_SILENCE_STOP_MS) {
            ESP_LOGI(TAG, "ptt recording stopped: silence detected");
            break;
        }
        if (elapsed_ms >= PTT_MAX_RECORD_MS) {
            ESP_LOGW(TAG, "ptt recording stopped: hit max duration cap");
            break;
        }
    }

    return recorded_bytes;
}

// POST /voice multipart. field name must be exactly "file" (matches
// adhi-backend/voice.py's `file: UploadFile = File(...)`); never sends
// one_shot/sync (spec section 3.2 / voice.py's own docstring: testing-only,
// real hardware must never set them). No built-in multipart helper in
// esp_http_client -- boundary/headers are hand-built and streamed via
// open()/write()/fetch_headers() rather than the single-buffer
// set_post_field() pattern device_sync_attempt() uses, since the body
// here (WAV header + audio) is too large to comfortably build as one
// contiguous string first. The stereo capture is downmixed to real mono
// (average L+R per sample) while streaming, rather than requiring a
// second full-size buffer -- halves the upload for no quality loss and
// matches the spec's stated recommendation. No retry loop: unlike
// /device/sync, voice failures are already designed to be silent/
// best-effort backend-side (spec's fire-and-forget framing), so one
// attempt is enough.
static bool upload_voice_note(const uint8_t *stereo_pcm, size_t stereo_bytes) {
    const char *boundary = "----adhiVoiceBoundary7f3a";

    char preamble[192];
    int preamble_len = snprintf(preamble, sizeof(preamble),
                                 "--%s\r\n"
                                 "Content-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n"
                                 "Content-Type: audio/wav\r\n"
                                 "\r\n",
                                 boundary);

    char epilogue[64];
    int epilogue_len = snprintf(epilogue, sizeof(epilogue), "\r\n--%s--\r\n", boundary);

    size_t mono_bytes = stereo_bytes / 2; // same bit depth, half the channels
    wav_header_t wav_hdr = build_wav_header(PTT_SAMPLE_RATE_HZ, 1, 16, (uint32_t)mono_bytes);

    size_t content_length = (size_t)preamble_len + sizeof(wav_hdr) + mono_bytes + (size_t)epilogue_len;

    char content_type_header[64];
    snprintf(content_type_header, sizeof(content_type_header), "multipart/form-data; boundary=%s", boundary);

    char url[256];
    snprintf(url, sizeof(url), "%s/voice", BACKEND_BASE_URL);

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    // 15s (the original guess) measured genuinely too short on real
    // hardware: a 384KB mono upload took ~15.9s end to end (observed
    // effective throughput ~25KB/s over wifi), so the client gave up
    // waiting for the response right as the backend was finishing --
    // the file had actually fully arrived and was processed correctly,
    // this was purely a client-side false-negative timeout. 60s covers
    // the worst case (the full 60s/1.92MB-mono recording cap) with
    // headroom; may still need tuning once tested against that case.
    config.timeout_ms = 60000;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "auth", API_TOKEN);
    esp_http_client_set_header(client, "Content-Type", content_type_header);

    esp_err_t err = esp_http_client_open(client, (int)content_length);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "/voice: failed to open connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    bool write_ok = true;
    write_ok = write_ok && esp_http_client_write(client, preamble, preamble_len) >= 0;
    write_ok = write_ok && esp_http_client_write(client, reinterpret_cast<const char *>(&wav_hdr), sizeof(wav_hdr)) >= 0;

    // Stream the stereo->mono downmix in chunks rather than allocating a
    // second full-size buffer for it. 4096 frames (8KB/write) cuts
    // per-call overhead vs. a smaller chunk -- but that buffer must be
    // heap/PSRAM-allocated, not a stack local: the main task stack is
    // only 8192 bytes total (see the sync_snapshot_t comment elsewhere
    // in this file), and an 8KB stack array alone nearly fills it,
    // which crashed outright on real hardware (silent panic, no error
    // logged first -- a stack overflow, unlike the SPI-reinit crash
    // found earlier, which did log before aborting).
    const int16_t *stereo_samples = reinterpret_cast<const int16_t *>(stereo_pcm);
    size_t stereo_frame_count = stereo_bytes / (PTT_CHANNELS * sizeof(int16_t));
    const size_t DOWNMIX_CHUNK_FRAMES = 4096;
    auto *mono_chunk = static_cast<int16_t *>(heap_caps_malloc(DOWNMIX_CHUNK_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (!mono_chunk) {
        ESP_LOGE(TAG, "/voice: failed to allocate downmix chunk buffer");
        esp_http_client_cleanup(client);
        return false;
    }

    for (size_t frame = 0; write_ok && frame < stereo_frame_count; frame += DOWNMIX_CHUNK_FRAMES) {
        size_t this_chunk_frames = (frame + DOWNMIX_CHUNK_FRAMES <= stereo_frame_count) ? DOWNMIX_CHUNK_FRAMES : (stereo_frame_count - frame);
        for (size_t i = 0; i < this_chunk_frames; i++) {
            int16_t l = stereo_samples[(frame + i) * 2];
            int16_t r = stereo_samples[(frame + i) * 2 + 1];
            mono_chunk[i] = (int16_t)(((int32_t)l + (int32_t)r) / 2);
        }
        write_ok = write_ok && esp_http_client_write(client, reinterpret_cast<const char *>(mono_chunk), (int)(this_chunk_frames * sizeof(int16_t))) >= 0;
    }
    heap_caps_free(mono_chunk);

    write_ok = write_ok && esp_http_client_write(client, epilogue, epilogue_len) >= 0;

    if (!write_ok) {
        ESP_LOGE(TAG, "/voice: write failed mid-upload");
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    bool ok = (status >= 200 && status < 300);
    ESP_LOGI(TAG, "/voice upload %s (status %d, %u bytes sent)", ok ? "OK" : "FAILED", status, (unsigned)content_length);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

static void run_push_to_talk_cycle(void) {
    ESP_LOGI(TAG, "push-to-talk: BOOT wake, starting voice cycle");

    BoardPower_Init();
    BoardPower_VBAT_ON();
    BoardPower_EPD_ON();
    BoardPower_Audio_ON();
    vTaskDelay(pdMS_TO_TICKS(100));

    // Codec_StartInit() expects the shared I2C bus already up (the
    // normal cycle gets this for free via its own RTC/SHTC3 init before
    // ever touching audio; found missing here via hardware testing --
    // codec_board logged a harmless-but-confusing "port has not been
    // initialized" error before self-recovering).
    I2cMasterBus::requestInstance(ESP32_I2C_SCL_PIN, ESP32_I2C_SDA_PIN, ESP32_I2C_DEV_NUM);
    Codec_StartInit();
    eink_show_message("RECORDING... PRESS BOOT TO STOP");

    const size_t max_bytes = (size_t)PTT_SAMPLE_RATE_HZ * PTT_CHANNELS * PTT_BYTES_PER_SAMPLE * PTT_MAX_RECORD_MS / 1000;
    auto *buf = static_cast<uint8_t *>(heap_caps_malloc(max_bytes, MALLOC_CAP_SPIRAM));
    if (!buf) {
        ESP_LOGE(TAG, "push-to-talk: failed to allocate %u byte recording buffer", (unsigned)max_bytes);
        eink_show_message("RECORDING FAILED");
    } else {
        size_t recorded_bytes = record_until_stopped(buf, max_bytes);
        double recorded_seconds = (double)recorded_bytes / (PTT_SAMPLE_RATE_HZ * PTT_CHANNELS * PTT_BYTES_PER_SAMPLE);
        ESP_LOGI(TAG, "push-to-talk: recorded %u bytes (%.1fs)", (unsigned)recorded_bytes, recorded_seconds);

        // Immediate feedback that the stop (silence or button) actually
        // registered -- found via hardware testing that wifi connect +
        // upload can take 20s+, and staying on "RECORDING..." that whole
        // time looks exactly like the stop didn't work.
        eink_show_message("SENDING...");

        bool wifi_ok = wifi_connect();
        bool upload_ok = false;
        if (wifi_ok) {
            upload_ok = upload_voice_note(buf, recorded_bytes);
        } else {
            ESP_LOGE(TAG, "push-to-talk: skipping upload -- wifi never connected");
        }
        eink_show_message(upload_ok ? "SENT" : "UPLOAD FAILED");
        heap_caps_free(buf);
    }

    // The regular sync cadence is untouched here -- no
    // rtc_schedule_alarm() call, the alarm from the last real sync cycle
    // is left exactly as it was. enter_deep_sleep() already powers down
    // audio/EPD itself.
    enter_deep_sleep();
}

// ---------------------------------------------------------------------

extern "C" void app_main(void) {
    s_boot_time_us = esp_timer_get_time();

    // Installed as early as possible for maximum coverage. Actual
    // mounting is lazy (first log call after BoardPower_VBAT_ON() powers
    // the SD rail) and retries on every call until it succeeds -- see
    // ensure_sdcard_mounted()'s comment.
    esp_log_set_vprintf(sd_log_vprintf);

    // NVS init first, per standard ESP-IDF convention -- required before
    // wifi, and independent of every other subsystem below.
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    // VBAT hold survives deep sleep; release before we can drive that
    // GPIO again this cycle (the EPD is fully power-cycled every wake now,
    // so its own rail needs no equivalent hold/release pair).
    rtc_gpio_hold_dis((gpio_num_t)VBAT_PWR_PIN);

    const char *reset_reason = reset_reason_to_string(esp_reset_reason());
    const char *wake_reason = wake_reason_to_string();
    ESP_LOGI(TAG, "boot: reset_reason=%s wake_reason=%s", reset_reason, wake_reason);

    // F6: BOOT is dedicated to push-to-talk and takes a completely
    // separate path -- checked before any of the normal cycle's board
    // power-on/wifi/sync setup below, since run_push_to_talk_cycle()
    // does its own power-on and never returns (ends in its own
    // enter_deep_sleep() call).
    if (wake_was_boot_button()) {
        run_push_to_talk_cycle();
    }

    BoardPower_Init();
    BoardPower_VBAT_ON();
    BoardPower_EPD_ON();
    vTaskDelay(pdMS_TO_TICKS(100));

    // TZ env state lives in normal RAM and does not survive deep sleep;
    // re-apply the last-known value every wake so localtime()/log
    // timestamps stay correct even on a cycle whose sync attempt fails.
    sync_tz_apply(s_tz_posix, "UTC0");

    // I2C bus is shared by the RTC and the SHTC3 sensor (the audio codec
    // manages its own separate I2C init internally).
    I2cMasterBus *i2c_bus = I2cMasterBus::requestInstance(ESP32_I2C_SCL_PIN, ESP32_I2C_SDA_PIN, ESP32_I2C_DEV_NUM);
    pcf85063a_dev_t rtc_dev;
    ESP_ERROR_CHECK(pcf85063a_init(&rtc_dev, i2c_bus->Get_I2cBusHandle(), PCF85063A_ADDRESS));
    Shtc3_Init(i2c_bus);

    // Read before wifi/sync, not after -- F1 (PROJECT_PLAN.md) wants
    // battery_mv in the sync request body itself, not just logged
    // afterward.
    BoardAdc_Init();
    float vbat = Get_VbatVoltage();
    int battery_mv = (int)(vbat * 1000.0f + 0.5f);
    uint8_t battery_pct = Get_Batterylevel();
    ESP_LOGI(TAG, "battery: %.3fV (%.0f%%)", vbat, (double)battery_pct);

    bool wifi_ok = wifi_connect();

    // sync_snapshot_t is ~8.4KB -- far too large for the 3.5KB main task
    // stack (CONFIG_ESP_MAIN_TASK_STACK_SIZE), so it must live in
    // heap/PSRAM, not as a stack local. Kept alive (not freed) until
    // after rendering below -- F4 (PROJECT_PLAN.md) needs the parsed
    // snapshot to draw real content, not just to extract TZ/RTC/poll
    // interval as before.
    sync_snapshot_t *snap = nullptr;
    bool have_fresh_snapshot = false;

    if (wifi_ok) {
        const size_t resp_capacity = 8192;
        http_response_buf_t resp;
        resp.data = static_cast<char *>(heap_caps_malloc(resp_capacity, MALLOC_CAP_SPIRAM));
        resp.capacity = resp_capacity;
        resp.written = 0;

        int http_status = 0;
        ESP_LOGI(TAG, "reporting time_awake_ms=%lld (measured last cycle)", (long long)s_last_cycle_time_awake_ms);
        bool sync_ok = device_sync(wake_reason, reset_reason, battery_mv, s_last_cycle_time_awake_ms, &resp, &http_status);

        if (sync_ok) {
            ESP_LOGI(TAG, "/device/sync OK (status %d)", http_status);
            snap = static_cast<sync_snapshot_t *>(heap_caps_malloc(sizeof(sync_snapshot_t), MALLOC_CAP_SPIRAM));
            if (sync_snapshot_parse(resp.data, snap)) {
                have_fresh_snapshot = true;
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
                if (snap->has_poll_interval_seconds) {
                    s_last_known_good_poll_interval = snap->poll_interval_seconds;
                    s_has_synced_ever = true;
                }
            } else {
                ESP_LOGE(TAG, "/device/sync response failed to parse as JSON at all");
            }
        } else {
            ESP_LOGE(TAG, "/device/sync failed after retries (last status %d)", http_status);
        }

        heap_caps_free(resp.data);
    } else {
        ESP_LOGE(TAG, "skipping /device/sync -- wifi never connected");
    }
    // s_last_known_good_poll_interval/s_has_synced_ever are deliberately
    // left untouched on any failure above -- spec section 5 wants the
    // *last successful* sync's poll_interval_seconds used as the sleep
    // duration when this cycle fails, not the hardcoded fallback, as
    // long as one has ever succeeded.

    // F5: this cycle's actual wake-to-sync-response duration, measured
    // now that the sync attempt (success, retries-exhausted, or wifi
    // never connecting at all) has concluded -- stored for *next*
    // cycle's sync call to report, per the comment on
    // s_last_cycle_time_awake_ms's declaration above.
    s_last_cycle_time_awake_ms = (esp_timer_get_time() - s_boot_time_us) / 1000;
    ESP_LOGI(TAG, "this cycle's wake-to-sync-response time: %lldms", (long long)s_last_cycle_time_awake_ms);

    float temp_c = 0, humidity_pct = 0;
    Shtc3_ReadTempHumi(&temp_c, &humidity_pct);
    ESP_LOGI(TAG, "SHTC3: temp=%.1fC humidity=%.1f%%", temp_c, humidity_pct);

    // Only redraw the e-ink display when there's fresh data to show it --
    // e-ink is bistable (holds its image with no power), so skipping the
    // refresh on a failed cycle naturally leaves the last successful
    // content on screen instead of blanking it. Matches CLAUDE.md: "a
    // sync failure or stale display is a degraded UX, never a missed
    // reminder" -- a stale screen beats an empty one.
    if (have_fresh_snapshot) {
        eink_render(*snap, battery_pct);
    } else {
        ESP_LOGW(TAG, "skipping e-ink refresh -- no fresh snapshot this cycle, display stays as-is");
    }
    if (snap) {
        heap_caps_free(snap);
    }

    audio_loopback_test();
    sdcard_test();

    int effective_poll_interval = sync_effective_poll_interval(
        s_has_synced_ever, s_last_known_good_poll_interval, FALLBACK_POLL_INTERVAL_SECONDS);
    rtc_schedule_alarm(&rtc_dev, effective_poll_interval);
    enter_deep_sleep();
}
