#include "icon_placeholder.h"

// 16x16 filled circle, LV_COLOR_FORMAT_I1 (1 bit/pixel, MSB-first,
// 2-byte row stride). First 8 bytes are the mandatory 2-entry palette
// LVGL expects for I1 images (index 0 = black, index 1 = white, RGBA
// bytes -- format/ordering copied from LVGL's own
// src/demos/render/assets/img_render_lvgl_logo_i1.c), generated
// programmatically (not hand-drawn) to avoid bit-packing mistakes.
static const uint8_t icon_placeholder_map[] = {
    0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, // palette: black, white

    0xff, 0xff,
    0xf8, 0x1f,
    0xf0, 0x0f,
    0xe0, 0x07,
    0xc0, 0x03,
    0x80, 0x01,
    0x80, 0x01,
    0x80, 0x01,
    0x80, 0x01,
    0x80, 0x01,
    0x80, 0x01,
    0xc0, 0x03,
    0xe0, 0x07,
    0xf0, 0x0f,
    0xf8, 0x1f,
    0xff, 0xff,
};

const lv_image_dsc_t icon_placeholder = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_I1,
        .flags = 0,
        .w = 16,
        .h = 16,
        .stride = 2,
        .reserved_2 = 0,
    },
    .data_size = sizeof(icon_placeholder_map),
    .data = icon_placeholder_map,
    .reserved = NULL,
    .reserved_2 = NULL,
};
