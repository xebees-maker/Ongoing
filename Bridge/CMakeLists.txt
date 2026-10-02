# 2026-09-22 — 브릿지(브) 1차 개발: 콘의 소스/초기화 구조는 쓰지 않지만(사용자 지시),
# 지금은 실물 브릿지 보드(외장안테나+CAN) 구매 전이라 LCD가 달린 콘 보드 하드웨어를 빌려
# 이 펌웨어를 플래시하는 벤치테스트 단계 — 그래서 콘과 동일한 esp32s3 타겟 + LCD/터치 BSP.
# 2026-10-02 — 실물 브 보드(ESP32-S3-RS485-CAN, 화면 없음)로 옮김 — LCD/터치 BSP 제거.
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "C:/Projects/Ongoing/Common/components")

# ESP-IDF는 project()가 발견한 모든 컴포넌트를 REQUIRES 여부와 무관하게 컴파일한다 —
# Common/components 안의 C6 전용 센서 컴포넌트가 S3 빌드를 깨므로 콘과 동일하게 제외
# (Cntl/CMakeLists.txt와 동일한 이유, project_cntl_scd41_lp_i2c_bug 메모리 참고)
set(EXCLUDE_COMPONENTS "sensor_dht22" "sensor_scd41" "sensor_ds18b20" "esp_lcd_touch_axs5106" "esp_lcd_touch_cst816")  # sensor_ds18b20: 센스 전용, 1-Wire 관리형 컴포넌트를 끌어옴
# 2026-10-02 — 터치 드라이버 2개: 예전엔 GT911 의존이 esp_lcd_touch를 끌어와 빌드됐지만 화면을 빼면서 그 의존이 없어짐(CAM과 같게 제외)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bridge)
