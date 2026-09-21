# 2026-09-21 — Bridge: CNTL의 ESP-NOW 라디오 스택을 통째로 대신 떠맡는 XIAO Seeed(ESP32-C6)
# 보드. CNTL<->Bridge는 isolated I2C 4단자(VCC/GND/SDA/SCL)로만 연결됨(project_cntl_i2c_bridge_design_2026_09_21
# 메모리 참고). Sens/CAM 쪽에서 보면 이 보드가 예전 CNTL과 똑같은 "허브"로 보임 — 프로토콜
# 변경 없음(esp_now_link.h 그대로 재사용).
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "C:/Projects/Ongoing/Common/components")

# ESP-IDF는 project()가 발견한 모든 컴포넌트를 REQUIRES 여부와 무관하게 컴파일한다(Sens의
# CMakeLists.txt 동일 이슈 참고, project_cntl_scd41_lp_i2c_bug 메모리). Bridge는 화면이 없는
# 헤드리스 보드라 LCD/터치 드라이버 컴포넌트가 전혀 필요 없는데, 이들이 CNTL/Sens 프로젝트의
# managed_components(espressif__esp_lcd_touch 등, 이 프로젝트엔 없음)에 의존해서 그냥 두면
# "unknown name" 설정 오류가 남 — 아예 빌드 대상에서 뺀다.
set(EXCLUDE_COMPONENTS
    esp_lcd_touch_axs5106
    esp_lcd_touch_cst816
    esp_lcd_jd9853
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bridge)
