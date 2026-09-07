/**
 * @file    sd_storage.h
 * @brief   콘 SD카드(4GB, 사용자가 이미 장착) 마운트 — 센서 시계열 통계 + (나중에) 캠 사진
 *          저장용 대용량 저장소. LittleFS(/assets, ~8MB, 폰트+설정 전용)와는 완전히 별개.
 *
 * 하드웨어: Waveshare ESP32-S3-Touch-LCD-4.3B(reference_cntl_schematic 메모리 참고).
 * SD는 SPI 모드(IO11=MOSI, IO12=SCK, IO13=MISO), CS는 일반 GPIO가 아니라 CH422G I2C
 * 익스팬더의 EXIO4(ch422g.h의 CH422G_IO_SD_CS)로 나감. 이 SPI버스엔 SD카드 말고 다른
 * 장치가 없음(사용자 확인, 2026-09-06) — 트랜잭션마다 CS를 I2C로 토글하면 SPI보다 훨씬
 * 느린 I2C가 매 파일시스템 접근마다 끼어들어 심하게 느려지므로, 마운트 시 한 번만
 * LOW(선택)로 내린 채 계속 유지하고, ESP-IDF SDSPI 드라이버의 gpio_cs는
 * SDSPI_SLOT_NO_CS(=GPIO_NUM_NC)로 둬서 드라이버가 CS를 아예 안 건드리게 함(공식
 * 지원되는 패턴 — esp_driver_sdspi/src/sdspi_host.c에서 GPIO_UNUSED로 skip 확인).
 */
#pragma once

#include "esp_err.h"

#define SD_STORAGE_MOUNT_POINT "/sdcard"

/* app_main()에서 waveshare_esp32_s3_rgb_lcd_init() 이후(I2C 버스+CH422G가 이미 초기화된
 * 뒤) 호출 — waveshare_rgb_lcd_get_i2c_bus()로 공유 I2C 버스를 가져다 씀. 실패해도 앱
 * 전체를 막지 않음(CAM 저장소 등 SD 없이도 동작해야 하는 기존 기능들과 무관) — 실패
 * 시 로그만 남기고 ESP_FAIL 등 반환, 호출부는 계속 진행하면 됨 */
esp_err_t sd_storage_init(void);
