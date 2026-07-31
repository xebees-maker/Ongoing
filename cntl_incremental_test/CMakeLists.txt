cmake_minimum_required(VERSION 3.5)

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
