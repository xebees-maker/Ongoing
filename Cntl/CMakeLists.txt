cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "C:/Projects/Ongoing/Common/components")
# Cntl(S3)에는 센서 하드웨어가 없음 — Common/components는 Sens(C6)와 공유되지만
# sensor_dht22/sensor_scd41은 Sens 전용. sensor_scd41은 C6 전용 LP_I2C 매크로를
# 하드코딩해서 S3 빌드를 깨므로 명시적으로 제외해야 함.
set(EXCLUDE_COMPONENTS "sensor_dht22" "sensor_scd41")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(startup)