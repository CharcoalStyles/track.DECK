idf_component_register(
    SRCS "eink_ui.cpp" "ui_screens.cpp" "icon_placeholder.c"
         "font_nokia_8.c" "font_nokia_16.c" "font_nokia_24.c"
    INCLUDE_DIRS "./"
    REQUIRES port_bsp sync_proto lvgl
    PRIV_REQUIRES esp_timer
)

# font_nokia_{8,16,24}.c are lv_font_conv output, generated assuming an
# external "lvgl/lvgl.h" install layout rather than this project's flat
# "lvgl.h" include path -- this define picks their LV_LVGL_H_INCLUDE_SIMPLE
# branch instead, matching icon_placeholder.c's own plain #include "lvgl.h".
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
