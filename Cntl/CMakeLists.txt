cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "C:/Projects/Ongoing/Common/components")
# Cntl(S3)에는 센서 하드웨어가 없음 — Common/components는 Sens(C6)와 공유되지만
# sensor_dht22/sensor_scd41은 Sens 전용. sensor_scd41은 C6 전용 LP_I2C 매크로를
# 하드코딩해서 S3 빌드를 깨므로 명시적으로 제외해야 함.
# bsp_ws_1_47은 1.85C 교체 후 더 안 쓰지만 디렉터리는 남겨둠(나중에 또 보드 바꿀 때 참고용).
# main이 REQUIRES에서만 빼는 걸로는 부족함 — 이 IDF 빌드는 non-minimal이라 발견된 컴포넌트를
# 전부 링크하는데, bsp_ws_1_47과 bsp_ws_1_85c가 같은 함수 이름(bsp_board_init 등)을 export해서
# 링크 순서상 먼저 오는 쪽(구버전)이 조용히 이겨버림 — 반드시 명시적으로 제외해야 함.
set(EXCLUDE_COMPONENTS "sensor_dht22" "sensor_scd41" "bsp_ws_1_47")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(startup)

# TinyTTF kerning 버그 패치 — managed_components는 gitignore라 컴포넌트 매니저가
# lvgl을 다시 받을 때마다 로컬 수정이 사라짐(2026-06-16 최초 수정, 2026-06-21 재발).
# 매 configure마다 멱등적으로 재적용 — 자세한 내용은 patches/fix_tiny_ttf_kerning.py 참고.
execute_process(
    COMMAND ${python} "${CMAKE_SOURCE_DIR}/patches/fix_tiny_ttf_kerning.py"
            "${CMAKE_SOURCE_DIR}/managed_components/lvgl__lvgl/src/libs/tiny_ttf/lv_tiny_ttf.c"
)