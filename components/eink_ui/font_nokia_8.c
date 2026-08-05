/*******************************************************************************
 * Size: 8 px
 * Bpp: 1
 * Opts: --font /home/charcoal/Downloads/nokiafc22.ttf --size 8 -r 0x20-0x7E --format lvgl --bpp 1 --no-compress --no-prefilter --force-fast-kern-format --lv-font-name font_nokia_8 -o font_nokia_8.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef FONT_NOKIA_8
#define FONT_NOKIA_8 1
#endif

#if FONT_NOKIA_8

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0020 " " */
    0x0,

    /* U+0021 "!" */
    0xff, 0xcc,

    /* U+0022 "\"" */
    0xb4,

    /* U+0023 "#" */
    0x57, 0xfe, 0xaf, 0xfd, 0x40,

    /* U+0024 "$" */
    0x53, 0xf5, 0xe7, 0xaf, 0xca,

    /* U+0025 "%" */
    0xcb, 0x61, 0xc, 0x21, 0xb4, 0xc0,

    /* U+0026 "&" */
    0x73, 0x67, 0x3d, 0xdf, 0x77, 0x40,

    /* U+0027 "'" */
    0xc0,

    /* U+0028 "(" */
    0x2b, 0x6d, 0x91,

    /* U+0029 ")" */
    0x89, 0xb6, 0xd4,

    /* U+002A "*" */
    0x25, 0x7e, 0xef, 0xd4, 0x80,

    /* U+002B "+" */
    0x21, 0x3e, 0x42, 0x0,

    /* U+002C "," */
    0x78,

    /* U+002D "-" */
    0xf0,

    /* U+002E "." */
    0xf0,

    /* U+002F "/" */
    0x25, 0xad, 0x20,

    /* U+0030 "0" */
    0x76, 0xf7, 0xbd, 0xed, 0xc0,

    /* U+0031 "1" */
    0x7d, 0xb6, 0xd8,

    /* U+0032 "2" */
    0xf0, 0xc6, 0xec, 0x63, 0xe0,

    /* U+0033 "3" */
    0xf0, 0xc6, 0xe1, 0x8f, 0xc0,

    /* U+0034 "4" */
    0x19, 0xd7, 0x3f, 0x8c, 0x60,

    /* U+0035 "5" */
    0xf4, 0x3c, 0x31, 0x8f, 0xc0,

    /* U+0036 "6" */
    0x76, 0x3d, 0xbd, 0xed, 0xc0,

    /* U+0037 "7" */
    0xf8, 0xcc, 0x66, 0x31, 0x80,

    /* U+0038 "8" */
    0x76, 0xf6, 0xed, 0xed, 0xc0,

    /* U+0039 "9" */
    0x76, 0xf7, 0xb7, 0x8d, 0xc0,

    /* U+003A ":" */
    0xf3, 0xc0,

    /* U+003B ";" */
    0xf1, 0xe0,

    /* U+003C "<" */
    0x13, 0x6c, 0x63, 0x10,

    /* U+003D "=" */
    0xf0, 0xf0,

    /* U+003E ">" */
    0x8c, 0x63, 0x6c, 0x80,

    /* U+003F "?" */
    0xf0, 0xcc, 0xc6, 0x1, 0x80,

    /* U+0040 "@" */
    0x7b, 0x3f, 0xf7, 0xff, 0x7, 0x80,

    /* U+0041 "A" */
    0x76, 0xf7, 0xbf, 0xef, 0x60,

    /* U+0042 "B" */
    0xf6, 0xfd, 0xbd, 0xef, 0xc0,

    /* U+0043 "C" */
    0x7e, 0x31, 0x8c, 0x61, 0xe0,

    /* U+0044 "D" */
    0xf6, 0xf7, 0xbd, 0xef, 0xc0,

    /* U+0045 "E" */
    0xfe, 0x3d, 0x8c, 0x63, 0xe0,

    /* U+0046 "F" */
    0xfe, 0x3d, 0x8c, 0x63, 0x0,

    /* U+0047 "G" */
    0x76, 0x31, 0xbd, 0xed, 0xe0,

    /* U+0048 "H" */
    0xde, 0xff, 0xbd, 0xef, 0x60,

    /* U+0049 "I" */
    0xff, 0xfc,

    /* U+004A "J" */
    0x33, 0x33, 0x33, 0xe0,

    /* U+004B "K" */
    0xcf, 0x6f, 0x38, 0xf3, 0x6c, 0xc0,

    /* U+004C "L" */
    0xcc, 0xcc, 0xcc, 0xf0,

    /* U+004D "M" */
    0x83, 0x8f, 0xbf, 0xfd, 0x78, 0xf1, 0x80,

    /* U+004E "N" */
    0x8f, 0x3e, 0xff, 0xdf, 0x3c, 0x40,

    /* U+004F "O" */
    0x7b, 0x3c, 0xf3, 0xcf, 0x37, 0x80,

    /* U+0050 "P" */
    0xf6, 0xf7, 0xbf, 0x63, 0x0,

    /* U+0051 "Q" */
    0x7b, 0x3c, 0xf3, 0xcf, 0x77, 0x83,

    /* U+0052 "R" */
    0xf6, 0xf7, 0xbf, 0x6b, 0x60,

    /* U+0053 "S" */
    0x7c, 0xc6, 0x33, 0xe0,

    /* U+0054 "T" */
    0xfc, 0xc3, 0xc, 0x30, 0xc3, 0x0,

    /* U+0055 "U" */
    0xde, 0xf7, 0xbd, 0xed, 0xc0,

    /* U+0056 "V" */
    0xcf, 0x3c, 0xde, 0x78, 0xc3, 0x0,

    /* U+0057 "W" */
    0xc7, 0x8f, 0x5f, 0xf7, 0xcf, 0x9b, 0x0,

    /* U+0058 "X" */
    0xcf, 0x37, 0x8c, 0x7b, 0x3c, 0xc0,

    /* U+0059 "Y" */
    0xcf, 0x37, 0x8c, 0x30, 0xc3, 0x0,

    /* U+005A "Z" */
    0xf8, 0xce, 0xee, 0x63, 0xe0,

    /* U+005B "[" */
    0xfb, 0x6d, 0xb7,

    /* U+005C "\\" */
    0x93, 0x26, 0x48,

    /* U+005D "]" */
    0xed, 0xb6, 0xdf,

    /* U+005E "^" */
    0x5e, 0x80,

    /* U+005F "_" */
    0xfc,

    /* U+0060 "`" */
    0x90,

    /* U+0061 "a" */
    0x70, 0xdf, 0xb7, 0x80,

    /* U+0062 "b" */
    0xc6, 0x3d, 0xbd, 0xef, 0xc0,

    /* U+0063 "c" */
    0x7c, 0xcc, 0x70,

    /* U+0064 "d" */
    0x18, 0xdf, 0xbd, 0xed, 0xe0,

    /* U+0065 "e" */
    0x76, 0xff, 0x87, 0x80,

    /* U+0066 "f" */
    0x7b, 0xed, 0xb0,

    /* U+0067 "g" */
    0x7e, 0xf6, 0xf1, 0xb8,

    /* U+0068 "h" */
    0xc6, 0x3d, 0xbd, 0xef, 0x60,

    /* U+0069 "i" */
    0xcf, 0xfc,

    /* U+006A "j" */
    0x61, 0xb6, 0xde,

    /* U+006B "k" */
    0xc6, 0x37, 0xee, 0x7b, 0x60,

    /* U+006C "l" */
    0xff, 0xfc,

    /* U+006D "m" */
    0xfe, 0xdb, 0xdb, 0xdb, 0xdb,

    /* U+006E "n" */
    0xf6, 0xf7, 0xbd, 0x80,

    /* U+006F "o" */
    0x76, 0xf7, 0xb7, 0x0,

    /* U+0070 "p" */
    0xf6, 0xf7, 0xec, 0x60,

    /* U+0071 "q" */
    0x7e, 0xf6, 0xf1, 0x8c,

    /* U+0072 "r" */
    0xdf, 0xcc, 0xc0,

    /* U+0073 "s" */
    0x7c, 0xf3, 0xe0,

    /* U+0074 "t" */
    0xdb, 0xed, 0x98,

    /* U+0075 "u" */
    0xde, 0xf7, 0xb7, 0x80,

    /* U+0076 "v" */
    0xde, 0xdc, 0xe2, 0x0,

    /* U+0077 "w" */
    0xc7, 0xaf, 0x5b, 0xe6, 0xc0,

    /* U+0078 "x" */
    0xde, 0xdd, 0xbd, 0x80,

    /* U+0079 "y" */
    0xde, 0xf6, 0xf1, 0xb8,

    /* U+007A "z" */
    0xf9, 0x99, 0x8f, 0x80,

    /* U+007B "{" */
    0xfb, 0x6d, 0xb7,

    /* U+007C "|" */
    0xff, 0xff,

    /* U+007D "}" */
    0xed, 0xb6, 0xdf,

    /* U+007E "~" */
    0x6d, 0x80
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 48, .box_w = 1, .box_h = 1, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 1, .adv_w = 48, .box_w = 2, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 3, .adv_w = 64, .box_w = 3, .box_h = 2, .ofs_x = 0, .ofs_y = 5},
    {.bitmap_index = 4, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 9, .adv_w = 96, .box_w = 5, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 14, .adv_w = 112, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 20, .adv_w = 112, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 26, .adv_w = 32, .box_w = 1, .box_h = 2, .ofs_x = 0, .ofs_y = 5},
    {.bitmap_index = 27, .adv_w = 64, .box_w = 3, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 30, .adv_w = 64, .box_w = 3, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 33, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 38, .adv_w = 96, .box_w = 5, .box_h = 5, .ofs_x = 0, .ofs_y = 1},
    {.bitmap_index = 42, .adv_w = 48, .box_w = 2, .box_h = 3, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 43, .adv_w = 80, .box_w = 4, .box_h = 1, .ofs_x = 0, .ofs_y = 3},
    {.bitmap_index = 44, .adv_w = 48, .box_w = 2, .box_h = 2, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 45, .adv_w = 64, .box_w = 3, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 48, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 53, .adv_w = 64, .box_w = 3, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 56, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 61, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 66, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 71, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 76, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 81, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 86, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 91, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 96, .adv_w = 48, .box_w = 2, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 98, .adv_w = 48, .box_w = 2, .box_h = 6, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 100, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 104, .adv_w = 80, .box_w = 4, .box_h = 3, .ofs_x = 0, .ofs_y = 1},
    {.bitmap_index = 106, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 110, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 115, .adv_w = 112, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 121, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 126, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 131, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 136, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 141, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 146, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 151, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 156, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 161, .adv_w = 48, .box_w = 2, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 163, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 167, .adv_w = 112, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 173, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 177, .adv_w = 128, .box_w = 7, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 184, .adv_w = 112, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 190, .adv_w = 112, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 196, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 201, .adv_w = 112, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 207, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 212, .adv_w = 80, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 216, .adv_w = 112, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 222, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 227, .adv_w = 112, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 233, .adv_w = 128, .box_w = 7, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 240, .adv_w = 112, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 246, .adv_w = 112, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 252, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 257, .adv_w = 64, .box_w = 3, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 260, .adv_w = 64, .box_w = 3, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 263, .adv_w = 64, .box_w = 3, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 266, .adv_w = 64, .box_w = 3, .box_h = 3, .ofs_x = 0, .ofs_y = 4},
    {.bitmap_index = 268, .adv_w = 96, .box_w = 6, .box_h = 1, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 269, .adv_w = 48, .box_w = 2, .box_h = 2, .ofs_x = 0, .ofs_y = 5},
    {.bitmap_index = 270, .adv_w = 96, .box_w = 5, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 274, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 279, .adv_w = 80, .box_w = 4, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 282, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 287, .adv_w = 96, .box_w = 5, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 291, .adv_w = 64, .box_w = 3, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 294, .adv_w = 96, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 298, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 303, .adv_w = 48, .box_w = 2, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 305, .adv_w = 64, .box_w = 3, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 308, .adv_w = 96, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 313, .adv_w = 48, .box_w = 2, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 315, .adv_w = 144, .box_w = 8, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 320, .adv_w = 96, .box_w = 5, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 324, .adv_w = 96, .box_w = 5, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 328, .adv_w = 96, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 332, .adv_w = 96, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 336, .adv_w = 80, .box_w = 4, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 339, .adv_w = 80, .box_w = 4, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 342, .adv_w = 64, .box_w = 3, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 345, .adv_w = 96, .box_w = 5, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 349, .adv_w = 96, .box_w = 5, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 353, .adv_w = 128, .box_w = 7, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 358, .adv_w = 96, .box_w = 5, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 362, .adv_w = 96, .box_w = 5, .box_h = 6, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 366, .adv_w = 96, .box_w = 5, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 370, .adv_w = 64, .box_w = 3, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 373, .adv_w = 48, .box_w = 2, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 375, .adv_w = 64, .box_w = 3, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 378, .adv_w = 96, .box_w = 5, .box_h = 2, .ofs_x = 0, .ofs_y = 5}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 32, .range_length = 95, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 1,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t font_nokia_8 = {
#else
lv_font_t font_nokia_8 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 8,          /*The maximum line height required by the font*/
    .base_line = 1,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if FONT_NOKIA_8*/

