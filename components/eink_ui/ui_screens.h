#ifndef UI_SCREENS_H
#define UI_SCREENS_H

#include "ui_state.h"

// Every ui_screens_render_*() below builds one full 200x200 LVGL screen and
// flushes it straight into port_display's buffer via EPD_DrawColorPixel.
// None of them call EPD_Clear()/EPD_Display() -- the caller (eink_ui.cpp)
// owns the panel write, same contract eink_lvgl_draw_dashboard() used to
// have.

// Status bar (time/battery/weather) + the soonest reminder/event, or blank
// if state.next.have_next is false. Shared by the normal dashboard, the
// restored last-known dashboard, and (with state.next set to the reminder
// that just fired) the F9 reminder-override screen.
void ui_screens_render_dashboard(const device_ui_state_t &state);

// Status bar + a live check-in's prompt taking over the content area.
void ui_screens_render_checkin(const device_ui_state_t &state);

// "REPLYING..." header + the prompt being replied to, same layout as the
// check-in screen's content area. No status bar (matches the screen it
// replaces, eink_show_checkin_recording()).
void ui_screens_render_recording(const char *prompt_text);

// A short wrapped message, no status bar. Used for transient PTT/check-in
// status text ("SENDING...", "SENT", ...).
void ui_screens_render_message(const char *message);

// PWR-hold shutdown screen: two centered lines, no status bar.
void ui_screens_render_shutdown(const char *line1, const char *line2);

// Small bottom-right "N/max" pending-voice-note badge, drawn as a second,
// small canvas over whatever screen was just rendered. Draws nothing when
// count <= 0, matching the badge's original silent-when-empty behavior.
void ui_screens_render_pending_voice_badge(int count, int max_count);

#endif // UI_SCREENS_H
