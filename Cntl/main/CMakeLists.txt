idf_component_register(
    SRCS
        "main.c"
        "fs.c"
        "ui_font.c"
        "ui_strings.c"
        "ui_main.c"
        "ui_log.c"
        "esp_now_hub.c"
        "esp_now_photo.c"
        "rtc_sync.c"
        "../components/waveshare_rgb_lcd_port.c"
    INCLUDE_DIRS
        "."
        "../components"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        esp_now_link
        esp_timer
        waveshare__pcf85063a
    PRIV_REQUIRES
        esp_new_jpeg)

idf_build_get_property(build_components BUILD_COMPONENTS)
if("lvgl" IN_LIST build_components)
    idf_component_get_property(lvgl_lib lvgl COMPONENT_LIB)
elseif("lvgl__lvgl" IN_LIST build_components)
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
endif()

if(lvgl_lib)
    target_compile_options(${lvgl_lib} PRIVATE -Wno-format)
endif()
