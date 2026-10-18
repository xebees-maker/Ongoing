# 2026-10-18(Docs/설계_보브_보릴_2026-10-18.md) — 보릴: 보일러를 켜고 끄는 릴레이 장치(XIAO ESP32-C6 + 외장 안테나 + 3.3V High 트리거
# 릴레이 모듈, 상시 전원). 보브(브릿지 2번)를 통해 콘과 ESP-NOW로 통신.
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "C:/Projects/Ongoing/Common/components")
# main과 그 의존만 빌드(Common의 센서·LCD 컴포넌트는 안 씀)
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(boril)
