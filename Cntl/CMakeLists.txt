cmake_minimum_required(VERSION 3.5)

# CAM 연결 기능: esp_now_link(공유 프로토콜 정의)는 Cntl/Sens/CAM이 공유하는 컴포넌트.
# Common/components는 Sens(C6) 전용 것도 같이 들어있는데, 이 IDF 빌드는 non-minimal이라
# EXTRA_COMPONENT_DIRS 밑에서 발견된 컴포넌트를 전부 링크함(실제 REQUIRES 여부 무관) —
# sensor_scd41이 C6 전용 LP_I2C 매크로를 하드코딩해서 S3 빌드를 깨므로 Cntl과 동일하게 제외.
set(EXTRA_COMPONENT_DIRS "C:/Projects/Ongoing/Common/components")
set(EXCLUDE_COMPONENTS "sensor_dht22" "sensor_scd41")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(lvgl_v9_demo)

# RTC 부팅 시딩용 time_sync.txt(main/rtc_sync.c)가 2026-08-01 커밋 이후 갱신된 적 없이
# 고정값으로 매번 재플래시되던 버그(2026-08-09 발견) 수정 — littlefs 이미지 생성 직전에
# 매 빌드마다 현재 PC 시각으로 재생성 (rtc_sync.c와 동일하게 TZ 변환 없이 raw 숫자로 기록,
# tools/gen_time_sync.py 참고)
add_custom_target(gen_time_sync ALL
    COMMAND ${python} "${CMAKE_SOURCE_DIR}/tools/gen_time_sync.py"
            "${CMAKE_SOURCE_DIR}/assets_root/time_sync.txt"
    COMMENT "Regenerating assets_root/time_sync.txt with current PC time"
)

# Cntl 통합 테스트 2단계: 커스텀 파티션 테이블(assets LittleFS 파티션 포함)로 전환하면서
# assets_root/ -> "assets" 파티션 이미지 자동 생성+플래시 (폰트+설정파일 공용, 2026-08-01 리네임)
littlefs_create_partition_image(assets "${CMAKE_SOURCE_DIR}/assets_root" FLASH_IN_PROJECT)
add_dependencies(littlefs_assets_bin gen_time_sync)

# Cntl 통합 테스트 3단계: TinyTTF kerning 버그 패치 (Cntl과 동일 이유 — managed_components는
# gitignore라 컴포넌트 매니저가 lvgl을 다시 받을 때마다 로컬 수정이 사라짐)
execute_process(
    COMMAND ${python} "${CMAKE_SOURCE_DIR}/patches/fix_tiny_ttf_kerning.py"
            "${CMAKE_SOURCE_DIR}/managed_components/lvgl__lvgl/src/libs/tiny_ttf/lv_tiny_ttf.c"
)
