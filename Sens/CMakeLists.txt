# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "C:/Projects/Ongoing/Common/components")

# ESP-IDF는 project()가 발견한 모든 컴포넌트를 REQUIRES 여부와 무관하게 컴파일한다
# (Cntl 쪽의 동일 이슈 참고: project_cntl_scd41_lp_i2c_bug 메모리). bsp_c6는 레거시
# LCD 콤보 보드(esp32c6) 전용이고 GPIO_NUM_22/23을 하드코딩해서 esp32c3에선 컴파일
# 자체가 깨지므로, esp32c3 타깃일 땐 아예 빌드 대상에서 뺀다.
if(IDF_TARGET STREQUAL "esp32c3")
    set(EXCLUDE_COMPONENTS "bsp_c6")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sensor-c6)
