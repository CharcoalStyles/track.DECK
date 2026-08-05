#include "ui_screens.h"

#include <cstdio>
#include <ctime>

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <lvgl.h>

#include "font_nokia.h"
#include "icon_placeholder.h"
#include "port_display.h"

// Panel is EPD_WIDTH x EPD_HEIGHT (200x200), byte-aligned (200/8 = 25, no
// stride padding), so the I1 draw buffer's row layout is a direct
// MSB-first bit-for-bit match with port_display's own framebuffer.
#define PANEL_W 200
#define PANEL_H 200

// Content (below the status bar) starts here on every screen that has one
// -- matches the original hand-rolled draw_status_bar()'s bottom divider
// at y=48 plus a small gap.
#define CONTENT_Y 54

// Pending-voice badge canvas: a thin strip along the bottom edge, full
// panel width so the label's own right-alignment lines up with panel
// coordinates (render_lvgl_canvas only offsets y, not x).
#define BADGE_H 16

static bool s_lvgl_core_initialized = false;
static int s_flush_stride = 0;
static int s_flush_y_offset = 0;

static uint32_t lvgl_tick_cb(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// I1 palette convention (confirmed against LVGL's own
// src/demos/render/assets/img_render_lvgl_logo_i1.c): bit=1 -> white,
// bit=0 -> black -- same convention as port_display's own
// DRIVER_COLOR_WHITE/BLACK, so no color remapping needed here beyond the
// bit test itself. s_flush_stride/s_flush_y_offset are set by
// render_lvgl_canvas() just before this fires -- safe since rendering is
// synchronous and single-threaded, never concurrent across canvases.
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    const uint8_t *pixels = px_map + 8; // skip the mandatory I1 palette header
    for (int32_t y = area->y1; y <= area->y2; y++) {
        for (int32_t x = area->x1; x <= area->x2; x++) {
            uint8_t byte = pixels[y * s_flush_stride + (x >> 3)];
            bool white = (byte >> (7 - (x & 7))) & 0x01;
            EPD_DrawColorPixel((uint16_t)x, (uint16_t)(y + s_flush_y_offset),
                                white ? DRIVER_COLOR_WHITE : DRIVER_COLOR_BLACK);
        }
    }
    lv_display_flush_ready(disp);
}

typedef void (*build_screen_fn)(lv_obj_t *screen, void *ctx);

// Shared by every ui_screens_render_*() below: allocate a draw buffer,
// create+configure an LVGL display for it, let the caller build its widget
// tree, force one synchronous render+flush, then tear everything down.
// Scoped to a single render call -- this device renders once per wake with
// no animation, so there's no reason to keep an LVGL display object (or
// its FreeRTOS-task-driven refresh loop, which this deliberately skips)
// resident for the rest of the boot.
static void render_lvgl_canvas(int width, int height, int y_offset, build_screen_fn build, void *ctx) {
    if (!s_lvgl_core_initialized) {
        lv_init();
        lv_tick_set_cb(lvgl_tick_cb);
        s_lvgl_core_initialized = true;
    }

    // LVGL reserves an 8-byte palette header before I1 pixel data, even
    // though this display's palette (black/white) is never actually
    // indexed into by anything of ours -- required buffer layout
    // regardless.
    size_t buf_size = (size_t)(width * height / 8) + 8;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        return; // leave the canvas as whatever EPD_Clear() left it
    }

    s_flush_stride = width / 8;
    s_flush_y_offset = y_offset;

    lv_display_t *disp = lv_display_create(width, height);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_I1);
    lv_display_set_buffers(disp, buf, NULL, (uint32_t)buf_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);

    build(lv_display_get_screen_active(disp), ctx);

    lv_tick_inc(1);
    lv_refr_now(disp);

    lv_display_delete(disp);
    heap_caps_free(buf);
}

static void format_local_hhmm(int64_t epoch, char *buf, size_t buf_len) {
    time_t t = (time_t)epoch;
    struct tm local_tm;
    localtime_r(&t, &local_tm);
    snprintf(buf, buf_len, "%02d:%02d", local_tm.tm_hour, local_tm.tm_min);
}

// Solid black horizontal rule -- explicit bg color/opa/border/radius so it
// renders correctly regardless of whatever default theme (or lack of one)
// is active, rather than relying on a plain lv_obj_create()'s default
// styling.
static void draw_divider(lv_obj_t *screen, int y) {
    lv_obj_t *line = lv_obj_create(screen);
    lv_obj_set_size(line, PANEL_W - 8, 1);
    lv_obj_set_pos(line, 4, y);
    lv_obj_set_style_bg_color(line, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
}

// F4 (PROJECT_PLAN.md): time (top-left), battery% (top-right), temp
// (top-center) if weather is available, then either a notice line (F8:
// "SYNC FAILED") or a plain divider, then (if weather is available) a
// cloud+rain row and a sunrise/sunset row, then a closing divider. Shared
// by every screen that has a status bar at all (dashboard, check-in,
// reminder-override) -- message/recording/shutdown screens skip it
// entirely, matching their original hand-rolled behavior.
static void ui_status_bar_build(lv_obj_t *screen, const device_ui_state_t &s) {
    char battery_buf[8];
    snprintf(battery_buf, sizeof(battery_buf), "%d%%", s.battery_pct);
    lv_obj_t *battery = lv_label_create(screen);
    lv_obj_set_style_text_font(battery, &font_nokia_16, 0);
    lv_label_set_text(battery, battery_buf);
    lv_obj_align(battery, LV_ALIGN_TOP_RIGHT, -4, 4);

    time_t now_epoch = time(nullptr);
    struct tm local_tm;
    localtime_r(&now_epoch, &local_tm);
    char time_buf[6];
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d", local_tm.tm_hour, local_tm.tm_min);
    lv_obj_t *clock = lv_label_create(screen);
    lv_obj_set_style_text_font(clock, &font_nokia_16, 0);
    lv_label_set_text(clock, time_buf);
    lv_obj_align(clock, LV_ALIGN_TOP_LEFT, 4, 4);

    if (s.has_weather) {
        char temp_buf[8];
        snprintf(temp_buf, sizeof(temp_buf), "%.0fC", (double)s.weather.temperature_c);
        lv_obj_t *temp = lv_label_create(screen);
        lv_obj_set_style_text_font(temp, &font_nokia_16, 0);
        lv_label_set_text(temp, temp_buf);
        lv_obj_align(temp, LV_ALIGN_TOP_MID, 0, 4);
    }

    if (s.notice) {
        lv_obj_t *notice = lv_label_create(screen);
        lv_obj_set_style_text_font(notice, &font_nokia_8, 0);
        lv_label_set_text(notice, s.notice);
        lv_obj_set_pos(notice, 8, 22);
    } else {
        draw_divider(screen, 22);
    }

    if (s.has_weather) {
        char cloud_buf[24];
        snprintf(cloud_buf, sizeof(cloud_buf), "CLOUD %d%%  RAIN %.1fMM", s.weather.cloud_cover_pct,
                  s.weather.precipitation_mm);
        lv_obj_t *cloud = lv_label_create(screen);
        lv_obj_set_style_text_font(cloud, &font_nokia_8, 0);
        lv_label_set_text(cloud, cloud_buf);
        lv_obj_set_pos(cloud, 8, 28);

        char sunrise_buf[6], sunset_buf[6];
        format_local_hhmm(s.weather.sunrise, sunrise_buf, sizeof(sunrise_buf));
        format_local_hhmm(s.weather.sunset, sunset_buf, sizeof(sunset_buf));
        char sun_buf[20];
        snprintf(sun_buf, sizeof(sun_buf), "SUN %s-%s", sunrise_buf, sunset_buf);
        lv_obj_t *sun = lv_label_create(screen);
        lv_obj_set_style_text_font(sun, &font_nokia_8, 0);
        lv_label_set_text(sun, sun_buf);
        lv_obj_set_pos(sun, 8, 37);
    }

    draw_divider(screen, 48);
}

static void ui_dashboard_content_build(lv_obj_t *screen, const next_item_t &next) {
    if (!next.have_next) {
        return; // blank content area
    }

    char time_buf[6];
    format_local_hhmm(next.at, time_buf, sizeof(time_buf));
    char header[24];
    snprintf(header, sizeof(header), "%s %s:", next.is_event ? "EVENT" : "NEXT", time_buf);

    lv_obj_t *headline = lv_label_create(screen);
    lv_obj_set_style_text_font(headline, &font_nokia_16, 0);
    lv_label_set_text(headline, header);
    lv_obj_set_pos(headline, 8, CONTENT_Y);

    lv_obj_t *icon = lv_image_create(screen);
    lv_image_set_src(icon, &icon_placeholder);
    lv_obj_set_pos(icon, PANEL_W - 16 - 8, CONTENT_Y);

    lv_obj_t *body = lv_label_create(screen);
    lv_obj_set_style_text_font(body, &font_nokia_8, 0);
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(body, PANEL_W - 16);
    lv_label_set_text(body, next.label);
    lv_obj_set_pos(body, 8, CONTENT_Y + 22);
}

// header_text is "CHECK-IN" for a live check-in, "REPLYING..." while
// recording a reply to one -- same prompt placement either way, so the
// prompt stays visible/unmoved when the header switches. Body uses
// LV_LABEL_LONG_MODE_DOTS (wrap + trailing "..." once the fixed
// width/height fills up) instead of a hand-rolled word-wrapper -- LVGL
// already does this natively.
static void ui_checkin_content_build(lv_obj_t *screen, const char *prompt, const char *header_text) {
    lv_obj_t *header = lv_label_create(screen);
    lv_obj_set_style_text_font(header, &font_nokia_16, 0);
    lv_label_set_text(header, header_text);
    lv_obj_set_pos(header, 8, CONTENT_Y);

    lv_obj_t *body = lv_label_create(screen);
    lv_obj_set_style_text_font(body, &font_nokia_8, 0);
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(body, PANEL_W - 16);
    lv_obj_set_height(body, PANEL_H - CONTENT_Y - 20 - 4);
    lv_label_set_text(body, prompt);
    lv_obj_set_pos(body, 8, CONTENT_Y + 20);
}

static void build_dashboard_screen(lv_obj_t *screen, void *ctx_ptr) {
    const device_ui_state_t *s = (const device_ui_state_t *)ctx_ptr;
    ui_status_bar_build(screen, *s);
    ui_dashboard_content_build(screen, s->next);
}

void ui_screens_render_dashboard(const device_ui_state_t &state) {
    render_lvgl_canvas(PANEL_W, PANEL_H, 0, build_dashboard_screen, (void *)&state);
}

static void build_checkin_screen(lv_obj_t *screen, void *ctx_ptr) {
    const device_ui_state_t *s = (const device_ui_state_t *)ctx_ptr;
    ui_status_bar_build(screen, *s);
    ui_checkin_content_build(screen, s->checkin_prompt, "CHECK-IN");
}

void ui_screens_render_checkin(const device_ui_state_t &state) {
    render_lvgl_canvas(PANEL_W, PANEL_H, 0, build_checkin_screen, (void *)&state);
}

static void build_recording_screen(lv_obj_t *screen, void *ctx_ptr) {
    const char *prompt = (const char *)ctx_ptr;
    ui_checkin_content_build(screen, prompt, "REPLYING...");
}

void ui_screens_render_recording(const char *prompt_text) {
    render_lvgl_canvas(PANEL_W, PANEL_H, 0, build_recording_screen, (void *)prompt_text);
}

static void build_message_screen(lv_obj_t *screen, void *ctx_ptr) {
    const char *message = (const char *)ctx_ptr;
    lv_obj_t *body = lv_label_create(screen);
    lv_obj_set_style_text_font(body, &font_nokia_16, 0);
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(body, PANEL_W - 16);
    lv_obj_set_height(body, 64);
    lv_label_set_text(body, message);
    lv_obj_set_pos(body, 8, 80);
}

void ui_screens_render_message(const char *message) {
    render_lvgl_canvas(PANEL_W, PANEL_H, 0, build_message_screen, (void *)message);
}

// Generic (font, text) stack, centered as one group -- currently only used
// by the shutdown screen below, written generically in case a second
// full-screen multi-line canvas shows up later.
struct line_spec_t {
    const lv_font_t *font;
    const char *text;
};
struct stacked_lines_ctx_t {
    const line_spec_t *lines;
    size_t count;
};
#define STACKED_LINE_GAP 4

static void build_stacked_lines_screen(lv_obj_t *screen, void *ctx_ptr) {
    stacked_lines_ctx_t *ctx = (stacked_lines_ctx_t *)ctx_ptr;

    int total_h = STACKED_LINE_GAP * (int)(ctx->count - 1);
    for (size_t i = 0; i < ctx->count; i++) {
        total_h += ctx->lines[i].font->line_height;
    }
    int y = (PANEL_H - total_h) / 2;
    if (y < 0) {
        y = 0;
    }

    for (size_t i = 0; i < ctx->count; i++) {
        lv_obj_t *label = lv_label_create(screen);
        lv_obj_set_style_text_font(label, ctx->lines[i].font, 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(label, PANEL_W);
        lv_label_set_text(label, ctx->lines[i].text);
        lv_obj_set_pos(label, 0, y);
        y += ctx->lines[i].font->line_height + STACKED_LINE_GAP;
    }
}

// PWR-hold shutdown screen: line1/line2 (e.g. "Track"/"Deck"), both at
// 24px, centered as one group on the full 200x200 canvas. A multi-size
// specimen stack (and a battery-percentage line) were tried here too but
// dropped after review -- back to the original screen's plain two-line
// shape, just with the new pixel font instead of the hand-rolled one.
void ui_screens_render_shutdown(const char *line1, const char *line2) {
    const line_spec_t lines[] = {
        {&font_nokia_24, line1},
        {&font_nokia_24, line2},
    };
    stacked_lines_ctx_t ctx = {lines, sizeof(lines) / sizeof(lines[0])};
    render_lvgl_canvas(PANEL_W, PANEL_H, 0, build_stacked_lines_screen, &ctx);
}

static void build_pending_voice_badge(lv_obj_t *screen, void *ctx_ptr) {
    const char *text = (const char *)ctx_ptr;
    lv_obj_t *label = lv_label_create(screen);
    lv_obj_set_style_text_font(label, &font_nokia_8, 0);
    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_TOP_RIGHT, -4, 4);
}

void ui_screens_render_pending_voice_badge(int count, int max_count) {
    if (count <= 0) {
        return; // silent when the queue is empty, matching the original indicator
    }
    char text[16];
    snprintf(text, sizeof(text), "%d/%d", count, max_count);
    render_lvgl_canvas(PANEL_W, BADGE_H, PANEL_H - BADGE_H, build_pending_voice_badge, (void *)text);
}
