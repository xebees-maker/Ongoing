idf_component_register(
    SRCS
        "cam_node.c"
        "rwdt_guard.c"
        "esp_now_cam.c"
        "dev_console.c"
        "cam_light.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash
        freertos
        log
        status_led
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        esp_now_link
        esp_now_channelsync
        esp_now_reliable
        esp32-camera
        esp_driver_ledc
        esp_driver_i2c
        bsp_esp32s3_cam
        io_expander_ch32v003
        console
        mbedtls
        esp_rom
        esp_driver_usb_serial_jtag
        esp_hal_wdt
        esp_hw_support
        cam_speaker
)

# 2026-10-20(사용자 결정 — 캠 송신 속도 조절) — esp_now_send를 감싸 아직 송신 완료(send_cb)가 안 온 프레임 수를 셈.
# 컴포넌트(esp_now_reliable 등)가 부르는 송신까지 다 세야 정확해서 링커 --wrap으로(__wrap_ 구현은 esp_now_cam.c)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_now_send")
