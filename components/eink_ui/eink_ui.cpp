#include "eink_ui.h"

#include <algorithm>
#include <cstring>
#include <ctime>

#include <esp_attr.h>
#include <esp_log.h>

#include "port_display.h"
#include "ui_screens.h"

static const char *TAG = "eink_ui";

// ---------------------------------------------------------------------
// F4 (PROJECT_PLAN.md): render the sync snapshot. A live check-in (spec
// section 4.3: "the one thing on the display that's actionable") takes
// over the whole screen; otherwise a compact dashboard (weather +
// soonest upcoming reminder/calendar event). Battery percentage and
// local time are always shown in the status bar. Each content section
// degrades independently per spec section 5 -- a missing/invalid field
// is just omitted, not flagged as an error on-screen. All actual widget
// layout lives in ui_screens.cpp (LVGL); this file only ever assembles a
// device_ui_state_t and decides which ui_screens_render_*() to call.
// ---------------------------------------------------------------------

static bool is_same_local_day(int64_t epoch_a, int64_t epoch_b) {
    time_t ta = (time_t)epoch_a, tb = (time_t)epoch_b;
    struct tm tma, tmb;
    localtime_r(&ta, &tma);
    localtime_r(&tb, &tmb);
    return tma.tm_year == tmb.tm_year && tma.tm_yday == tmb.tm_yday;
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

// Soonest upcoming reminder or calendar event goes in `next`; everything
// else that's still today goes in `remaining`, soonest-first -- a single
// same-day scan feeds both the always-visible "next up" line and the
// dashboard's lower section (rest of today's items, shown whenever there's
// no live check-in). `remaining` is deliberately not persisted anywhere
// (unlike `next`, see eink_render_last_known() below) -- the raw
// reminders/calendar_events arrays aren't RTC_DATA_ATTR-friendly
// (sync_snapshot_t is ~8.4KB, far more than the RTC slow memory budget),
// and a redraw-from-last-known cycle showing just "next up" with no lower
// list is an acceptable approximation, not a fresh render.
//
// Sorts directly into `remaining.items` (exactly UI_MAX_REMAINING_ITEMS
// slots -- the true worst case of every reminder+event landing today) as
// scratch space instead of a second same-size array, then drops the
// soonest element with one memmove -- avoids a duplicate ~1KB stack array
// on top of the caller's own copy. Combined with eink_render() writing
// straight into its device_ui_state_t (no separate local `remaining`
// anymore), this struct is now built and read in place, never copied.
static void find_today_items(const sync_snapshot_t &snap, next_item_t &next, remaining_items_t &remaining) {
    next = {};
    remaining = {};
    time_t now = time(nullptr);

    sorted_item_t *todays = remaining.items;
    int count = 0;

    if (snap.reminders_valid) {
        for (int i = 0; i < snap.reminders_count && count < UI_MAX_REMAINING_ITEMS; i++) {
            if (!is_same_local_day(snap.reminders[i].due_at, now)) {
                continue;
            }
            todays[count].is_event = false;
            todays[count].at = snap.reminders[i].due_at;
            strncpy(todays[count].label, snap.reminders[i].message, sizeof(todays[count].label) - 1);
            todays[count].label[sizeof(todays[count].label) - 1] = '\0';
            count++;
        }
    }
    if (snap.calendar_events_valid) {
        for (int i = 0; i < snap.calendar_events_count && count < UI_MAX_REMAINING_ITEMS; i++) {
            if (!is_same_local_day(snap.calendar_events[i].start, now)) {
                continue;
            }
            todays[count].is_event = true;
            todays[count].at = snap.calendar_events[i].start;
            strncpy(todays[count].label, snap.calendar_events[i].summary, sizeof(todays[count].label) - 1);
            todays[count].label[sizeof(todays[count].label) - 1] = '\0';
            count++;
        }
    }

    std::sort(todays, todays + count, [](const sorted_item_t &a, const sorted_item_t &b) { return a.at < b.at; });

    if (count > 0) {
        next.have_next = true;
        next.is_event = todays[0].is_event;
        next.at = todays[0].at;
        strncpy(next.label, todays[0].label, sizeof(next.label) - 1);
        next.label[sizeof(next.label) - 1] = '\0';
    }
    remaining.count = count > 0 ? count - 1 : 0;
    if (remaining.count > 0) {
        memmove(todays, todays + 1, (size_t)remaining.count * sizeof(sorted_item_t));
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

// Footer strip (bottom-right "N/15" queued-voice-note badge + bottom-left
// notice) -- the badge count lives in main/adhi-firmware.cpp (needs
// ensure_sdcard_mounted()/pending_voice_list_sorted(), voice-note-queue
// internals that don't belong in this component), invoked here through a
// callback registered once at boot via eink_ui_set_pending_voice_indicator_cb().
// notice is forwarded straight through -- only eink_render_last_known()
// ever has one non-null.
static void (*s_pending_voice_cb)(const char *notice) = nullptr;

void eink_ui_set_pending_voice_indicator_cb(void (*cb)(const char *notice)) {
    s_pending_voice_cb = cb;
}

static void draw_footer(const char *notice) {
    if (s_pending_voice_cb) {
        s_pending_voice_cb(notice);
    }
}

void eink_ui_draw_pending_voice_badge(int count, int max_count, const char *notice) {
    ui_screens_render_footer(count, max_count, notice);
}

// Shared by eink_render()'s reminder-override branch and
// handle_reminder_only_wake() (the two places a reminder that just
// activated takes over the screen): clear, status bar + dashboard on the
// reminder, queued-voice indicator, flip. Owns EPD_Clear/EPD_Display
// itself since both callers always want the whole screen replaced, never
// a partial redraw.
void draw_reminder_override_screen(bool has_weather, const sync_weather_t &weather, int battery_pct,
                                    const next_item_t &reminder_item) {
    eink_ensure_initialized();
    EPD_Clear();

    device_ui_state_t state = {};
    state.battery_pct = battery_pct;
    state.has_weather = has_weather;
    state.weather = weather;
    state.next = reminder_item;
    ui_screens_render_dashboard(state);
    draw_footer(nullptr); // F9 override never has a sync-failure notice to show

    EPD_Display();
}

// F9: override_reminder is non-null only when handle_due_reminder() just
// fired this cycle -- a reminder that just activated always takes over
// the screen from a live check-in, unconditionally. Built directly from
// override_reminder rather than reused from find_today_items()'s own scan
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

        eink_ensure_initialized();
        EPD_Clear();

        // find_today_items() writes state.next/state.remaining directly --
        // no separate local to copy out of afterwards (remaining_items_t is
        // the one struct here big enough to matter on an 8KB task stack).
        device_ui_state_t state = {};
        state.battery_pct = battery_pct;
        state.has_weather = snap.has_weather;
        state.weather = snap.weather;
        find_today_items(snap, state.next, state.remaining);
        next = state.next;
        if (live_checkin) {
            state.checkin_prompt = live_checkin->prompt_text;
        }
        ui_screens_render_dashboard(state);
        draw_footer(nullptr); // a freshly successful sync never has a notice
        render_kind = "dashboard";

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

    device_ui_state_t state = {};
    state.battery_pct = battery_pct;
    state.has_weather = s_last_screen.valid && s_last_screen.has_weather;
    state.weather = s_last_screen.weather;

    if (s_last_screen.valid) {
        state.next = s_last_screen.next;
        if (s_last_screen.is_checkin) {
            state.checkin_prompt = s_last_screen.checkin_prompt;
        }
        // remaining-items list isn't persisted across deep sleep (see
        // find_today_items()'s comment) -- this restore stays "next up"
        // only, blank lower list, when there's no live check-in.
    }
    ui_screens_render_dashboard(state); // blank status bar + blank content if s_last_screen was never valid
    draw_footer(notice); // the only caller that ever passes a non-null notice

    EPD_Display();
    ESP_LOGI(TAG, "e-ink render done (%s%s)",
             s_last_screen.valid ? (s_last_screen.is_checkin ? "restored check-in" : "restored dashboard") : "blank, no last-known screen yet",
             notice ? ", with notice" : "");
}

// F6 (PROJECT_PLAN.md): simple full-refresh status message -- used by the
// push-to-talk cycle ("RECORDING...", "SENT", "UPLOAD FAILED"), which
// doesn't have a fresh snapshot to render (it deliberately skips wifi
// until after recording finishes).
void eink_show_message(const char *message) {
    eink_ensure_initialized();
    EPD_Clear();
    ui_screens_render_message(message);
    EPD_Display();
    ESP_LOGI(TAG, "e-ink message shown: %s", message);
}

// F7: recording a reply to a live check-in keeps the dashboard's check-in
// section on screen with a "REPLYING..." header instead of the header/prompt
// being replaced by a generic "RECORDING..." message -- the user should
// still be able to see what they're replying to while talking. Reads the
// prompt/weather/next straight out of s_last_screen (the check-in that's
// live right now is necessarily the one that was just rendered), same
// fields eink_render_last_known() restores from -- only battery_pct needs
// to come from the caller since it isn't persisted.
void eink_show_checkin_recording(int battery_pct) {
    eink_ensure_initialized();
    EPD_Clear();

    device_ui_state_t state = {};
    state.battery_pct = battery_pct;
    state.has_weather = s_last_screen.has_weather;
    state.weather = s_last_screen.weather;
    state.next = s_last_screen.next;
    state.checkin_prompt = s_last_screen.checkin_prompt;
    state.checkin_replying = true;
    ui_screens_render_dashboard(state);

    EPD_Display();
    ESP_LOGI(TAG, "e-ink message shown: replying to check-in");
}

// PWR-hold shutdown screen: two centered lines, e-ink being bistable
// means this stays visible on screen after VBAT is cut below.
void eink_show_shutdown_screen(const char *line1, const char *line2) {
    eink_ensure_initialized();
    EPD_Clear();
    ui_screens_render_shutdown(line1, line2);
    EPD_Display();
    ESP_LOGI(TAG, "e-ink message shown: shutdown");
}
