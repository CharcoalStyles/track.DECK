idf_component_register(
    SRCS "eink_ui.cpp"
    INCLUDE_DIRS "./"
    REQUIRES port_bsp sync_proto eink_lvgl
)
