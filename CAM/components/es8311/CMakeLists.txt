# 2026-09-26 — 레지스트리 espressif/es8311 1.0.0의 프로젝트 수정본(IDF 6 새 I2C 드라이버 대응).
# 원본: https://github.com/espressif/esp-bsp/tree/master/components/es8311
idf_component_register(
    SRCS "es8311.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES esp_driver_i2c
    PRIV_REQUIRES log freertos
)
