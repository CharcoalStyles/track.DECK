#ifndef FONT_NOKIA_H
#define FONT_NOKIA_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Generated via lv_font_conv from ~/Downloads/nokiafc22.ttf ("Nokia
// Cellphone FC"), --bpp 1 (true monochrome, no anti-aliasing) so it stays
// crisp on this 1bpp e-ink panel -- same reasoning as the built-in Unscii
// fonts this replaces, but this typeface reads less "chunky" at these
// sizes. Regenerate (one command per size, wrapped here for readability
// -- no trailing backslashes needed, this is not shell input) with:
//   npx lv_font_conv --font nokiafc22.ttf --size <size> -r 0x20-0x7E
//   --format lvgl --bpp 1 --no-compress --no-prefilter
//   --force-fast-kern-format --lv-font-name font_nokia_<size>
//   -o font_nokia_<size>.c
extern lv_font_t font_nokia_8;
extern lv_font_t font_nokia_16;
extern lv_font_t font_nokia_24;

#ifdef __cplusplus
}
#endif

#endif // FONT_NOKIA_H
