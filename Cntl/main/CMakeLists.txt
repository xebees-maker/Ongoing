idf_component_register(
    SRCS
        "main.c"
        "fs.c"
        "ui_font.c"
        "ui_strings.c"
        "ui_main.c"
        "ui_log.c"
        "dev_log.c"
        "node_hub.c"
        "photo_rx.c"
        "node_request.c"
        "wifi_sta.c"
        "rtc_sync.c"
        "device_config.c"
        "sd_storage.c"
        "sd_health.c"
        "fs_bench.c"
        "flash_usage.c"
        "net_diag.c"
        "stats_store.c"
        "photo_storage.c"
        "storage_mgr.c"
        "power_relay.c"
        "sens_kind_store.c"
        "can_bridge.c"
        "notify.c"
        "alarm.c"
        "romanize.c"
        "web_auth.c"
        "web_session.c"
        "web_api.c"

        "../components/waveshare_rgb_lcd_port.c"
        "../components/ch422g.c"
    INCLUDE_DIRS
        "."
        "../components"
    REQUIRES
        nvs_flash
        app_update
        bootloader_support
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        esp_http_client
        mbedtls
        esp_now_link
        esp_timer
        waveshare__pcf85063a
        battery_monitor
        fatfs
        sdmmc
        esp_driver_sdspi
        esp_driver_spi
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_twai
        can_bridge_link
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

# 2026-10-17 내부 RAM — esp_new_jpeg(바이너리 전용)는 디코드 작업 버퍼를 내부 RAM 우선(모자라면 PSRAM)으로 잡아서, 사진 한 장 디코드에
# 내부 RAM 45K를 잠깐 씀(웹 전송과 겹치면 바닥 근처). 라이브러리에 설정이 없어 할당 함수 두 개만 링커로 가로채 PSRAM 우선으로 바꿈
# (__wrap_ 구현은 ui_main.c의 decode_jpeg_scaled 옆)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=jpeg_calloc_inner" "-Wl,--wrap=jpeg_calloc_align_inner")
