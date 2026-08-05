#ifndef ICON_PLACEHOLDER_H
#define ICON_PLACEHOLDER_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// 16x16 filled-circle placeholder, LV_COLOR_FORMAT_I1. Proves out the
// lv_image_dsc_t pipeline for icons on this 1bpp panel -- content is not
// meaningful yet. ponytail: once real icon artwork exists, generate its
// lv_image_dsc_t with LVGL's `lv_img_conv` CLI (CF_INDEXED_1_BIT output)
// instead of hand-authoring more of these.
extern const lv_image_dsc_t icon_placeholder;

#ifdef __cplusplus
}
#endif

#endif // ICON_PLACEHOLDER_H
