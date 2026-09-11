#ifndef UI_STATE_H
#define UI_STATE_H

#include <cstdint>

#include "sync_snapshot.h"

// Soonest upcoming reminder or calendar event, whichever is sooner.
struct next_item_t {
    bool have_next;
    bool is_event;
    int64_t at;
    char label[SYNC_STR_TEXT_LEN];
};

// Today's remaining reminders/events (soonest excluded -- that's next_item_t
// above), soonest-first. Sized for the worst case: every reminder and every
// calendar event landing on the same local day -- so labels are truncated
// to what a single 8pt row can actually show (the row itself further
// ellipsizes via LV_LABEL_LONG_MODE_DOTS), not the full SYNC_STR_TEXT_LEN.
// Keeping this struct small matters: it's stack-allocated, copied more than
// once per render, and shares an 8KB task stack with wifi/HTTP/JSON/LVGL
// locals -- an earlier version used SYNC_STR_TEXT_LEN (256) here and blew
// the stack.
#define UI_REMAINING_LABEL_LEN 40
struct sorted_item_t {
    bool is_event;
    int64_t at;
    char label[UI_REMAINING_LABEL_LEN];
};
#define UI_MAX_REMAINING_ITEMS (SYNC_MAX_REMINDERS + SYNC_MAX_CALENDAR_EVENTS)
struct remaining_items_t {
    int count;
    sorted_item_t items[UI_MAX_REMAINING_ITEMS];
};

// Shared data contract for the LVGL screen builders in ui_screens.cpp --
// each builder is a pure (parent, state) -> widget-tree function, reading
// only from this struct, never reaching past it for content.
struct device_ui_state_t {
    // status bar -- ui_status_bar_build() only reads these
    int battery_pct;
    bool has_weather;
    sync_weather_t weather;

    // content -- the dashboard content builder always draws "next", then
    // either the check-in or the remaining-items list below it.
    next_item_t next;
    remaining_items_t remaining;   // shown when checkin_prompt is null
    const char *checkin_prompt;    // non-null = a live check-in is showing
    bool checkin_replying;         // true -> "REPLYING..." header, else "CHECK-IN"
};

#endif // UI_STATE_H
