idf_component_register(
    SRCS
        "main.c"
        "ui_screen.c"
        "bridge_esp_now.c"
        "bridge_sr.c"
        "can_link.c"

        "../components/waveshare_rgb_lcd_port.c"
        "../components/ch422g.c"
    INCLUDE_DIRS
        "."
        "../components"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_twai
        can_bridge_link
        esp_now_reliable
        esp_now_link
)

idf_build_get_property(build_components BUILD_COMPONENTS)
if("lvgl" IN_LIST build_components)
    idf_component_get_property(lvgl_lib lvgl COMPONENT_LIB)
elseif("lvgl__lvgl" IN_LIST build_components)
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
endif()

if(lvgl_lib)
    target_compile_options(${lvgl_lib} PRIVATE -Wno-format)
endif()
