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
#include <sys/stat.h>
#include <dirent.h>
#include <strings.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_log.h>
#include <esp_system.h>
#include <esp_random.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_attr.h>
#include <nvs_flash.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
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
#include "eink_ui.h"

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

// Verbose SD-card mirroring of every ESP_LOG* line to /sdcard/debug.log
// (see sd_log_vprintf() below). Built as a stopgap for F6/F7 development,
// when USB-Serial drops on sleep and reconnecting can itself trigger a
// reset. Costs an SD mount/write per log line, so it's worth being able to
// turn off for battery-sensitive runs rather than leaving it on forever --
// flip to 0 to fall back to plain console logging only. reset_history.log
// (one line per cycle, negligible cost) is unaffected by this flag.
#define SD_DEBUG_LOG_ENABLED 1

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
// ~3-4s per auth attempt observed against this AP; 10 covers the 30s
// connect timeout below instead of exhausting around 15-20s.
#define WIFI_MAX_RETRY 10
#define FALLBACK_POLL_INTERVAL_SECONDS 300

// CLAUDE.md's "Firmware versioning" section: bump at least once before
// every commit that changes main/ or components/ source, in the same
// commit as the change itself -- this is the single point of truth for
// both /device/sync and /device/error, so bumping here covers both.
#define FIRMWARE_VERSION "0.4.5-eink-same-day-filter"

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

// Bug fix: PWR is very likely a momentary self-latching power switch,
// not a plain wake input (see the power-latch comment in app_main). The
// original Waveshare reference example treats any PWR-triggered wake as
// "the user wants to power off." Our own PWR still means "sync now" on
// a tap (unchanged, relied on throughout this whole project's testing)
// -- only a *held* PWR press means power off, checked via
// wait_for_tap_or_hold() below.
static bool wake_was_pwr_button(void) {
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) {
        return false;
    }
    uint64_t status = esp_sleep_get_ext1_wakeup_status();
    return (status & (1ULL << PWR_BUTTON_PIN)) != 0;
}

// Instant (no e-ink refresh delay) visible confirmation that the device
// is executing code -- added alongside the power-latch timing fix,
// replacing an earlier e-ink splash message that took ~2s (the panel's
// own fixed full-refresh time) and would have corrupted
// wait_for_tap_or_hold()'s timing if shown before it. Reconfigures the
// pin fresh every call since deep sleep doesn't preserve normal GPIO
// direction config.
static void led_set(bool on) {
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    // Active-low -- found via hardware testing: the original active-high
    // assumption had this backwards, so the LED was off for the whole
    // awake cycle and only flashed briefly right as enter_deep_sleep()'s
    // led_set(false) call (driving it "off" = high, actually turning it
    // on) ran just before esp_deep_sleep_start() cut the GPIO drive.
    gpio_set_level(LED_PIN, on ? 0 : 1);
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
// POST /device/error -- best-effort error reporting. Recording an error
// (record_device_error()) never touches the network, so it's safe to
// call from anywhere, including code paths that run with no wifi at all
// (chime playback during F9's reminder-only wake, or an SD mount
// failure before wifi is even attempted this boot). Actually sending it
// (flush_pending_device_error()) happens once per cycle, only after
// wifi is confirmed up.
// ---------------------------------------------------------------------

// Single-slot pending report, persisted across deep sleep so it survives
// however many network-free wakes happen before the next one with wifi.
// Only the most recent error is kept if several occur first -- same
// "single slot, not a queue" tradeoff already accepted for
// s_last_announced_reminder_id (F9) elsewhere in this file.
#define DEVICE_ERROR_TYPE_LEN 32
#define DEVICE_ERROR_MESSAGE_LEN 128
static RTC_DATA_ATTR bool s_pending_error = false;
static RTC_DATA_ATTR char s_pending_error_type[DEVICE_ERROR_TYPE_LEN] = "";
static RTC_DATA_ATTR char s_pending_error_message[DEVICE_ERROR_MESSAGE_LEN] = "";

// Shared shape for the three simple POST call sites (device_sync's
// per-attempt request, /device/error, checkin skip): build a JSON-or-empty
// POST, run it, report whether the status was 2xx. Deliberately not used
// by the /voice upload -- that's a structurally different streaming
// multipart upload (esp_http_client_open/write/fetch_headers), not a
// single set_post_field() call, so folding it in here would just be a
// forced-fit abstraction. `event_handler`/`user_data` are optional (used
// only by device_sync_attempt, to stream the response into its own
// buffer); `body` is optional (skip_checkin has none).
static bool http_post(const char *url, const char *body,
                       http_event_handle_cb event_handler, void *user_data,
                       int *out_status) {
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 10000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    if (event_handler) {
        config.event_handler = event_handler;
        config.user_data = user_data;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "auth", API_TOKEN);
    if (body) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    bool ok = false;
    int status = -1;
    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(client);
        ok = (status >= 200 && status < 300);
    } else {
        ESP_LOGE(TAG, "POST %s failed: %s", url, esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    if (out_status) {
        *out_status = status;
    }
    return ok;
}

static void record_device_error(const char *error_type, const char *message) {
    s_pending_error = true;
    strncpy(s_pending_error_type, error_type, sizeof(s_pending_error_type) - 1);
    s_pending_error_type[sizeof(s_pending_error_type) - 1] = '\0';
    if (message) {
        strncpy(s_pending_error_message, message, sizeof(s_pending_error_message) - 1);
        s_pending_error_message[sizeof(s_pending_error_message) - 1] = '\0';
    } else {
        s_pending_error_message[0] = '\0';
    }
    ESP_LOGW(TAG, "device error recorded (pending report): %s: %s", error_type, s_pending_error_message);
}

// One best-effort attempt -- unlike device_sync(), no bounded-retry loop
// within the cycle; a failure here just leaves s_pending_error set for
// flush_pending_device_error()'s caller to try again next time wifi is up.
static bool send_device_error(const char *error_type, const char *message, const char *reset_reason,
                               const char *wake_reason, int battery_mv, int battery_pct) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error_type", error_type);
    cJSON_AddStringToObject(root, "message", message ? message : "");
    cJSON_AddStringToObject(root, "firmware_version", FIRMWARE_VERSION);
    cJSON_AddStringToObject(root, "reset_reason", reset_reason);
    cJSON_AddStringToObject(root, "wake_reason", wake_reason);
    cJSON_AddNumberToObject(root, "battery_mv", battery_mv);
    cJSON_AddNumberToObject(root, "battery_pct", battery_pct);
    cJSON_AddNumberToObject(root, "rssi_dbm", get_rssi_dbm());
    cJSON_AddNumberToObject(root, "free_internal_heap_bytes", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    char *body = cJSON_PrintUnformatted(root);

    char url[256];
    snprintf(url, sizeof(url), "%s/device/error", BACKEND_BASE_URL);

    int status = -1;
    bool ok = http_post(url, body, nullptr, nullptr, &status);
    if (!ok && status >= 0) {
        ESP_LOGW(TAG, "/device/error POST returned status %d", status);
    }

    cJSON_free(body);
    cJSON_Delete(root);
    return ok;
}

// Called once per cycle after wifi is confirmed up. Sends whatever error
// is pending (possibly recorded on an earlier network-free wake) and
// only clears it on success -- a failed report just retries next time.
static void flush_pending_device_error(const char *reset_reason, const char *wake_reason, int battery_mv, int battery_pct) {
    if (!s_pending_error) {
        return;
    }
    bool ok = send_device_error(s_pending_error_type, s_pending_error_message, reset_reason, wake_reason, battery_mv, battery_pct);
    if (ok) {
        s_pending_error = false;
    }
}

// ---------------------------------------------------------------------
// POST /device/sync
// ---------------------------------------------------------------------

struct http_response_buf_t {
    char *data;
    size_t capacity;
    size_t written;
    bool truncated; // set if a chunk had to be clamped -- response exceeded capacity
};

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        auto *buf = static_cast<http_response_buf_t *>(evt->user_data);
        if (buf && buf->data && buf->written + 1 < buf->capacity) {
            size_t copy_len = evt->data_len;
            if (copy_len > buf->capacity - 1 - buf->written) {
                copy_len = buf->capacity - 1 - buf->written;
                buf->truncated = true;
            }
            memcpy(buf->data + buf->written, evt->data, copy_len);
            buf->written += copy_len;
            buf->data[buf->written] = '\0';
        } else if (buf && evt->data_len > 0) {
            // Buffer was already full when this chunk arrived -- still
            // truncated, just nothing left to clamp/copy.
            buf->truncated = true;
        }
    }
    return ESP_OK;
}

// One attempt at POST /device/sync. Returns true on a 2xx response that
// wasn't truncated -- a truncated response is treated as a failed
// attempt regardless of HTTP status, since handing partial JSON to
// sync_snapshot_parse() would silently degrade every section, not just
// the one that happened to push the response over capacity.
static bool device_sync_attempt(const char *request_body, http_response_buf_t *resp, int *out_status) {
    resp->written = 0;
    resp->truncated = false;
    resp->data[0] = '\0';

    char url[256];
    snprintf(url, sizeof(url), "%s/device/sync", BACKEND_BASE_URL);

    bool ok = http_post(url, request_body, http_event_handler, resp, out_status);
    if (ok && resp->truncated) {
        ESP_LOGE(TAG, "/device/sync response truncated at %u bytes (capacity %u) -- treating as failed attempt",
                 (unsigned)resp->written, (unsigned)resp->capacity);
        record_device_error("sync_response_truncated", "response exceeded resp_capacity");
        ok = false;
    }
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
    cJSON_AddStringToObject(root, "firmware_version", FIRMWARE_VERSION);
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

// F9: reminder wake + chime state, all persisted across deep-sleep cycles.
// A compact {id, message, due_at} cache (not the full sync_reminder_t,
// which also carries event_uid[40] -- unneeded here) refreshed from every
// successful sync, so a reminder-only wake (see
// handle_reminder_only_wake() below) can decide what to do -- including
// what to draw if it needs to override a live check-in on screen --
// without a fresh network fetch.
typedef struct {
    char id[SYNC_STR_ID_LEN];
    char message[SYNC_STR_TEXT_LEN];
    int64_t due_at;
} reminder_wake_entry_t;
static RTC_DATA_ATTR reminder_wake_entry_t s_pending_reminders[SYNC_MAX_REMINDERS];
static RTC_DATA_ATTR int s_pending_reminders_count = 0;

// When the next real sync is owed (epoch seconds); 0 until the first
// successful sync. Fixed at the end of every full-sync cycle and left
// untouched across any number of intervening reminder-only wakes.
static RTC_DATA_ATTR int64_t s_next_full_sync_at = 0;

// Whether the alarm currently armed was set for an upcoming reminder's
// exact due_at (true) rather than the regular sync deadline (false). The
// PCF85063 has only one hardware alarm register and can't tell firmware
// *why* it fired -- this is how app_main tells a reminder-only "timer"
// wake apart from a normal poll-interval one.
static RTC_DATA_ATTR bool s_next_wake_is_reminder_only = false;

// Dedups chiming across wake cycles. Snapshots are always a full replace
// with no "fired"/"dismissed" flag on reminders (unlike sync_checkin_t's
// has_fired_at), so a reminder with a past due_at keeps reappearing in
// every snapshot until the backend actually removes it. Persisting just
// the last-announced id (not a set) means: if a second reminder becomes
// due the same day before the backend removes the first, only the
// earliest-due one is guaranteed to chime until it's removed from the
// snapshot -- accepted limitation, see PROJECT_PLAN.md's F9 entry.
static RTC_DATA_ATTR char s_last_announced_reminder_id[SYNC_STR_ID_LEN] = "";

// ---------------------------------------------------------------------
// SD card: mount, write, read back, verify. Also backs debug logging
// below -- ensure_sdcard_mounted() is shared so the two don't fight over
// double-mounting the same card.
// ---------------------------------------------------------------------

static bool s_sdcard_mounting_in_progress = false;
static bool s_sdcard_mounted = false;
static bool s_sdcard_mount_error_reported_this_boot = false;

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
    if (!s_sdcard_mounted && !s_sdcard_mount_error_reported_this_boot) {
        // Set before recording -- record_device_error() itself logs,
        // which (if SD_DEBUG_LOG_ENABLED) can re-enter this function via
        // sd_log_vprintf(). That reentrant call is harmless on its own
        // (this function already supports being called repeatedly, per
        // the comment above), but this flag stops it from recording a
        // second time and log-looping.
        s_sdcard_mount_error_reported_this_boot = true;
        record_device_error("sdcard_mount_failed", "Sdcard_Init() returned false");
    }
    return s_sdcard_mounted;
}

// Caps /sdcard/debug.log at this size before rotating the whole file to
// /sdcard/debug.log.old (overwriting any previous .old), so a device left
// running for weeks/months can't fill the card. Checked once per boot
// rather than per log line -- deep sleep restarts app_main every cycle
// anyway, so a per-boot check still bounds growth tightly enough without
// stat()-ing the file on every single log call.
#define SD_DEBUG_LOG_MAX_BYTES (256 * 1024)

static bool s_debug_log_rotation_checked = false;

static void rotate_debug_log_if_needed(void) {
    struct stat st;
    if (stat(SDlist "/debug.log", &st) == 0 && st.st_size > SD_DEBUG_LOG_MAX_BYTES) {
        remove(SDlist "/debug.log.old");
        rename(SDlist "/debug.log", SDlist "/debug.log.old");
    }
}

// Debugging aid: this board's native USB-Serial/JTAG only stays up while
// awake, and reconnecting to it mid-test can itself trigger a reset (see
// PROJECT_PLAN.md's flashing notes) -- so catching a log for a cycle
// that runs and sleeps again quickly is unreliable over serial alone.
// Mirrors every ESP_LOG* line to /sdcard/debug.log (append mode) in
// addition to the normal console output, installed once at the very top
// of app_main (gated by SD_DEBUG_LOG_ENABLED) so even the earliest boot
// lines are captured. Pull the SD card to read it directly, no serial
// connection needed. Rotated via rotate_debug_log_if_needed() above once
// per boot, so it stays bounded for indefinite/production use.
static int sd_log_vprintf(const char *fmt, va_list args) {
    int ret = vprintf(fmt, args);

    if (ensure_sdcard_mounted()) {
        if (!s_debug_log_rotation_checked) {
            s_debug_log_rotation_checked = true;
            rotate_debug_log_if_needed();
        }
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

// F3/F8 soak test: a dedicated, pre-filtered log separate from the
// verbose debug.log above -- one compact line per normal-cycle wake,
// just the reset/wake cause and battery level, so a multi-hour
// unattended run can be reviewed directly (no grep needed) for both
// brownout/watchdog resets and overnight battery drain. UTC, not local
// time: this runs before sync_tz_apply() applies each cycle's timezone,
// so local time isn't reliably known yet this early -- the RTC's own
// system time (persisted across deep sleep, corrected each successful
// sync) is still meaningful even so, just not yet TZ-adjusted.
static void log_reset_history(const char *reset_reason, const char *wake_reason, float vbat, int battery_pct) {
    if (!ensure_sdcard_mounted()) {
        return;
    }
    FILE *f = fopen(SDlist "/reset_history.log", "a");
    if (!f) {
        return;
    }
    time_t now = time(nullptr);
    struct tm utc_tm;
    gmtime_r(&now, &utc_tm);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d UTC reset_reason=%s wake_reason=%s battery=%.3fV(%d%%)\n",
            utc_tm.tm_year + 1900, utc_tm.tm_mon + 1, utc_tm.tm_mday,
            utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec,
            reset_reason, wake_reason, (double)vbat, battery_pct);
    fclose(f);
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
    led_set(false);
    ESP_ERROR_CHECK(rtc_gpio_hold_en((gpio_num_t)VBAT_PWR_PIN));

    esp_deep_sleep_start();
}

// ---------------------------------------------------------------------
// F6 (PROJECT_PLAN.md): push-to-talk. BOOT wakes the device, recording
// starts immediately (no wifi yet -- that only happens after recording
// finishes, right before upload), stops on silence or a second BOOT
// press or a hard duration cap, then uploads and goes straight back to
// sleep. Deliberately skips the entire normal sync cycle (wifi-connect
// beforehand, /device/sync, snapshot render, sdcard_test, RTC-alarm
// rescheduling) -- spec section 4.2 says a sync afterward is optional,
// not required, and keeping this path lean keeps the whole interaction
// fast.
// ---------------------------------------------------------------------

#define PTT_SAMPLE_RATE_HZ 16000
#define PTT_CHANNELS 2 // mic hardware is wired stereo (fixed I2S TDM config
                        // in codec_board); downmixed to mono at upload time.
#define PTT_BYTES_PER_SAMPLE 2
#define PTT_CHUNK_MS 200
// 5 minutes -- was 60000 (60s), raised now that recording streams to SD
// instead of one PSRAM buffer (this board's 8MB total PSRAM couldn't hold
// more than ~90s of stereo audio as a single allocation; SD card capacity
// isn't a meaningful constraint by comparison). A full-length recording at
// this cap does take real time and battery to upload afterward -- see
// expected_upload_ms()'s comment -- that's an inherent cost of supporting
// this length, not a bug.
#define PTT_MAX_RECORD_MS 300000  // hard safety cap regardless of the two below
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

// ---------------------------------------------------------------------
// Voice-note SD retry queue. A push-to-talk recording streams straight to
// a file under PENDING_VOICE_DIR from the moment capture starts (see
// record_until_stopped() below) -- the recording *is* the queue entry,
// not a separate save-on-failure step. A live upload attempt streams from
// that same file; on success the file is deleted, on failure it's already
// sitting in the queue directory, correctly formatted, for
// flush_pending_voice_notes() to retry on a later wake. Bounded to 15
// pending notes (oldest evicted FIFO), one upload attempt per note per
// wake, no expiry by age -- a note is retried forever until it succeeds
// or is evicted.
// ---------------------------------------------------------------------

#define PENDING_VOICE_DIR SDlist "/pending_voice"
#define PENDING_VOICE_MAX_QUEUED 15
#define PENDING_VOICE_MAGIC "APVQ"

#pragma pack(push, 1)
struct pending_voice_header_t {
    char magic[4];             // "APVQ" once finalized; all-zero while recording is still in
                                // progress, so a file left behind by a power cut mid-recording
                                // is unambiguously incomplete and gets cleaned up, not retried.
    uint8_t format_version;    // 1
    uint32_t sample_rate;      // PTT_SAMPLE_RATE_HZ at record time
    uint8_t bits_per_sample;   // 16
    uint8_t channels;          // 1 (always -- downmix happens per-chunk during capture)
    uint32_t mono_bytes;       // payload length following this header; 0 until finalized
    char checkin_id[SYNC_STR_ID_LEN]; // "" if this recording wasn't a reply to a live check-in
};
#pragma pack(pop)

// Only usable entries -- exactly 10 digits + ".pv" -- are ever seq-parsed;
// anything else (stray dotfiles, partial writes with a different name) is
// simply invisible to the queue rather than tripping up the scan.
static bool pending_voice_filename_seq(const char *name, uint32_t *out_seq) {
    size_t len = strlen(name);
    if (len != 13 || strcmp(name + 10, ".pv") != 0) {
        return false;
    }
    for (int i = 0; i < 10; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
    }
    *out_seq = (uint32_t)strtoul(name, nullptr, 10);
    return true;
}

static void pending_voice_path_for_seq(uint32_t seq, char *out_path, size_t out_path_len) {
    snprintf(out_path, out_path_len, "%s/%010u.pv", PENDING_VOICE_DIR, (unsigned)seq);
}

// Single opendir pass: collects every queued sequence number (bounded by
// PENDING_VOICE_MAX_QUEUED, so a fixed-size caller array is safe) and sorts
// ascending -- oldest first, insertion sort since there are never more than
// 15 entries. Directory not existing yet reads the same as "empty"
// (count=0) -- callers create it lazily on first save. Callers that only
// need the oldest/newest entry read out_seqs[0]/out_seqs[count-1].
static int pending_voice_list_sorted(uint32_t *out_seqs, int max_out) {
    DIR *dir = opendir(PENDING_VOICE_DIR);
    if (!dir) {
        return 0;
    }
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr && count < max_out) {
        uint32_t seq;
        if (!pending_voice_filename_seq(entry->d_name, &seq)) {
            continue;
        }
        int pos = count;
        while (pos > 0 && out_seqs[pos - 1] > seq) {
            out_seqs[pos] = out_seqs[pos - 1];
            pos--;
        }
        out_seqs[pos] = seq;
        count++;
    }
    closedir(dir);
    return count;
}

// Bottom-right "N/15" indicator (scale 1), registered with eink_ui via
// eink_ui_set_pending_voice_indicator_cb() in app_main() -- it lives here
// (not in eink_ui) because it needs ensure_sdcard_mounted()/
// pending_voice_list_sorted(), voice-note-queue internals. Silent (draws
// nothing) when the queue is empty, so the normal screen is untouched on
// the common case of every recording having uploaded fine.
static void draw_pending_voice_indicator(void) {
    if (!ensure_sdcard_mounted()) {
        return;
    }
    uint32_t seqs[PENDING_VOICE_MAX_QUEUED];
    int count = pending_voice_list_sorted(seqs, PENDING_VOICE_MAX_QUEUED);
    if (count == 0) {
        return;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%d/%d", count, PENDING_VOICE_MAX_QUEUED);
    int w = (int)strlen(buf) * (FONT_GLYPH_W + 1);
    draw_text(buf, EPD_WIDTH - w - 4, EPD_HEIGHT - FONT_GLYPH_H - 4, 1, DRIVER_COLOR_BLACK);
}

// Two different numbers, deliberately not conflated: expected_upload_ms()
// is a plain estimate (no safety margin) used only to decide how many
// queued notes fit in one wake's flush budget -- see
// flush_pending_voice_notes(). voice_upload_timeout_ms() is the actual
// esp_http_client timeout, padded so a genuinely slower-than-observed
// connection doesn't get killed as a false negative (the same failure mode
// that made the original flat 60s constant insufficient for a full-length
// recording -- 384KB measured taking ~15.9s on real hardware, ~24.7KB/s,
// which scales to ~78s for a full 60s/1.92MB-mono recording).
static int64_t expected_upload_ms(uint32_t mono_bytes) {
    const int64_t EXPECTED_BYTES_PER_SEC = 25000; // rounds the ~24.7KB/s observed
    const int64_t EXPECTED_BASE_MS = 3000;         // typical connection-setup overhead
    return EXPECTED_BASE_MS + (int64_t)mono_bytes * 1000 / EXPECTED_BYTES_PER_SEC;
}

static int voice_upload_timeout_ms(uint32_t mono_bytes) {
    const double SAFETY_MULTIPLIER = 1.35;  // cushion against slower-than-observed throughput
    const int64_t SAFETY_MARGIN_MS = 10000; // flat pad for connection-setup variance
    return (int)((double)expected_upload_ms(mono_bytes) * SAFETY_MULTIPLIER) + SAFETY_MARGIN_MS;
}

// ---------------------------------------------------------------------
// F9: reminder chime. SD card WAV lookup (components/port_bsp's
// ensure_sdcard_mounted() is already used elsewhere in this file) with a
// generated-tone fallback -- no sound asset shipped with the firmware,
// the user drops their own .wav files on the card if/when they have any.
// ---------------------------------------------------------------------

#define CHIME_DIR SDlist "/notesnd"
#define CHIME_SAMPLE_RATE_HZ 16000 // must match Codec_StartInit()'s opened format
#define CHIME_CHANNELS 2           // codec is opened fixed-stereo regardless of source
#define CHIME_BYTES_PER_SAMPLE 2
#define CHIME_IO_CHUNK_FRAMES 1024 // frames per read/write chunk (1 frame = 1 sample per channel)
#define CHIME_VOLUME_PERCENT 40    // starting point -- tune after hearing it on hardware;
                                    // deliberately below Codec_StartInit()'s full-volume(100)
                                    // default, which is a quiet bedside/desk device (see
                                    // PROJECT_PLAN.md's clock-tick-noise removal notes)
#define CHIME_TONE_FREQ_HZ 880.0
#define CHIME_TONE_DURATION_MS 700
#define CHIME_TONE_FADE_IN_FRACTION 0.6 // the requested "slow ramp-up"
#define CHIME_TONE_FADE_OUT_MS 60       // short tail, avoids a click at buffer end
#define CHIME_TONE_AMPLITUDE 0.6        // headroom below full-scale

// Usable chime candidate: a ".wav" file, not a dotfile/".."/macOS "._*"
// AppleDouble junk that Finder writes when copying to SD cards.
static bool is_usable_wav_filename(const char *name) {
    if (name[0] == '.') {
        return false;
    }
    size_t name_len = strlen(name);
    return name_len >= 4 && strcasecmp(name + name_len - 4, ".wav") == 0;
}

// Picks uniformly at random among every usable ".wav" file under
// CHIME_DIR, so repeated chimes don't always play the same sound once
// more than one is present. Two passes -- first counts usable entries,
// second re-scans and returns the one at a randomly chosen index --
// rather than collecting names into an array, keeping memory use bounded
// regardless of how large the folder grows. readdir() order is stable
// across the two back-to-back scans since nothing else writes to this
// directory mid-chime. False (silent fallback to the generated tone) if
// the directory doesn't exist or has no usable entry -- this is an
// expected, not an error, state (matches this file's "degrade
// independently" pattern elsewhere).
static bool find_random_wav_in_notesnd(char *out_path, size_t out_path_len) {
    DIR *dir = opendir(CHIME_DIR);
    if (!dir) {
        return false;
    }
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (is_usable_wav_filename(entry->d_name)) {
            count++;
        }
    }
    closedir(dir);
    if (count == 0) {
        return false;
    }

    int target = (int)(esp_random() % (uint32_t)count);

    dir = opendir(CHIME_DIR);
    if (!dir) {
        return false; // pathological: directory vanished between the two passes
    }
    bool found = false;
    int i = 0;
    while ((entry = readdir(dir)) != nullptr) {
        if (!is_usable_wav_filename(entry->d_name)) {
            continue;
        }
        if (i == target) {
            snprintf(out_path, out_path_len, "%s/%s", CHIME_DIR, entry->d_name);
            found = true;
            break;
        }
        i++;
    }
    closedir(dir);
    return found;
}

struct wav_fmt_info_t {
    uint16_t audio_format;
    uint16_t num_channels;
    uint16_t bits_per_sample;
    uint32_t sample_rate;
    long data_offset;
    uint32_t data_length;
};

// Minimal RIFF/WAVE chunk walker -- just enough to locate `fmt ` and
// `data`, tolerating (and skipping over) any other chunk type via
// word-aligned chunk-size seeks. Returns false if EOF is hit before both
// required chunks are found, or the file isn't RIFF/WAVE at all.
static bool wav_parse_header(FILE *f, wav_fmt_info_t *out) {
    char riff_tag[4], wave_tag[4];
    uint32_t riff_size;
    if (fread(riff_tag, 1, 4, f) != 4 || fread(&riff_size, 4, 1, f) != 1 || fread(wave_tag, 1, 4, f) != 4) {
        return false;
    }
    if (memcmp(riff_tag, "RIFF", 4) != 0 || memcmp(wave_tag, "WAVE", 4) != 0) {
        return false;
    }

    bool have_fmt = false, have_data = false;
    memset(out, 0, sizeof(*out));

    while (!(have_fmt && have_data)) {
        char chunk_tag[4];
        uint32_t chunk_size;
        if (fread(chunk_tag, 1, 4, f) != 4 || fread(&chunk_size, 4, 1, f) != 1) {
            break; // EOF before both chunks were found
        }
        if (memcmp(chunk_tag, "fmt ", 4) == 0 && chunk_size >= 16) {
            long fmt_start = ftell(f);
            if (fread(&out->audio_format, 2, 1, f) != 1 || fread(&out->num_channels, 2, 1, f) != 1 ||
                fread(&out->sample_rate, 4, 1, f) != 1) {
                break;
            }
            fseek(f, fmt_start + 14, SEEK_SET); // skip byte_rate/block_align to bits_per_sample
            if (fread(&out->bits_per_sample, 2, 1, f) != 1) {
                break;
            }
            fseek(f, fmt_start + chunk_size + (chunk_size & 1), SEEK_SET); // skip any WAVE_FORMAT_EXTENSIBLE tail
            have_fmt = true;
        } else if (memcmp(chunk_tag, "data", 4) == 0) {
            out->data_offset = ftell(f);
            out->data_length = chunk_size;
            have_data = true;
            // Deliberately not seeking past the data chunk -- it's the
            // last thing this reader needs.
        } else {
            if (fseek(f, chunk_size + (chunk_size & 1), SEEK_CUR) != 0) {
                break;
            }
        }
    }
    return have_fmt && have_data;
}

static bool wav_file_matches_codec_format(const wav_fmt_info_t &fmt) {
    return fmt.audio_format == 1 /* PCM */ && fmt.bits_per_sample == 16 && fmt.sample_rate == CHIME_SAMPLE_RATE_HZ &&
           (fmt.num_channels == 1 || fmt.num_channels == 2);
}

// Shared tail for both the WAV and generated-tone playback paths.
static void codec_write_pcm_chunked(const uint8_t *data, size_t total_bytes) {
    size_t offset = 0;
    while (offset < total_bytes) {
        size_t remaining = total_bytes - offset;
        size_t chunk = remaining < CHIME_IO_CHUNK_FRAMES * CHIME_CHANNELS * CHIME_BYTES_PER_SAMPLE
                           ? remaining
                           : CHIME_IO_CHUNK_FRAMES * CHIME_CHANNELS * CHIME_BYTES_PER_SAMPLE;
        Codec_PlaybackData(const_cast<uint8_t *>(data + offset), chunk);
        offset += chunk;
    }
}

// Streams the `data` chunk of `path` to the codec, upmixing mono to
// interleaved stereo per chunk (the codec is opened fixed-stereo). False
// (falls back to the generated tone, with a logged warning) if the file
// can't be opened, isn't a well-formed WAV, or doesn't exactly match the
// codec's opened format -- no resampling is attempted.
static bool play_wav_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "chime: could not open %s", path);
        return false;
    }
    wav_fmt_info_t fmt;
    if (!wav_parse_header(f, &fmt) || !wav_file_matches_codec_format(fmt)) {
        ESP_LOGW(TAG, "chime: %s is not a usable WAV (need PCM/16-bit/%dHz mono-or-stereo)", path, CHIME_SAMPLE_RATE_HZ);
        char msg[160];
        snprintf(msg, sizeof(msg), "unusable WAV: %s", path);
        record_device_error("chime_wav_invalid", msg);
        fclose(f);
        return false;
    }
    fseek(f, fmt.data_offset, SEEK_SET);

    const size_t read_frames = CHIME_IO_CHUNK_FRAMES;
    const size_t src_bytes_per_frame = fmt.num_channels * CHIME_BYTES_PER_SAMPLE;
    uint8_t *read_buf = static_cast<uint8_t *>(heap_caps_malloc(read_frames * src_bytes_per_frame, MALLOC_CAP_SPIRAM));
    uint8_t *stereo_buf = static_cast<uint8_t *>(
        heap_caps_malloc(read_frames * CHIME_CHANNELS * CHIME_BYTES_PER_SAMPLE, MALLOC_CAP_SPIRAM));
    if (!read_buf || !stereo_buf) {
        ESP_LOGE(TAG, "chime: failed to allocate WAV read buffers");
        record_device_error("heap_alloc_failed", "play_wav_file read/stereo buffers");
        if (read_buf) heap_caps_free(read_buf);
        if (stereo_buf) heap_caps_free(stereo_buf);
        fclose(f);
        return false;
    }

    uint32_t bytes_remaining = fmt.data_length;
    while (bytes_remaining > 0) {
        size_t want_bytes = read_frames * src_bytes_per_frame;
        if (want_bytes > bytes_remaining) {
            want_bytes = bytes_remaining;
        }
        size_t got = fread(read_buf, 1, want_bytes, f);
        if (got == 0) {
            break;
        }
        size_t frames = got / src_bytes_per_frame;

        if (fmt.num_channels == 2) {
            codec_write_pcm_chunked(read_buf, frames * CHIME_CHANNELS * CHIME_BYTES_PER_SAMPLE);
        } else {
            const int16_t *mono = reinterpret_cast<const int16_t *>(read_buf);
            int16_t *stereo = reinterpret_cast<int16_t *>(stereo_buf);
            for (size_t i = 0; i < frames; i++) {
                stereo[i * 2] = mono[i];
                stereo[i * 2 + 1] = mono[i];
            }
            codec_write_pcm_chunked(stereo_buf, frames * CHIME_CHANNELS * CHIME_BYTES_PER_SAMPLE);
        }
        bytes_remaining -= (uint32_t)got;
    }

    heap_caps_free(read_buf);
    heap_caps_free(stereo_buf);
    fclose(f);
    return true;
}

// No SD card / no usable file fallback: a short synthesized sine beep
// with a slow amplitude ramp-up (the requested "slow ramp-up") and a
// brief fade-out tail to avoid a click at the end of the buffer.
static void play_generated_tone_chime(void) {
    size_t total_frames = (size_t)CHIME_SAMPLE_RATE_HZ * CHIME_TONE_DURATION_MS / 1000;
    size_t fade_in_frames = (size_t)((double)total_frames * CHIME_TONE_FADE_IN_FRACTION);
    size_t fade_out_frames = (size_t)CHIME_SAMPLE_RATE_HZ * CHIME_TONE_FADE_OUT_MS / 1000;

    int16_t *buf = static_cast<int16_t *>(
        heap_caps_malloc(total_frames * CHIME_CHANNELS * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (!buf) {
        ESP_LOGE(TAG, "chime: failed to allocate tone buffer");
        return;
    }

    for (size_t i = 0; i < total_frames; i++) {
        double envelope = 1.0;
        if (i < fade_in_frames) {
            envelope = (double)i / (double)fade_in_frames;
        } else if (i >= total_frames - fade_out_frames) {
            envelope = (double)(total_frames - i) / (double)fade_out_frames;
        }
        double t = (double)i / CHIME_SAMPLE_RATE_HZ;
        double sample = envelope * CHIME_TONE_AMPLITUDE * sin(2.0 * M_PI * CHIME_TONE_FREQ_HZ * t);
        int16_t s16 = (int16_t)(sample * 32767.0);
        buf[i * 2] = s16;
        buf[i * 2 + 1] = s16;
    }

    codec_write_pcm_chunked(reinterpret_cast<uint8_t *>(buf), total_frames * CHIME_CHANNELS * sizeof(int16_t));
    heap_caps_free(buf);
}

// Single entry point: try an SD-card WAV first, fall back to the
// generated tone. Handles its own audio power-on/off, matching the
// pattern used by run_push_to_talk_cycle() elsewhere in this file.
static void play_reminder_chime(void) {
    BoardPower_Audio_ON();
    Codec_StartInit();
    Codec_SetPlaybackVolume(CHIME_VOLUME_PERCENT);

    bool played = false;
    char wav_path[160];
    if (ensure_sdcard_mounted() && find_random_wav_in_notesnd(wav_path, sizeof(wav_path))) {
        played = play_wav_file(wav_path);
    }
    if (!played) {
        play_generated_tone_chime();
    }

    BoardPower_Audio_OFF();
}

// Chimes for `soonest` if it's actually due and hasn't already been
// announced (dedup via s_last_announced_reminder_id). Returns true if it
// fired this cycle (chimed + marked announced) -- callers use that to
// decide whether to override a live check-in on screen with this
// reminder. Takes precedence over any live check-in unconditionally: a
// reminder becoming due always wins, it no longer defers to a check-in
// that's currently showing. Only ever called on a genuine RTC-timer
// wake (both call sites gate on wake_reason == "timer"), which is what
// keeps this safe to also use for the display-override decision --
// s_last_announced_reminder_id would otherwise risk being set by a
// PWR-tap sync, silently suppressing the real timer-wake chime later.
static bool handle_due_reminder(const sync_soonest_reminder_t &soonest) {
    if (!soonest.have_reminder) {
        return false;
    }
    int64_t now_epoch = time(nullptr);
    if (soonest.due_at > now_epoch) {
        return false; // not due yet -- alarm scheduling wakes early for it
    }
    if (strncmp(soonest.id, s_last_announced_reminder_id, sizeof(s_last_announced_reminder_id)) == 0) {
        return false; // already chimed for this exact reminder id
    }

    ESP_LOGI(TAG, "reminder due: playing chime for id=%s", soonest.id);
    play_reminder_chime();
    strncpy(s_last_announced_reminder_id, soonest.id, sizeof(s_last_announced_reminder_id) - 1);
    s_last_announced_reminder_id[sizeof(s_last_announced_reminder_id) - 1] = '\0';
    return true;
}

// Soonest-due entry (past or future) in the compact RTC-persisted cache --
// same shape as sync_soonest_reminder_t so it can feed straight into
// handle_due_reminder() and, when overriding a live check-in on screen,
// straight into a next_item_t for drawing too.
static sync_soonest_reminder_t find_soonest_cached_reminder(void) {
    sync_soonest_reminder_t result = {};
    for (int i = 0; i < s_pending_reminders_count; i++) {
        if (!result.have_reminder || s_pending_reminders[i].due_at < result.due_at) {
            result.have_reminder = true;
            result.due_at = s_pending_reminders[i].due_at;
            strncpy(result.id, s_pending_reminders[i].id, sizeof(result.id) - 1);
            result.id[sizeof(result.id) - 1] = '\0';
            strncpy(result.message, s_pending_reminders[i].message, sizeof(result.message) - 1);
            result.message[sizeof(result.message) - 1] = '\0';
        }
    }
    return result;
}

// Shared tail for both the full-sync cycle and reminder-only wakes:
// decides the next alarm from cached reminder data + the known sync
// deadline, persists whether the next "timer" wake should be the
// lightweight reminder-only path, and sleeps. Never returns.
static void schedule_next_wake_and_sleep(pcf85063a_dev_t *rtc_dev) {
    int64_t now_epoch = time(nullptr);
    sync_soonest_reminder_t soonest = find_soonest_cached_reminder();
    bool have_future_reminder = soonest.have_reminder && soonest.due_at > now_epoch;
    int64_t seconds_until_reminder = have_future_reminder ? (soonest.due_at - now_epoch) : 0;

    int64_t seconds_until_sync = (s_next_full_sync_at > 0)
        ? (s_next_full_sync_at - now_epoch)
        : sync_effective_poll_interval(s_has_synced_ever, s_last_known_good_poll_interval, FALLBACK_POLL_INTERVAL_SECONDS);

    bool is_reminder_wake = false;
    int interval = sync_effective_alarm_interval_seconds(
        (int)seconds_until_sync, have_future_reminder, seconds_until_reminder,
        SYNC_MIN_ALARM_INTERVAL_SECONDS, &is_reminder_wake);

    s_next_wake_is_reminder_only = is_reminder_wake;
    ESP_LOGI(TAG, "next wake in %ds (%s)", interval, is_reminder_wake ? "reminder-only" : "full sync");
    rtc_schedule_alarm(rtc_dev, interval);
    enter_deep_sleep(); // never returns
}

// F9: the lightweight path for a "timer" wake that was scheduled
// specifically for an upcoming reminder (s_next_wake_is_reminder_only).
// No wifi, no /device/sync -- just chime (if still due and not already
// announced) and reschedule from whatever was cached at the last full
// sync. The one exception: if the reminder just activated *and* a live
// check-in was showing, this redraws the screen to show the reminder
// instead (using cached weather + a fresh local battery read -- no
// network involved either way), overriding the check-in. That's the
// only case that touches ADC/e-ink here; the common case (no check-in
// showing) stays exactly as cheap as before. Never returns.
static void handle_reminder_only_wake(pcf85063a_dev_t *rtc_dev) {
    sync_soonest_reminder_t soonest = find_soonest_cached_reminder();
    bool activated = handle_due_reminder(soonest);

    if (activated && s_last_screen.valid && s_last_screen.is_checkin) {
        BoardAdc_Init();
        uint8_t battery_pct = Get_Batterylevel();

        next_item_t reminder_item = {};
        reminder_item.have_next = true;
        reminder_item.is_event = false;
        reminder_item.at = soonest.due_at;
        strncpy(reminder_item.label, soonest.message, sizeof(reminder_item.label) - 1);
        reminder_item.label[sizeof(reminder_item.label) - 1] = '\0';

        draw_reminder_override_screen(s_last_screen.has_weather, s_last_screen.weather, battery_pct, reminder_item);
        ESP_LOGI(TAG, "e-ink render done (reminder-override, reminder-only wake)");

        s_last_screen.is_checkin = false;
        s_last_screen.next = reminder_item;
    }

    schedule_next_wake_and_sleep(rtc_dev); // never returns
}

// Records in ~200ms chunks, downmixing each chunk to mono and streaming it
// straight to out_file as it's captured -- streaming to SD instead of
// accumulating one big PSRAM buffer is what decouples recording length
// from PSRAM size (this board's PSRAM is only 8MB total, PROJECT_PLAN.md:131;
// a multi-minute stereo buffer wouldn't fit). Stops on whichever of three
// independent conditions fires first: a second BOOT press, ~2s of
// continuous silence (after an initial warm-up grace period), or the hard
// PTT_MAX_RECORD_MS cap. Returns the number of mono bytes actually written.
static size_t record_until_stopped(FILE *out_file) {
    const size_t stereo_chunk_bytes = (size_t)PTT_SAMPLE_RATE_HZ * PTT_CHANNELS * PTT_BYTES_PER_SAMPLE * PTT_CHUNK_MS / 1000;
    const size_t mono_chunk_bytes = stereo_chunk_bytes / 2;

    // Callers only ever reach here after wait_for_tap_or_hold() has
    // already confirmed the original wake-press was a tap (i.e. already
    // released) -- no separate "wait for release" guard needed here
    // anymore before constructing the button object below.

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

    // Small, reused-every-chunk buffers -- this is now the *only* PSRAM the
    // recording path needs (a few KB total, versus megabytes when the
    // whole recording was buffered in memory before upload).
    auto *stereo_chunk = static_cast<int16_t *>(heap_caps_malloc(stereo_chunk_bytes, MALLOC_CAP_SPIRAM));
    auto *mono_chunk = static_cast<int16_t *>(heap_caps_malloc(mono_chunk_bytes, MALLOC_CAP_SPIRAM));
    if (!stereo_chunk || !mono_chunk) {
        ESP_LOGE(TAG, "ptt: failed to allocate recording chunk buffers");
        if (stereo_chunk) heap_caps_free(stereo_chunk);
        if (mono_chunk) heap_caps_free(mono_chunk);
        return 0;
    }

    size_t mono_bytes_written = 0;
    int elapsed_ms = 0;
    int silence_ms = 0;
    bool write_ok = true;

    while (write_ok) {
        Codec_RecordData(reinterpret_cast<uint8_t *>(stereo_chunk), stereo_chunk_bytes);
        elapsed_ms += PTT_CHUNK_MS;

        size_t stereo_sample_count = stereo_chunk_bytes / sizeof(int16_t);
        int64_t sum_sq = 0;
        for (size_t i = 0; i < stereo_sample_count; i++) {
            sum_sq += (int64_t)stereo_chunk[i] * stereo_chunk[i];
        }
        float rms = sqrtf((float)sum_sq / (float)stereo_sample_count);
        ESP_LOGD(TAG, "ptt chunk rms=%.1f elapsed_ms=%d silence_ms=%d", (double)rms, elapsed_ms, silence_ms);

        if (elapsed_ms > PTT_SILENCE_WARMUP_MS) {
            silence_ms = (rms < PTT_SILENCE_RMS_THRESHOLD) ? (silence_ms + PTT_CHUNK_MS) : 0;
        }

        // Same average-L+R downmix math used everywhere else in this file
        // for stereo->mono conversion.
        size_t mono_frame_count = stereo_chunk_bytes / (PTT_CHANNELS * sizeof(int16_t));
        for (size_t i = 0; i < mono_frame_count; i++) {
            int16_t l = stereo_chunk[i * 2];
            int16_t r = stereo_chunk[i * 2 + 1];
            mono_chunk[i] = (int16_t)(((int32_t)l + (int32_t)r) / 2);
        }
        write_ok = fwrite(mono_chunk, 1, mono_chunk_bytes, out_file) == mono_chunk_bytes;
        if (write_ok) {
            mono_bytes_written += mono_chunk_bytes;
        } else {
            ESP_LOGE(TAG, "ptt: SD write failed mid-recording");
            break;
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

    heap_caps_free(stereo_chunk);
    heap_caps_free(mono_chunk);
    return mono_bytes_written;
}

static const char *VOICE_MULTIPART_BOUNDARY = "----adhiVoiceBoundary7f3a";

struct voice_multipart_parts_t {
    char checkin_part[192];
    int checkin_part_len;
    char preamble[192];
    int preamble_len;
    char epilogue[64];
    int epilogue_len;
    wav_header_t wav_hdr;
    size_t content_length;
};

// Pure computation, no I/O -- builds the multipart preamble/epilogue (and
// the optional checkin_id field, F7) plus the WAV header and total
// content-length, split out from upload_voice_note_from_file() so that
// function's body is just the network/file streaming steps.
static voice_multipart_parts_t build_voice_multipart_parts(const char *checkin_id, uint32_t mono_bytes) {
    voice_multipart_parts_t parts = {};

    if (checkin_id && checkin_id[0] != '\0') {
        parts.checkin_part_len = snprintf(parts.checkin_part, sizeof(parts.checkin_part),
                                           "--%s\r\n"
                                           "Content-Disposition: form-data; name=\"checkin_id\"\r\n"
                                           "\r\n"
                                           "%s\r\n",
                                           VOICE_MULTIPART_BOUNDARY, checkin_id);
    }

    parts.preamble_len = snprintf(parts.preamble, sizeof(parts.preamble),
                                   "--%s\r\n"
                                   "Content-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n"
                                   "Content-Type: audio/wav\r\n"
                                   "\r\n",
                                   VOICE_MULTIPART_BOUNDARY);

    parts.epilogue_len = snprintf(parts.epilogue, sizeof(parts.epilogue), "\r\n--%s--\r\n", VOICE_MULTIPART_BOUNDARY);

    parts.wav_hdr = build_wav_header(PTT_SAMPLE_RATE_HZ, 1, 16, mono_bytes);
    parts.content_length =
        (size_t)parts.checkin_part_len + (size_t)parts.preamble_len + sizeof(parts.wav_hdr) + mono_bytes + (size_t)parts.epilogue_len;
    return parts;
}

// Writes the multipart body (checkin_id field if present, file-part
// preamble, WAV header, chunked PCM read off SD, epilogue) to an
// already-open esp_http_client connection. Takes ownership of f -- always
// closes it before returning, success or failure. Logs its own failure
// reason (chunk-buffer allocation vs. a write failing partway through) so
// the caller doesn't need to.
static bool stream_voice_multipart_body(esp_http_client_handle_t client, FILE *f, uint32_t mono_bytes,
                                         const voice_multipart_parts_t &parts) {
    bool write_ok = true;
    if (parts.checkin_part_len > 0) {
        write_ok = write_ok && esp_http_client_write(client, parts.checkin_part, parts.checkin_part_len) >= 0;
    }
    write_ok = write_ok && esp_http_client_write(client, parts.preamble, parts.preamble_len) >= 0;
    write_ok = write_ok && esp_http_client_write(client, reinterpret_cast<const char *>(&parts.wav_hdr), sizeof(parts.wav_hdr)) >= 0;

    // Chunked read straight off SD -- must be heap/PSRAM-allocated, not a
    // stack local: the main task stack is only 8192 bytes total, and an
    // 8KB stack array alone nearly fills it, which crashed outright on
    // real hardware (silent panic, no error logged first -- a stack
    // overflow, unlike the SPI-reinit crash found earlier, which did log
    // before aborting).
    const size_t READ_CHUNK_BYTES = 8192;
    auto *chunk = static_cast<uint8_t *>(heap_caps_malloc(READ_CHUNK_BYTES, MALLOC_CAP_SPIRAM));
    if (!chunk) {
        ESP_LOGE(TAG, "/voice: failed to allocate upload chunk buffer");
        fclose(f);
        return false;
    }

    uint32_t remaining = mono_bytes;
    while (write_ok && remaining > 0) {
        size_t want = remaining < READ_CHUNK_BYTES ? remaining : READ_CHUNK_BYTES;
        size_t got = fread(chunk, 1, want, f);
        if (got == 0) {
            write_ok = false;
            break;
        }
        write_ok = esp_http_client_write(client, reinterpret_cast<const char *>(chunk), (int)got) >= 0;
        remaining -= (uint32_t)got;
    }
    heap_caps_free(chunk);
    fclose(f);

    write_ok = write_ok && esp_http_client_write(client, parts.epilogue, parts.epilogue_len) >= 0;
    if (!write_ok) {
        ESP_LOGE(TAG, "/voice: write failed mid-upload");
    }
    return write_ok;
}

// POST /voice multipart. field name must be exactly "file" (matches
// adhi-backend/voice.py's `file: UploadFile = File(...)`); never sends
// one_shot/sync (spec section 3.2 / voice.py's own docstring: testing-only,
// real hardware must never set them). No built-in multipart helper in
// esp_http_client -- boundary/headers are hand-built and streamed via
// open()/write()/fetch_headers() rather than the single-buffer
// set_post_field() pattern device_sync_attempt() uses, since the body
// here (WAV header + audio) is too large to comfortably build as one
// contiguous string first. No retry loop: unlike /device/sync, voice
// failures are already designed to be silent/best-effort backend-side
// (spec's fire-and-forget framing), so one attempt is enough here -- a
// failed attempt leaves the file in place under PENDING_VOICE_DIR for
// flush_pending_voice_notes() to retry on a later wake.
//
// Streams the mono PCM payload straight from an already-downmixed file on
// SD (mono_pcm_path, produced either by record_until_stopped() live or
// already sitting queued from a previous failed attempt) rather than a
// memory buffer -- mirrors the established fopen -> seek-past-header ->
// chunked fread pattern already used by play_wav_file() elsewhere in this
// file, so there's exactly one upload implementation regardless of
// whether this is a brand-new recording or a queued retry.
//
// F7: checkin_id is non-null only when this recording is a reply to a
// currently-displayed live check-in -- sent as its own form field
// (spec section 3.2) so the backend routes the reply to that check-in
// instead of resolving it by keyword/recency.
static bool upload_voice_note_from_file(const char *mono_pcm_path, uint32_t mono_bytes, const char *checkin_id) {
    voice_multipart_parts_t parts = build_voice_multipart_parts(checkin_id, mono_bytes);

    char content_type_header[64];
    snprintf(content_type_header, sizeof(content_type_header), "multipart/form-data; boundary=%s", VOICE_MULTIPART_BOUNDARY);

    char url[256];
    snprintf(url, sizeof(url), "%s/voice", BACKEND_BASE_URL);

    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    // Sized from the payload rather than a flat constant -- see
    // voice_upload_timeout_ms()'s comment for why a flat 60s (the original
    // guess, itself already a bump from an even-too-short 15s once real
    // hardware showed 384KB taking ~15.9s / ~25KB/s) still isn't enough
    // once recordings can run several minutes long.
    config.timeout_ms = voice_upload_timeout_ms(mono_bytes);
    config.crt_bundle_attach = esp_crt_bundle_attach;

    FILE *f = fopen(mono_pcm_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "/voice: could not open %s for upload", mono_pcm_path);
        return false;
    }
    if (fseek(f, sizeof(pending_voice_header_t), SEEK_SET) != 0) {
        ESP_LOGE(TAG, "/voice: failed to seek past header in %s", mono_pcm_path);
        fclose(f);
        return false;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "auth", API_TOKEN);
    esp_http_client_set_header(client, "Content-Type", content_type_header);

    esp_err_t err = esp_http_client_open(client, (int)parts.content_length);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "/voice: failed to open connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        fclose(f);
        return false;
    }

    if (!stream_voice_multipart_body(client, f, mono_bytes, parts)) {
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    bool ok = (status >= 200 && status < 300);
    ESP_LOGI(TAG, "/voice upload %s (status %d, %u bytes sent)", ok ? "OK" : "FAILED", status, (unsigned)parts.content_length);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

// Validates a queued file's header before trusting mono_bytes/checkin_id
// from it. Rejects anything left behind by a power cut mid-recording or
// mid-finalize (all-zero magic, or a size that doesn't match the header's
// own accounting) -- this is data-integrity cleanup for corrupt entries,
// not the age/attempt-count eviction that's deliberately out of scope for
// this queue (see the queue's top-of-section comment).
static bool pending_voice_read_valid_header(const char *path, pending_voice_header_t *out_hdr) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    bool ok = fread(out_hdr, sizeof(*out_hdr), 1, f) == 1;
    if (ok && memcmp(out_hdr->magic, PENDING_VOICE_MAGIC, sizeof(out_hdr->magic)) != 0) {
        ok = false;
    }
    if (ok) {
        struct stat st;
        ok = (fstat(fileno(f), &st) == 0) &&
             ((uint64_t)st.st_size == sizeof(pending_voice_header_t) + (uint64_t)out_hdr->mono_bytes);
    }
    fclose(f);
    return ok;
}

// Called once per normal wake, only after wifi is confirmed up (mirrors
// flush_pending_device_error()'s call site/guard exactly). Retries queued
// notes oldest-first, one upload attempt each, within a 5-minute
// total-connected-time budget for this wake -- see expected_upload_ms()/
// voice_upload_timeout_ms()'s comment for the full reasoning. The budget
// check is a look-ahead (elapsed-so-far + this note's estimate), not a
// plain "have we already blown the budget" check -- a plain check would
// let a note start that can't possibly finish in time. The very first
// note attempted this wake is never skipped by the look-ahead, so a
// single oversized note at the front of the queue still gets its one
// attempt rather than being blocked by its own size forever.
static void flush_pending_voice_notes(void) {
    if (!ensure_sdcard_mounted()) {
        return;
    }
    uint32_t seqs[PENDING_VOICE_MAX_QUEUED];
    int count = pending_voice_list_sorted(seqs, PENDING_VOICE_MAX_QUEUED);
    if (count == 0) {
        return;
    }

    const int64_t FLUSH_BUDGET_MS = 5 * 60 * 1000;
    int64_t elapsed_ms = 0;

    for (int i = 0; i < count; i++) {
        char path[64];
        pending_voice_path_for_seq(seqs[i], path, sizeof(path));

        pending_voice_header_t hdr;
        if (!pending_voice_read_valid_header(path, &hdr)) {
            ESP_LOGW(TAG, "voice queue: %s is corrupt/incomplete, discarding", path);
            remove(path);
            continue;
        }

        if (elapsed_ms > 0 && elapsed_ms + expected_upload_ms(hdr.mono_bytes) > FLUSH_BUDGET_MS) {
            ESP_LOGI(TAG, "voice queue: flush budget spent, leaving %d note(s) for next wake", count - i);
            break;
        }

        const char *checkin_id = hdr.checkin_id[0] != '\0' ? hdr.checkin_id : nullptr;
        int64_t t0 = esp_timer_get_time();
        bool ok = upload_voice_note_from_file(path, hdr.mono_bytes, checkin_id);
        elapsed_ms += (esp_timer_get_time() - t0) / 1000;

        if (ok) {
            remove(path);
        }
        // else: leave in place -- exactly one attempt this wake, retried
        // again on a later wake per the queue's no-expiry policy.
    }
}

// F7: POST /device/checkin/{id}/skip -- no request body (spec section
// 3.3). Note the /device prefix: POST /checkin/{id}/skip (no /device) is
// a *different* endpoint for the backend's own magic-link flow and does
// not accept this device's auth token at all.
static bool skip_checkin(const char *checkin_id) {
    char url[256];
    snprintf(url, sizeof(url), "%s/device/checkin/%s/skip", BACKEND_BASE_URL, checkin_id);

    int status = -1;
    bool ok = http_post(url, nullptr, nullptr, nullptr, &status);
    if (status >= 0) {
        ESP_LOGI(TAG, "checkin skip %s (status %d)", ok ? "OK" : "FAILED", status);
    }
    return ok;
}

// Distinguishes a tap from a hold on the very press that woke the
// device (BOOT: tap=record/hold=skip a live check-in; PWR:
// tap=sync-now/hold=power off). Returns as soon as either the pin
// releases (tap) or it's still held past LONG_PRESS_MS (hold) -- the
// hold case doesn't wait for release, so feedback is immediate. A
// normal tap has almost always already released by the time app_main
// reaches this point (boot takes 300-700ms), so the tap path typically
// returns on its very first check.
static bool wait_for_tap_or_hold(gpio_num_t pin) {
    const int LONG_PRESS_MS = 1500;
    const int POLL_INTERVAL_MS = 50;
    int held_ms = 0;
    while (gpio_get_level(pin) == 0) {
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
        held_ms += POLL_INTERVAL_MS;
        if (held_ms >= LONG_PRESS_MS) {
            return true; // still held past the threshold -- a hold
        }
    }
    return false; // released before the threshold -- a normal tap
}

// F7: skip the currently-displayed live check-in. Shows a confirmation,
// then deliberately does *not* restore s_last_screen afterward -- it
// still describes the now-resolved check-in, and redrawing it would
// make an already-skipped prompt look like it's still pending. The next
// real sync (PWR press or the regular RTC alarm) will bring fresh
// content; leaving the confirmation up until then is a minor, honest
// staleness, not a wrong one.
static void run_checkin_skip_cycle(void) {
    ESP_LOGI(TAG, "push-to-talk: BOOT held, skipping live check-in %s", s_last_screen.checkin_id);
    eink_show_message("SKIPPING...");

    bool wifi_ok = wifi_connect();
    bool skip_ok = false;
    if (wifi_ok) {
        skip_ok = skip_checkin(s_last_screen.checkin_id);
    } else {
        ESP_LOGE(TAG, "checkin skip: wifi never connected");
    }
    eink_show_message(skip_ok ? "SKIPPED" : "SKIP FAILED");

    s_last_screen.valid = false;
}

// Bug fix: PWR held ~1.5s genuinely powers the device off -- not just
// deep sleep. VBAT_PWR_PIN very likely gates power to the ESP32 itself,
// not just peripherals (see the power-latch comment in app_main); the
// original Waveshare reference example cuts it directly with no further
// action afterward, treating the cut itself as sufficiently immediate.
// Deliberately does *not* arm any EXT1/RTC wake source afterward (unlike
// enter_deep_sleep()) -- this is meant to be a genuine off state that
// only a fresh physical PWR press (re-establishing power from scratch,
// the same way the very first power-on works) can undo, not something
// the RTC alarm or another button press should be able to interrupt.
static void run_power_off_cycle(void) {
    ESP_LOGI(TAG, "PWR held -- shutting down");

    // Two centered lines instead of a plain "SHUTTING DOWN" message --
    // e-ink is bistable, so this stays visible on screen after VBAT is
    // cut below, same as the normal message would.
    eink_ensure_initialized();
    EPD_Clear();
    const int scale = 3;
    const char *line1 = "TRACK";
    const char *line2 = "DECK";
    int w1 = (int)strlen(line1) * (FONT_GLYPH_W + 1) * scale;
    int w2 = (int)strlen(line2) * (FONT_GLYPH_W + 1) * scale;
    int line_h = (FONT_GLYPH_H + 4) * scale;
    int y0 = (EPD_HEIGHT - line_h * 2) / 2;
    draw_text(line1, (EPD_WIDTH - w1) / 2, y0, scale, DRIVER_COLOR_BLACK);
    draw_text(line2, (EPD_WIDTH - w2) / 2, y0 + line_h, scale, DRIVER_COLOR_BLACK);
    EPD_Display();
    ESP_LOGI(TAG, "e-ink message shown: track/deck");

    vTaskDelay(pdMS_TO_TICKS(300));

    BoardPower_EPD_OFF();
    BoardPower_Audio_OFF();
    led_set(false);
    BoardPower_VBAT_OFF();

    // Safety net in case cutting VBAT above doesn't kill power
    // immediately (e.g. residual charge): fall into a deep sleep with no
    // wake source armed at all, rather than the normal
    // enter_deep_sleep() (which re-arms EXT1 and keeps VBAT held for the
    // next scheduled wake).
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_deep_sleep_start();
}

struct ptt_recording_result_t {
    bool have_pending_file;
    char pending_path[64];
    uint32_t pending_mono_bytes;
};

// Records one push-to-talk take straight to a new file under
// PENDING_VOICE_DIR, using the same on-disk format/finalize sequence the
// SD retry queue uses -- there's no separate "save on failure" step; a
// failed upload just means this file, already correctly formatted, stays
// where flush_pending_voice_notes() will find it on a later wake. Handles
// its own SD mount, FIFO eviction, and "RECORDING FAILED" e-ink feedback
// on any early-exit path; have_pending_file stays false whenever there's
// nothing worth uploading (SD unavailable, header write failed, or a
// zero-byte take).
static ptt_recording_result_t record_ptt_take_to_pending_file(bool have_live_checkin) {
    ptt_recording_result_t result = {};

    if (!ensure_sdcard_mounted()) {
        ESP_LOGE(TAG, "push-to-talk: SD card unavailable, cannot record");
        eink_show_message("RECORDING FAILED");
        return result;
    }

    mkdir(PENDING_VOICE_DIR, 0755); // ignore EEXIST -- already-there is the common case

    uint32_t seqs[PENDING_VOICE_MAX_QUEUED];
    int count = pending_voice_list_sorted(seqs, PENDING_VOICE_MAX_QUEUED);
    if (count >= PENDING_VOICE_MAX_QUEUED) {
        char evict_path[64];
        pending_voice_path_for_seq(seqs[0], evict_path, sizeof(evict_path));
        remove(evict_path);
    }
    uint32_t new_seq = (count > 0) ? seqs[count - 1] + 1 : 1;
    pending_voice_path_for_seq(new_seq, result.pending_path, sizeof(result.pending_path));

    FILE *f = fopen(result.pending_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "push-to-talk: failed to open %s for recording", result.pending_path);
        eink_show_message("RECORDING FAILED");
        return result;
    }

    pending_voice_header_t hdr = {};
    // magic left all-zero until the recording finishes normally -- see
    // pending_voice_header_t's comment for why.
    hdr.format_version = 1;
    hdr.sample_rate = PTT_SAMPLE_RATE_HZ;
    hdr.bits_per_sample = 16;
    hdr.channels = 1;
    hdr.mono_bytes = 0;
    if (have_live_checkin) {
        strncpy(hdr.checkin_id, s_last_screen.checkin_id, sizeof(hdr.checkin_id) - 1);
        hdr.checkin_id[sizeof(hdr.checkin_id) - 1] = '\0';
    }
    bool header_ok = fwrite(&hdr, sizeof(hdr), 1, f) == 1;
    if (!header_ok) {
        fclose(f);
        remove(result.pending_path);
        return result;
    }

    if (have_live_checkin) {
        eink_show_checkin_recording(s_last_screen.checkin_prompt);
    } else {
        eink_show_message("RECORDING... PRESS BOOT TO STOP");
    }

    size_t recorded_bytes = record_until_stopped(f);
    double recorded_seconds = (double)recorded_bytes / (PTT_SAMPLE_RATE_HZ * PTT_BYTES_PER_SAMPLE);
    ESP_LOGI(TAG, "push-to-talk: recorded %u mono bytes (%.1fs)", (unsigned)recorded_bytes, recorded_seconds);

    if (recorded_bytes == 0) {
        fclose(f);
        remove(result.pending_path);
        return result;
    }

    // Finalize: patch magic + real byte count now that they're known. A
    // file left with all-zero magic (e.g. power cut between here and the
    // write completing) is unambiguously incomplete to
    // flush_pending_voice_notes().
    memcpy(hdr.magic, PENDING_VOICE_MAGIC, sizeof(hdr.magic));
    hdr.mono_bytes = (uint32_t)recorded_bytes;
    fseek(f, 0, SEEK_SET);
    bool finalize_ok = fwrite(&hdr, sizeof(hdr), 1, f) == 1;
    fclose(f);
    if (finalize_ok) {
        result.have_pending_file = true;
        result.pending_mono_bytes = (uint32_t)recorded_bytes;
    } else {
        remove(result.pending_path);
    }
    return result;
}

// Uploads a just-recorded pending voice note (fire-and-forget wifi
// connect -> upload -> SENT/SAVED FOR RETRY feedback), then decides how to
// leave the screen: a check-in reply invalidates s_last_screen so the next
// real sync brings fresh content instead of redrawing an already-answered
// prompt; a plain recording restores the normal screen after a few
// seconds so the confirmation is actually visible first.
static void upload_and_finish_ptt_take(const ptt_recording_result_t &rec, bool have_live_checkin, int battery_pct) {
    // Immediate feedback that the stop (silence or button) actually
    // registered -- found via hardware testing that wifi connect +
    // upload can take 20s+, and staying on "RECORDING..." that whole
    // time looks exactly like the stop didn't work.
    eink_show_message("SENDING...");

    bool wifi_ok = wifi_connect();
    bool upload_ok = false;
    if (wifi_ok) {
        // F7: tag this reply to the live check-in, if one's showing,
        // so the backend routes it to that check-in instead of
        // resolving it by keyword/recency.
        upload_ok = upload_voice_note_from_file(rec.pending_path, rec.pending_mono_bytes,
                                                 have_live_checkin ? s_last_screen.checkin_id : nullptr);
    } else {
        ESP_LOGE(TAG, "push-to-talk: skipping upload -- wifi never connected");
    }

    if (upload_ok) {
        remove(rec.pending_path);
    }
    eink_show_message(upload_ok ? "SENT" : "SAVED FOR RETRY");

    if (have_live_checkin) {
        // This reply resolves the check-in -- the persisted screen
        // now describes something already answered, so redrawing it
        // would make an already-replied-to prompt look like it's
        // still pending. Leave the confirmation up instead (same
        // reasoning as run_checkin_skip_cycle()); the next real sync
        // brings fresh content.
        s_last_screen.valid = false;
    } else {
        // Give the user a moment to actually see the confirmation
        // before restoring the normal screen underneath it.
        vTaskDelay(pdMS_TO_TICKS(3000));
        eink_render_last_known(battery_pct, nullptr);
    }
}

static void run_push_to_talk_cycle(void) {
    ESP_LOGI(TAG, "push-to-talk: BOOT wake, starting voice cycle");

    // Tap vs hold only needs a GPIO read, no board power yet.
    bool is_hold = wait_for_tap_or_hold(BOOT_BUTTON_PIN);
    if (is_hold) {
        // Whether we end up skipping or falling through to record (no
        // live check-in to skip), either path needs a clean released
        // state before touching the button component again -- same
        // false-positive-click concern record_until_stopped() guards
        // against for its own stop detection. Bounded so a genuinely
        // stuck button can't hang the device forever.
        int wait_ms = 0;
        while (gpio_get_level(BOOT_BUTTON_PIN) == 0 && wait_ms < 5000) {
            vTaskDelay(pdMS_TO_TICKS(50));
            wait_ms += 50;
        }
    }
    bool have_live_checkin = s_last_screen.valid && s_last_screen.is_checkin;

    // Power (VBAT/EPD) and the settle delay already happened in
    // app_main() before this function was even called -- see the fix
    // there for why that ordering matters.

    if (is_hold && have_live_checkin) {
        run_checkin_skip_cycle();
        enter_deep_sleep();
        return;
    }

    BoardPower_Audio_ON();
    BoardAdc_Init();
    int battery_pct = Get_Batterylevel();

    // Codec_StartInit() expects the shared I2C bus already up (the
    // normal cycle gets this for free via its own RTC/SHTC3 init before
    // ever touching audio; found missing here via hardware testing --
    // codec_board logged a harmless-but-confusing "port has not been
    // initialized" error before self-recovering).
    I2cMasterBus::requestInstance(ESP32_I2C_SCL_PIN, ESP32_I2C_SDA_PIN, ESP32_I2C_DEV_NUM);
    Codec_StartInit();

    ptt_recording_result_t rec = record_ptt_take_to_pending_file(have_live_checkin);
    if (rec.have_pending_file) {
        upload_and_finish_ptt_take(rec, have_live_checkin, battery_pct);
    }

    // The regular sync cadence is untouched here -- no
    // rtc_schedule_alarm() call, the alarm from the last real sync cycle
    // is left exactly as it was. enter_deep_sleep() already powers down
    // audio/EPD itself.
    enter_deep_sleep();
}

// ---------------------------------------------------------------------

extern "C" void app_main(void) {
    // Wires eink_ui's render paths back to the SD-card pending-voice-queue
    // scan (draw_pending_voice_indicator(), defined below in the SD-card
    // section) -- must happen before any eink_render*/draw_*_screen call,
    // so do it first, before anything else in this function.
    eink_ui_set_pending_voice_indicator_cb(draw_pending_voice_indicator);

    s_boot_time_us = esp_timer_get_time();

    // F8/bug fix: this must be the literal first thing app_main does --
    // before NVS init, before installing the SD-log hook, before
    // anything. This board's PWR button is very likely a momentary
    // self-latching power switch, not a plain always-on GPIO input: the
    // original Waveshare reference example (12_RTC_Sleep_Test) explicitly
    // cuts VBAT_POWER_OFF() when it detects a PWR-triggered wake,
    // implying the *only* thing keeping the board powered past the
    // button's initial physical pulse is firmware asserting this GPIO
    // itself, quickly. Any delay here risks the board losing power on
    // battery alone right as the user releases the button -- completely
    // masked whenever USB is plugged in (which supplies power directly,
    // regardless of this GPIO), which is exactly how this went unnoticed
    // through this whole session's testing. Release the sleep-time hold
    // before driving the pin again, matching the pin's own hold/release
    // pairing.
    rtc_gpio_hold_dis((gpio_num_t)VBAT_PWR_PIN);
    BoardPower_Init();
    BoardPower_VBAT_ON();
    BoardPower_EPD_ON();

    // Instant visible confirmation the device is executing code --
    // replaces an earlier e-ink splash message here, which took ~2s (the
    // panel's fixed full-refresh time) and would have corrupted
    // wait_for_tap_or_hold()'s timing below if left in this spot.
    led_set(true);

    // Installed right after the power latch above, not before --
    // SD-card logging tries a fresh mount attempt on *every* log call
    // until it succeeds (see ensure_sdcard_mounted()'s comment), and the
    // SD card has no power until BoardPower_VBAT_ON() just ran. Any log
    // line issued before that point would previously have added its own
    // mount-attempt delay before the power latch -- exactly the kind of
    // self-inflicted delay this whole reordering is fixing. Gated by
    // SD_DEBUG_LOG_ENABLED: when off, logging falls back to plain console
    // output only, skipping the SD mount/write cost per log line entirely.
#if SD_DEBUG_LOG_ENABLED
    esp_log_set_vprintf(sd_log_vprintf);
#endif

    // NVS init, per standard ESP-IDF convention -- required before wifi,
    // and independent of every other subsystem below.
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    vTaskDelay(pdMS_TO_TICKS(100));

    const char *reset_reason = reset_reason_to_string(esp_reset_reason());
    const char *wake_reason = wake_reason_to_string();
    ESP_LOGI(TAG, "boot: reset_reason=%s wake_reason=%s", reset_reason, wake_reason);

    // F6: BOOT is dedicated to push-to-talk and takes a completely
    // separate path -- power is already latched above either way, so
    // run_push_to_talk_cycle() no longer needs its own power-on calls.
    // Never returns (ends in its own enter_deep_sleep() call).
    if (wake_was_boot_button()) {
        run_push_to_talk_cycle();
    }

    // Bug fix: PWR tap still means "sync now" (unchanged, falls through
    // to the normal cycle below); a held PWR press instead means "power
    // off" -- see run_power_off_cycle()'s comment. Checked here, not
    // before the BOOT check above, since only one of BOOT/PWR can have
    // triggered this particular wake anyway.
    if (wake_was_pwr_button()) {
        bool pwr_is_hold = wait_for_tap_or_hold(PWR_BUTTON_PIN);
        if (pwr_is_hold) {
            run_power_off_cycle();
            // never returns
        }
    }

    // TZ env state lives in normal RAM and does not survive deep sleep;
    // re-apply the last-known value every wake so localtime()/log
    // timestamps stay correct even on a cycle whose sync attempt fails.
    sync_tz_apply(s_tz_posix, "UTC0");

    // I2C bus is shared by the RTC and the SHTC3 sensor (the audio codec
    // manages its own separate I2C init internally). Brought up here,
    // ahead of the F9 reminder-only branch below, since that branch still
    // needs the RTC (to read "now" and reschedule the alarm) even though
    // it skips everything else in this section.
    I2cMasterBus *i2c_bus = I2cMasterBus::requestInstance(ESP32_I2C_SCL_PIN, ESP32_I2C_SDA_PIN, ESP32_I2C_DEV_NUM);
    pcf85063a_dev_t rtc_dev;
    ESP_ERROR_CHECK(pcf85063a_init(&rtc_dev, i2c_bus->Get_I2cBusHandle(), PCF85063A_ADDRESS));

    // F9: a "timer" wake scheduled specifically for an upcoming reminder
    // skips wifi/sync entirely -- just chime and reschedule. Distinguished
    // from a normal poll-interval "timer" wake via
    // s_next_wake_is_reminder_only, since the PCF85063 has only one
    // hardware alarm register and can't tell firmware *why* it fired.
    // Never returns if taken.
    if (strcmp(wake_reason, "timer") == 0 && s_next_wake_is_reminder_only) {
        handle_reminder_only_wake(&rtc_dev);
    }

    Shtc3_Init(i2c_bus); // only the full cycle below needs temp/humidity

    // Read before wifi/sync, not after -- F1 (PROJECT_PLAN.md) wants
    // battery_mv in the sync request body itself, not just logged
    // afterward.
    BoardAdc_Init();
    float vbat = Get_VbatVoltage();
    int battery_mv = (int)(vbat * 1000.0f + 0.5f);
    uint8_t battery_pct = Get_Batterylevel();
    ESP_LOGI(TAG, "battery: %.3fV (%.0f%%)", vbat, (double)battery_pct);
    log_reset_history(reset_reason, wake_reason, vbat, battery_pct);

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
        // 32KiB, up from a prior 8192 -- that was tight even before this
        // feature (8 checkins/reminders/calendar_events at up to 256
        // bytes of text each can already approach ~9-10KB combined), and
        // silently truncated rather than erroring (see the `truncated`
        // flag on http_response_buf_t below). Buffer is PSRAM-backed and
        // freed right after this cycle's sync attempt, so the extra size
        // costs nothing real.
        const size_t resp_capacity = 32768;
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
            // Only recorded here, not also inside device_sync_attempt's
            // truncation branch -- that path already records its own
            // more specific "sync_response_truncated" error, and the
            // single-slot pending state would just overwrite it with
            // this more generic one if both ran.
            if (!resp.truncated) {
                char msg[64];
                snprintf(msg, sizeof(msg), "status %d after retries", http_status);
                record_device_error("sync_failed", msg);
            }
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

    // Flush now that wifi's state for this cycle is known -- catches
    // whatever's pending from this cycle's own sync_failed/truncated
    // above, or from an earlier network-free reminder-only wake (F9).
    // No-op (and no wifi assumption needed) if nothing is pending.
    if (wifi_ok) {
        flush_pending_device_error(reset_reason, wake_reason, battery_mv, battery_pct);
        flush_pending_voice_notes();
    }

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

    // F8: on a failed cycle, still redraw (from the persisted summary,
    // not a fresh snapshot) with a "SYNC FAILED" notice -- CLAUDE.md's
    // "a sync failure or stale display is a degraded UX, never a missed
    // reminder" is about not treating a stale display as catastrophic,
    // not about hiding the fact that something's actually wrong. Found
    // via F8 hardware testing that silently leaving the old screen up
    // with zero indication gives no visible signal of a real wifi/backend
    // problem.
    sync_soonest_reminder_t soonest_reminder = {};
    bool reminder_activated = false;
    if (have_fresh_snapshot) {
        // F9: computed (and, on a timer wake, acted on) *before* rendering
        // -- eink_render() needs to know whether a reminder just activated
        // so it can override a live check-in on screen with it, and
        // chiming/dedup-marking has to happen exactly once, not split
        // across a pre-render and post-render pass.
        soonest_reminder = sync_find_soonest_reminder(snap);
        if (strcmp(wake_reason, "timer") == 0) {
            reminder_activated = handle_due_reminder(soonest_reminder);
        }

        eink_render(*snap, battery_pct, reminder_activated ? &soonest_reminder : nullptr);

        // F9: re-cache reminders (id+message+due_at) for reminder-only
        // wakes between now and the next full sync.
        if (snap->reminders_valid) {
            s_pending_reminders_count = snap->reminders_count < SYNC_MAX_REMINDERS ? snap->reminders_count : SYNC_MAX_REMINDERS;
            for (int i = 0; i < s_pending_reminders_count; i++) {
                strncpy(s_pending_reminders[i].id, snap->reminders[i].id, sizeof(s_pending_reminders[i].id) - 1);
                s_pending_reminders[i].id[sizeof(s_pending_reminders[i].id) - 1] = '\0';
                strncpy(s_pending_reminders[i].message, snap->reminders[i].message, sizeof(s_pending_reminders[i].message) - 1);
                s_pending_reminders[i].message[sizeof(s_pending_reminders[i].message) - 1] = '\0';
                s_pending_reminders[i].due_at = snap->reminders[i].due_at;
            }
        } else {
            s_pending_reminders_count = 0;
        }
    } else {
        ESP_LOGW(TAG, "sync failed this cycle -- showing last-known screen with a SYNC FAILED notice");
        eink_render_last_known(battery_pct, "SYNC FAILED");
        // s_pending_reminders/s_next_full_sync_at deliberately left
        // untouched on failure -- keep using the last known-good reminder
        // timing rather than discarding it, matching this file's existing
        // last-known-good pattern for s_last_known_good_poll_interval.
    }
    if (snap) {
        heap_caps_free(snap);
    }

    int effective_poll_interval = sync_effective_poll_interval(
        s_has_synced_ever, s_last_known_good_poll_interval, FALLBACK_POLL_INTERVAL_SECONDS);
    if (have_fresh_snapshot) {
        s_next_full_sync_at = time(nullptr) + effective_poll_interval;
    }

    schedule_next_wake_and_sleep(&rtc_dev); // never returns
}
