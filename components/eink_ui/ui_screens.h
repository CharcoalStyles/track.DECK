#ifndef UI_SCREENS_H
#define UI_SCREENS_H

#include "ui_state.h"

// Every ui_screens_render_*() below builds one full 200x200 LVGL screen and
// flushes it straight into port_display's buffer via EPD_DrawColorPixel.
// None of them call EPD_Clear()/EPD_Display() -- the caller (eink_ui.cpp)
// owns the panel write, same contract eink_lvgl_draw_dashboard() used to
// have.

// Status bar (time/battery/weather), the soonest reminder/event, and below
// that either a live check-in's prompt (state.checkin_prompt non-null,
// header picked by state.checkin_replying) or the rest of today's
// reminders/events (state.remaining). Shared by the normal dashboard, the
// restored last-known dashboard, the F9 reminder-override screen, and the
// "replying to a check-in" screen.
void ui_screens_render_dashboard(const device_ui_state_t &state);

// A short wrapped message, no status bar. Used for transient PTT/check-in
// status text ("SENDING...", "SENT", ...).
void ui_screens_render_message(const char *message);

// PWR-hold shutdown screen: two centered lines, no status bar.
void ui_screens_render_shutdown(const char *line1, const char *line2);

// Bottom footer strip: pending-voice-note "N/max" badge (bottom-right) and,
// when notice is non-null (e.g. F8's "SYNC FAILED"), a notice (bottom-left)
// -- drawn together in one small canvas over whatever screen was just
// rendered. Draws nothing when count <= 0 and notice is null.
void ui_screens_render_footer(int count, int max_count, const char *notice);

#endif // UI_SCREENS_H
