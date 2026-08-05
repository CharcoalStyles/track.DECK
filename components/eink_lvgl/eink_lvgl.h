#ifndef EINK_LVGL_H
#define EINK_LVGL_H

#include <cstdint>

// Renders the dashboard content region (below the status bar, which the
// caller has already drawn) via LVGL widgets, writing directly into
// port_display's buffer through EPD_DrawColorPixel. Does not call
// EPD_Clear()/EPD_Display() itself -- caller owns the panel write, exactly
// like the hand-rolled draw_dashboard() this replaces at its two call
// sites (eink_render()'s regular-sync branch, eink_render_last_known()'s
// restored-dashboard branch). draw_reminder_override_screen()'s call to
// the hand-rolled draw_dashboard() is deliberately left alone -- out of
// scope for this pass, see PROJECT_PLAN.md/CLAUDE.md context.
void eink_lvgl_draw_dashboard(bool has_next, bool is_event, int64_t at_epoch, const char *label);

// PWR-hold shutdown screen: line1/line2 (e.g. "Track"/"Deck"), both at
// 24px, centered as one group, full-screen. Same panel-write contract as
// eink_lvgl_draw_dashboard() above: writes into port_display's buffer,
// caller still owns EPD_Clear()/EPD_Display().
void eink_lvgl_draw_shutdown_screen(const char *line1, const char *line2);

#endif // EINK_LVGL_H
