idf_component_register(SRCS "adhi-firmware.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES
                    port_bsp
                    sync_proto
                    waveshare__pcf85063a
                    my_button
                    json
                    nvs_flash
                    esp_wifi
                    esp_netif
                    esp_event
                    esp_http_client
                    esp_timer
                    driver)
