#include "eink_lvgl.h"

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

// Content region below the existing hand-rolled status bar (y=0..53,
// drawn separately by eink_ui.cpp's draw_status_bar() before this is
// called) -- matches draw_dashboard()'s own y=54 start.
#define CONTENT_W PANEL_W
#define CONTENT_H 146
#define CONTENT_Y_OFFSET 54

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

// Shared by every eink_lvgl_draw_*() entry point below: allocate a draw
// buffer, create+configure an LVGL display for it, let the caller build
// its widget tree, force one synchronous render+flush, then tear
// everything down. Scoped to a single render call -- this device renders
// once per wake with no animation, so there's no reason to keep an LVGL
// display object (or its FreeRTOS-task-driven refresh loop, which this
// deliberately skips) resident for the rest of the boot.
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

struct dashboard_ctx_t {
    bool has_next;
    bool is_event;
    int64_t at_epoch;
    const char *label;
};

static void build_dashboard_screen(lv_obj_t *screen, void *ctx_ptr) {
    dashboard_ctx_t *ctx = (dashboard_ctx_t *)ctx_ptr;
    if (!ctx->has_next) {
        return; // blank content area, matching draw_dashboard()'s own no-op behavior
    }

    time_t at = (time_t)ctx->at_epoch;
    struct tm local_tm;
    localtime_r(&at, &local_tm);
    char time_buf[6];
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d", local_tm.tm_hour, local_tm.tm_min);
    char header[24];
    snprintf(header, sizeof(header), "%s %s:", ctx->is_event ? "EVENT" : "NEXT", time_buf);

    lv_obj_t *headline = lv_label_create(screen);
    lv_obj_set_style_text_font(headline, &font_nokia_16, 0);
    lv_label_set_text(headline, header);
    lv_obj_set_pos(headline, 8, 0);

    lv_obj_t *icon = lv_image_create(screen);
    lv_image_set_src(icon, &icon_placeholder);
    lv_obj_set_pos(icon, CONTENT_W - 16 - 8, 0);

    lv_obj_t *body = lv_label_create(screen);
    lv_obj_set_style_text_font(body, &font_nokia_8, 0);
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(body, CONTENT_W - 16);
    lv_label_set_text(body, ctx->label);
    lv_obj_set_pos(body, 8, 22);
}

void eink_lvgl_draw_dashboard(bool has_next, bool is_event, int64_t at_epoch, const char *label) {
    dashboard_ctx_t ctx = {has_next, is_event, at_epoch, label};
    render_lvgl_canvas(CONTENT_W, CONTENT_H, CONTENT_Y_OFFSET, build_dashboard_screen, &ctx);
}

// Generic (font, text) stack, centered as one group -- shared by every
// full-screen multi-line canvas below.
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
void eink_lvgl_draw_shutdown_screen(const char *line1, const char *line2) {
    const line_spec_t lines[] = {
        {&font_nokia_24, line1},
        {&font_nokia_24, line2},
    };
    stacked_lines_ctx_t ctx = {lines, sizeof(lines) / sizeof(lines[0])};
    render_lvgl_canvas(PANEL_W, PANEL_H, 0, build_stacked_lines_screen, &ctx);
}
