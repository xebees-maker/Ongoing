# 2026-09-22 — 브릿지(브) 1차 개발: 콘의 소스/초기화 구조는 쓰지 않지만(사용자 지시),
# 지금은 실물 브릿지 보드(외장안테나+CAN) 구매 전이라 LCD가 달린 콘 보드 하드웨어를 빌려
# 이 펌웨어를 플래시하는 벤치테스트 단계 — 그래서 콘과 동일한 esp32s3 타겟 + LCD/터치 BSP.
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "C:/Projects/Ongoing/Common/components")

# ESP-IDF는 project()가 발견한 모든 컴포넌트를 REQUIRES 여부와 무관하게 컴파일한다 —
# Common/components 안의 C6 전용 센서 컴포넌트가 S3 빌드를 깨므로 콘과 동일하게 제외
# (Cntl/CMakeLists.txt와 동일한 이유, project_cntl_scd41_lp_i2c_bug 메모리 참고)
set(EXCLUDE_COMPONENTS "sensor_dht22" "sensor_scd41")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bridge)
