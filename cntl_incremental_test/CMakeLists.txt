cmake_minimum_required(VERSION 3.5)

# CAM 연결 기능: esp_now_link(공유 프로토콜 정의)는 Cntl/Sens/CAM이 공유하는 컴포넌트.
# Common/components는 Sens(C6) 전용 것도 같이 들어있는데, 이 IDF 빌드는 non-minimal이라
# EXTRA_COMPONENT_DIRS 밑에서 발견된 컴포넌트를 전부 링크함(실제 REQUIRES 여부 무관) —
# sensor_scd41이 C6 전용 LP_I2C 매크로를 하드코딩해서 S3 빌드를 깨므로 Cntl과 동일하게 제외.
set(EXTRA_COMPONENT_DIRS "C:/Projects/Ongoing/Common/components")
set(EXCLUDE_COMPONENTS "sensor_dht22" "sensor_scd41")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(lvgl_v9_demo)

# Cntl 통합 테스트 2단계: 커스텀 파티션 테이블(fonts LittleFS 파티션 포함)로 전환하면서
# fs_root/ -> "fonts" 파티션 이미지 자동 생성+플래시 (Cntl과 동일)
littlefs_create_partition_image(fonts "${CMAKE_SOURCE_DIR}/fs_root" FLASH_IN_PROJECT)

# Cntl 통합 테스트 3단계: TinyTTF kerning 버그 패치 (Cntl과 동일 이유 — managed_components는
# gitignore라 컴포넌트 매니저가 lvgl을 다시 받을 때마다 로컬 수정이 사라짐)
execute_process(
    COMMAND ${python} "${CMAKE_SOURCE_DIR}/patches/fix_tiny_ttf_kerning.py"
            "${CMAKE_SOURCE_DIR}/managed_components/lvgl__lvgl/src/libs/tiny_ttf/lv_tiny_ttf.c"
)
