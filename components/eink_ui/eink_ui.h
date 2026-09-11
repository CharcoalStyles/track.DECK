#ifndef EINK_UI_H
#define EINK_UI_H

#include <cstdint>

#include "sync_snapshot.h"
#include "ui_state.h"

// Persisted summary of the last real sync render, survives deep sleep --
// lets eink_render_last_known() redraw the same screen without a fresh
// sync/snapshot.
struct last_screen_t {
    bool valid;
    bool is_checkin;
    char checkin_prompt[SYNC_STR_TEXT_LEN];
    char checkin_id[SYNC_STR_ID_LEN];
    bool has_weather;
    sync_weather_t weather;
    next_item_t next;
};
extern last_screen_t s_last_screen;

// Runs PortDisplay_Init()/EPD_Init() once per boot; safe to call from
// every e-ink entry point below.
void eink_ensure_initialized(void);

// Registers the footer-draw callback (pending-voice "N/15" badge, plus
// whatever notice the current render passes through -- see
// eink_render_last_known()'s notice param). The pending-voice count itself
// lives in main/adhi-firmware.cpp (it scans the SD card's pending_voice
// queue), so eink_ui calls back into it via this function pointer rather
// than depending on main directly. Call once at boot, before the first
// render.
void eink_ui_set_pending_voice_indicator_cb(void (*cb)(const char *notice));

// F9: reminder that just activated takes over the screen. Owns
// EPD_Clear/EPD_Display itself.
void draw_reminder_override_screen(bool has_weather, const sync_weather_t &weather, int battery_pct,
                                    const next_item_t &reminder_item);

// Main sync-driven render (live check-in, dashboard, or reminder
// override). Updates s_last_screen.
void eink_render(const sync_snapshot_t &snap, int battery_pct, const sync_soonest_reminder_t *override_reminder);

// Redraws from s_last_screen, no wifi/sync involved. notice is non-null
// only when this cycle's sync failed (e.g. "SYNC FAILED").
void eink_render_last_known(int battery_pct, const char *notice);

// Simple full-refresh status message (e.g. "RECORDING...", "SENT").
void eink_show_message(const char *message);

// Same dashboard layout as a live check-in, but with a "REPLYING..." header
// -- used while recording a voice reply to a check-in. Prompt/weather/next
// come from s_last_screen; only battery_pct (not persisted) is passed in.
void eink_show_checkin_recording(int battery_pct);

// PWR-hold shutdown screen: two centered lines (e.g. "Track"/"Deck").
// Owns EPD_Clear/EPD_Display itself, same as every entry point above.
void eink_show_shutdown_screen(const char *line1, const char *line2);

// Footer strip (pending-voice "N/max" badge bottom-right, notice
// bottom-left), drawn as an overlay on top of whatever screen is currently
// on the panel. Draws nothing when count <= 0 and notice is null. Called
// from main/adhi-firmware.cpp's own callback (registered via
// eink_ui_set_pending_voice_indicator_cb() above), which owns the SD-card
// queue scan this needs a count from.
void eink_ui_draw_pending_voice_badge(int count, int max_count, const char *notice);

#endif // EINK_UI_H
