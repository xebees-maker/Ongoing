# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "C:/Projects/Ongoing/Common/components")

# ESP-IDF는 project()가 발견한 모든 컴포넌트를 REQUIRES 여부와 무관하게 컴파일한다
# (Sens/Cntl 쪽의 동일 이슈 참고: project_cntl_scd41_lp_i2c_bug 메모리). CAM은 화면이
# 없어서 esp_lcd_touch 관리형 컴포넌트를 idf_component.yml에 선언하지 않는데, Common/
# components에 있는 터치 드라이버(Cntl의 1.85C 디스플레이 교체 작업용)가 그걸 요구해서
# 빌드 자체가 깨진다 — CAM 빌드에서는 아예 뺀다.
set(EXCLUDE_COMPONENTS "esp_lcd_touch_axs5106" "esp_lcd_touch_cst816")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cam-node)
