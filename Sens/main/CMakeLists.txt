# NOTE: REQUIRES는 항상 두 빌드 타깃의 합집합으로 고정한다 — idf_component_register()의
# REQUIRES는 CMake가 sdkconfig를 다 읽기 전 "컴포넌트 요구사항 조기 확장" 단계에서
#한 번 평가되는데, 그 시점엔 CONFIG_SENS_BUILD_HEADLESS 같은 Kconfig 값이 아직 없어서
# if(CONFIG_...)로 REQUIRES를 갈라도 항상 else() 쪽으로 평가돼버린다(실기 확인됨).
# SRCS는 이 문제가 없음(실제 빌드 단계에서 다시 평가되어 Kconfig 값이 반영됨) — 그래서
# 소스 파일만 조건부로 가르고, REQUIRES는 안 쓰는 쪽도 그냥 같이 요구한다(빌드는 되지만
# 안 쓰는 쪽 SRCS가 없으니 실제로 컴파일/링크되진 않음).
# 2026-09-05 — SCD41 헤드리스 빌드만 esp_now_node_cask.c(CAM과 동일한 CASK/WAKE_HELLO 구조,
# project_cntl_cam_wake_hello_no_mem_desync 세션 참고)를 씀. 그 외(레거시 C6 콤보,
# DHT22/SHT45/SHT40 헤드리스)는 예전 esp_now_node.c 그대로 — 사용자가 이번 작업 범위를
# SCD41 헤드리스로만 명시적으로 한정함. 위 REQUIRES 조기평가 문제와 같은 이유로 여기도
# SRCS(빌드 단계에서 재평가됨)만 가르고 두 파일을 동시에 REQUIRES에 넣지는 않음(esp_now_node.h가
# 둘 다에서 쓰는 공용 선언부).
if(CONFIG_SENS_BUILD_HEADLESS)
    if(CONFIG_SENS_SENSOR_SCD41)
        set(sens_srcs
            "sens_deep_sleep_node.c"
            "esp_now_node_cask.c"
            "rwdt_guard.c"
        )
    else()
        set(sens_srcs
            "sensor_node.c"
            "wifi_dashboard.c"
            "esp_now_node.c"
        )
    endif()
else()
    set(sens_srcs
        "sensor-c6.c"
        "wifi_dashboard.c"
        "esp_now_node.c"
    )
endif()

# bsp_c6는 esp32c3 타깃에서 REQUIRES에 넣으면 안 됨 — REQUIRES는 main이 그 컴포넌트를
# 실제로 쓰는지와 무관하게 그 컴포넌트 자신의 SRCS(bsp_c6.c)를 통째로 컴파일하게 만드는데,
# bsp_c6.h가 GPIO_NUM_22/23을 하드코딩해서 이 핀이 없는 esp32c3에선 컴파일 자체가 깨짐.
# IDF_TARGET은(Kconfig CONFIG_* 와 달리) REQUIRES 조기 확장 단계에서도 이미 값이 있어서
# 여기서 조건부로 걸어도 안전하다.
set(sens_requires
    nvs_flash
    freertos
    log
    lvgl
    bsp_c3_pico
    status_led
    sensor_dht22
    sensor_scd41
    sensor_sht4x
    battery_monitor
    history_log
    esp_adc
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_usb_serial_jtag
    esp_wifi
    esp_event
    esp_netif
    esp_http_server
    esp_timer
    esp_now_link
    esp_now_channelsync
    esp_now_reliable
    esp_pm
    esp_hal_wdt
    esp_hw_support
)
if(NOT IDF_TARGET STREQUAL "esp32c3")
    list(APPEND sens_requires bsp_c6)
endif()

idf_component_register(
    SRCS
        ${sens_srcs}
    INCLUDE_DIRS
        "."
    REQUIRES
        ${sens_requires}
)
