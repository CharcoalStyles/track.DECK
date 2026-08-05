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

// Shared data contract for the LVGL screen builders in ui_screens.cpp --
// each builder is a pure (parent, state) -> widget-tree function, reading
// only from this struct, never reaching past it for content.
struct device_ui_state_t {
    // status bar -- ui_status_bar_build() only reads these
    int battery_pct;
    bool has_weather;
    sync_weather_t weather;
    const char *notice; // e.g. "SYNC FAILED", or nullptr

    // content -- read by whichever ui_screens_render_*() the caller picked
    next_item_t next;             // dashboard / reminder-override content
    const char *checkin_prompt;   // check-in content (nullptr = none)
};

#endif // UI_STATE_H
