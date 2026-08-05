#include "eink_ui.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include <esp_attr.h>
#include <esp_log.h>

#include "port_display.h"
#include "epaper_config.h"
#include "eink_lvgl.h"

static const char *TAG = "eink_ui";

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

int draw_text(const char *s, int x0, int y0, int scale, uint8_t color) {
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

static void format_local_hhmm(int64_t epoch, char *buf, size_t buf_len) {
    time_t t = (time_t)epoch;
    struct tm local_tm;
    localtime_r(&t, &local_tm);
    snprintf(buf, buf_len, "%02d:%02d", local_tm.tm_hour, local_tm.tm_min);
}

static bool is_same_local_day(int64_t epoch_a, int64_t epoch_b) {
    time_t ta = (time_t)epoch_a, tb = (time_t)epoch_b;
    struct tm tma, tmb;
    localtime_r(&ta, &tma);
    localtime_r(&tb, &tmb);
    return tma.tm_year == tmb.tm_year && tma.tm_yday == tmb.tm_yday;
}

// F8: notice is non-null only when this cycle's sync failed -- drawn in
// place of the plain divider line (same row, y=22) rather than adding a
// new one, since a scale-1 text line (7px tall) fits in the same space
// without disturbing anything above (status bar text, y=4-18) or below
// (cloud+rain row at y=28, sunrise/sunset row at y=37, second divider at
// y=48, body content starts at y=54).
static void draw_status_bar(bool has_weather, const sync_weather_t &weather, int battery_pct, const char *notice) {
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
    if (has_weather) {
        char temp_buf[8];
        snprintf(temp_buf, sizeof(temp_buf), "%.0fC", (double)weather.temperature_c);
        int temp_w = (int)strlen(temp_buf) * (FONT_GLYPH_W + 1) * 2;
        draw_text(temp_buf, (EPD_WIDTH - temp_w) / 2, 4, 2, DRIVER_COLOR_BLACK);
    }

    if (notice) {
        draw_text(notice, 8, 22, 1, DRIVER_COLOR_BLACK);
    } else {
        for (int x = 4; x < EPD_WIDTH - 4; x++) {
            EPD_DrawColorPixel(x, 22, DRIVER_COLOR_BLACK);
        }
    }

    // Second/third header rows: cloud cover + rain, then sunrise/sunset --
    // same on every screen (check-in included) since it's status-bar-level
    // info now, not dashboard-only.
    if (has_weather) {
        char cloud_buf[24];
        snprintf(cloud_buf, sizeof(cloud_buf), "CLOUD %d%%  RAIN %.1fMM", weather.cloud_cover_pct, weather.precipitation_mm);
        draw_text(cloud_buf, 8, 28, 1, DRIVER_COLOR_BLACK);

        char sunrise_buf[6], sunset_buf[6];
        format_local_hhmm(weather.sunrise, sunrise_buf, sizeof(sunrise_buf));
        format_local_hhmm(weather.sunset, sunset_buf, sizeof(sunset_buf));
        char sun_buf[20];
        snprintf(sun_buf, sizeof(sun_buf), "SUN %s-%s", sunrise_buf, sunset_buf);
        draw_text(sun_buf, 8, 37, 1, DRIVER_COLOR_BLACK);
    }

    for (int x = 4; x < EPD_WIDTH - 4; x++) {
        EPD_DrawColorPixel(x, 48, DRIVER_COLOR_BLACK);
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

static void draw_checkin(const char *prompt_text) {
    draw_text("CHECK-IN", 8, 54, 2, DRIVER_COLOR_BLACK);
    // Body text at scale 1 (not 2) -- prompt_text can run to a full
    // sentence or two, and scale 2 didn't leave enough room to fit it
    // without truncating. Scale 1 fits ~30 chars/line x 14 lines (420
    // chars) well over the 256-char field cap, so truncation shouldn't
    // happen in practice anymore. Capped at 14 (not 15) so the last line
    // can't land past y=200 (screen bottom) and get silently dropped.
    draw_wrapped_text(prompt_text, 8, 74, EPD_WIDTH - 16, 1, DRIVER_COLOR_BLACK, 14);
}

// Soonest upcoming reminder or calendar event, whichever is sooner -- a
// single combined "next thing" rather than two separate lists, since
// there's limited vertical space to spend on it. Factored out of
// draw_dashboard() so the same computed result can also be persisted for
// eink_render_last_known() below, without needing the raw
// reminders/calendar_events arrays (which aren't RTC_DATA_ATTR-friendly
// -- sync_snapshot_t is ~8.4KB, far more than the RTC slow memory budget).
static next_item_t find_next_item(const sync_snapshot_t &snap) {
    next_item_t result = {};
    time_t now = time(nullptr);

    if (snap.reminders_valid) {
        for (int i = 0; i < snap.reminders_count; i++) {
            if (!is_same_local_day(snap.reminders[i].due_at, now)) {
                continue;
            }
            if (!result.have_next || snap.reminders[i].due_at < result.at) {
                result.have_next = true;
                result.at = snap.reminders[i].due_at;
                strncpy(result.label, snap.reminders[i].message, sizeof(result.label) - 1);
                result.label[sizeof(result.label) - 1] = '\0';
                result.is_event = false;
            }
        }
    }
    if (snap.calendar_events_valid) {
        for (int i = 0; i < snap.calendar_events_count; i++) {
            if (!is_same_local_day(snap.calendar_events[i].start, now)) {
                continue;
            }
            if (!result.have_next || snap.calendar_events[i].start < result.at) {
                result.have_next = true;
                result.at = snap.calendar_events[i].start;
                strncpy(result.label, snap.calendar_events[i].summary, sizeof(result.label) - 1);
                result.label[sizeof(result.label) - 1] = '\0';
                result.is_event = true;
            }
        }
    }
    return result;
}

static void draw_dashboard(const next_item_t &next) {
    int y = 54;

    if (next.have_next) {
        char time_buf[6];
        format_local_hhmm(next.at, time_buf, sizeof(time_buf));
        char header[24];
        snprintf(header, sizeof(header), "%s %s:", next.is_event ? "EVENT" : "NEXT", time_buf);
        draw_text(header, 8, y, 2, DRIVER_COLOR_BLACK);
        y += 20;
        draw_wrapped_text(next.label, 8, y, EPD_WIDTH - 16, 2, DRIVER_COLOR_BLACK, 4);
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

void eink_ensure_initialized(void) {
    if (!s_eink_initialized_this_boot) {
        PortDisplay_Init();
        EPD_Init();
        s_eink_initialized_this_boot = true;
    }
}

// Remembers just enough of the last real sync render to redraw the same
// screen later without a fresh sync (F6's push-to-talk cycle wants to
// return to "whatever was on screen before" after SENT/UPLOAD FAILED,
// but e-ink has no undo -- once EPD_Display() overwrites it, the old
// image is gone unless we redraw it from data we still have). Only the
// final computed display values are kept, not the raw snapshot (which
// wouldn't fit in RTC slow memory at ~8.4KB).
RTC_DATA_ATTR last_screen_t s_last_screen = {};

// Bottom-right "N/15" queued-voice-note indicator -- lives in
// main/adhi-firmware.cpp (needs ensure_sdcard_mounted()/
// pending_voice_list_sorted(), voice-note-queue internals that don't
// belong in this component), invoked here through a callback registered
// once at boot via eink_ui_set_pending_voice_indicator_cb().
static void (*s_pending_voice_cb)(void) = nullptr;

void eink_ui_set_pending_voice_indicator_cb(void (*cb)(void)) {
    s_pending_voice_cb = cb;
}

static void draw_pending_voice_indicator(void) {
    if (s_pending_voice_cb) {
        s_pending_voice_cb();
    }
}

// Shared by eink_render()'s reminder-override branch and
// handle_reminder_only_wake() (the two places a reminder that just
// activated takes over the screen): clear, status bar, dashboard on the
// reminder, queued-voice indicator, flip. Owns EPD_Clear/EPD_Display
// itself since both callers always want the whole screen replaced, never
// a partial redraw.
void draw_reminder_override_screen(bool has_weather, const sync_weather_t &weather, int battery_pct,
                                    const next_item_t &reminder_item) {
    eink_ensure_initialized();
    EPD_Clear();
    draw_status_bar(has_weather, weather, battery_pct, nullptr);
    draw_dashboard(reminder_item);
    draw_pending_voice_indicator();
    EPD_Display();
}

// F9: override_reminder is non-null only when handle_due_reminder() just
// fired this cycle -- a reminder that just activated always takes over
// the screen from a live check-in, unconditionally. Built directly from
// override_reminder rather than reused from find_next_item()'s own scan
// (which merges reminders+calendar events and could in principle land on
// a different item) so what's drawn always matches what actually chimed.
void eink_render(const sync_snapshot_t &snap, int battery_pct, const sync_soonest_reminder_t *override_reminder) {
    const sync_checkin_t *live_checkin = nullptr;
    next_item_t next = {};
    next_item_t reminder_item = {};
    const char *render_kind;

    if (override_reminder) {
        reminder_item.have_next = true;
        reminder_item.is_event = false;
        reminder_item.at = override_reminder->due_at;
        strncpy(reminder_item.label, override_reminder->message, sizeof(reminder_item.label) - 1);
        reminder_item.label[sizeof(reminder_item.label) - 1] = '\0';

        draw_reminder_override_screen(snap.has_weather, snap.weather, battery_pct, reminder_item);
        render_kind = "reminder-override";
    } else {
        live_checkin = find_live_checkin(snap);
        next = find_next_item(snap);

        eink_ensure_initialized();
        EPD_Clear();
        draw_status_bar(snap.has_weather, snap.weather, battery_pct, nullptr);

        if (live_checkin) {
            draw_checkin(live_checkin->prompt_text);
            // No indicator here -- a live check-in deliberately takes over the
            // whole screen (see this function's header comment), and its
            // wrapped prompt text can already run all the way to the bottom
            // edge (draw_checkin()'s own comment on its 14-line cap).
            render_kind = "check-in";
        } else {
            eink_lvgl_draw_dashboard(next.have_next, next.is_event, next.at, next.label);
            draw_pending_voice_indicator();
            render_kind = "dashboard";
        }

        EPD_Display();
    }
    ESP_LOGI(TAG, "e-ink render done (%s)", render_kind);

    s_last_screen.valid = true;
    s_last_screen.is_checkin = (!override_reminder && live_checkin != nullptr);
    if (s_last_screen.is_checkin) {
        strncpy(s_last_screen.checkin_prompt, live_checkin->prompt_text, sizeof(s_last_screen.checkin_prompt) - 1);
        s_last_screen.checkin_prompt[sizeof(s_last_screen.checkin_prompt) - 1] = '\0';
        strncpy(s_last_screen.checkin_id, live_checkin->id, sizeof(s_last_screen.checkin_id) - 1);
        s_last_screen.checkin_id[sizeof(s_last_screen.checkin_id) - 1] = '\0';
    }
    s_last_screen.has_weather = snap.has_weather;
    s_last_screen.weather = snap.weather;
    s_last_screen.next = override_reminder ? reminder_item : next;
}

// F6/F8: redraws whatever eink_render() last actually drew, using the
// persisted summary above -- no wifi/sync involved. Used after a
// push-to-talk cycle finishes (notice is null), so the display goes
// back to the normal screen instead of sitting on "SENT"/"UPLOAD FAILED"
// until the next real sync; and after a failed sync cycle (notice is
// "SYNC FAILED"), so a wifi/backend problem is actually visible on the
// device instead of just silently leaving the stale screen up with no
// signal anything's wrong. Renders even with no persisted screen yet
// (e.g. the very first-ever sync fails before anything has ever
// succeeded) -- still shows the status bar/notice, just no body content.
void eink_render_last_known(int battery_pct, const char *notice) {
    eink_ensure_initialized();
    EPD_Clear();

    bool has_weather = s_last_screen.valid && s_last_screen.has_weather;
    draw_status_bar(has_weather, s_last_screen.weather, battery_pct, notice);

    if (s_last_screen.valid) {
        if (s_last_screen.is_checkin) {
            draw_checkin(s_last_screen.checkin_prompt);
        } else {
            eink_lvgl_draw_dashboard(s_last_screen.next.have_next, s_last_screen.next.is_event,
                                      s_last_screen.next.at, s_last_screen.next.label);
            draw_pending_voice_indicator();
        }
    } else {
        draw_pending_voice_indicator();
    }

    EPD_Display();
    ESP_LOGI(TAG, "e-ink render done (%s%s)",
             s_last_screen.valid ? (s_last_screen.is_checkin ? "restored check-in" : "restored dashboard") : "blank, no last-known screen yet",
             notice ? ", with notice" : "");
}

// F6 (PROJECT_PLAN.md): simple full-refresh status message, reusing the
// same font/wrap helpers as the snapshot render above -- used by the
// push-to-talk cycle ("RECORDING...", "SENT", "UPLOAD FAILED"), which
// doesn't have a fresh snapshot to render (it deliberately skips wifi
// until after recording finishes).
void eink_show_message(const char *message) {
    eink_ensure_initialized();
    EPD_Clear();
    draw_wrapped_text(message, 8, 80, EPD_WIDTH - 16, 2, DRIVER_COLOR_BLACK, 4);
    EPD_Display();
    ESP_LOGI(TAG, "e-ink message shown: %s", message);
}

// F7: recording a reply to a live check-in keeps the same layout
// draw_checkin() uses (same header position, same prompt placement at
// scale 1) so the prompt stays put on screen instead of being replaced
// by a generic "RECORDING..." message -- the user should still be able
// to see what they're replying to while talking.
void eink_show_checkin_recording(const char *prompt_text) {
    eink_ensure_initialized();
    EPD_Clear();
    draw_text("REPLYING...", 8, 54, 2, DRIVER_COLOR_BLACK);
    draw_wrapped_text(prompt_text, 8, 74, EPD_WIDTH - 16, 1, DRIVER_COLOR_BLACK, 14);
    EPD_Display();
    ESP_LOGI(TAG, "e-ink message shown: replying to check-in");
}
