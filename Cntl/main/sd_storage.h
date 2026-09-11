/**
 * @file    sd_storage.h
 * @brief   콘 SD카드(4GB, 사용자가 이미 장착) 마운트 — 센서 시계열 통계 + (나중에) 캠 사진
 *          저장용 대용량 저장소. LittleFS(/assets, ~8MB, 폰트+설정 전용)와는 완전히 별개.
 *
 * 하드웨어: Waveshare ESP32-S3-Touch-LCD-4.3B(reference_cntl_schematic 메모리 참고).
 * SD는 SPI 모드(IO11=MOSI, IO12=SCK, IO13=MISO), CS는 일반 GPIO가 아니라 CH422G I2C
 * 익스팬더의 EXIO4(ch422g.h의 CH422G_IO_SD_CS)로 나감. 이 SPI버스엔 SD카드 말고 다른
 * 장치가 없음(사용자 확인, 2026-09-06) — 트랜잭션마다 CS를 I2C로 토글하면 SPI보다 훨씬
 * 느린 I2C가 매 파일시스템 접근마다 끼어들어 심하게 느려지므로, 마운트 성공 후에는
 * LOW(선택)로 내린 채 계속 유지하고, ESP-IDF SDSPI 드라이버의 gpio_cs는
 * SDSPI_SLOT_NO_CS(=GPIO_NUM_NC)로 둬서 드라이버가 CS를 아예 안 건드리게 함(공식
 * 지원되는 패턴 — esp_driver_sdspi/src/sdspi_host.c에서 GPIO_UNUSED로 skip 확인).
 * 2026-09-11(재설계) — 단, 마운트/재마운트 "시도 직전"에는 sd_storage_init()이 매번
 * HIGH(비선택)->지연->LOW(선택) 펄스를 한 번 만듦 — 새 카드 교체 후 소프트웨어 재연결만
 * 해서는 실패하고 완전 리붓해야만 되던 실기 증상의 원인(CS가 재연결 내내 계속 LOW로
 * 고정돼 있어 SD SPI모드 진입에 필요한 엣지가 안 생겼던 것)을 해결하기 위함. */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#define SD_STORAGE_MOUNT_POINT "/sdcard"

/* app_main()에서 waveshare_esp32_s3_rgb_lcd_init() 이후(I2C 버스+CH422G가 이미 초기화된
 * 뒤) 호출 — waveshare_rgb_lcd_get_i2c_bus()로 공유 I2C 버스를 가져다 씀. 실패해도 앱
 * 전체를 막지 않음(CAM 저장소 등 SD 없이도 동작해야 하는 기존 기능들과 무관) — 실패
 * 시 로그만 남기고 ESP_FAIL 등 반환, 호출부는 계속 진행하면 됨 */
esp_err_t sd_storage_init(void);

/* 2026-09-10(사용자 설계 — "Storage[%(Remain MB)]: Picture xx(yy) / Measure zz(kk) /
 * Total aa(bb)") — SD카드 실제 전체용량/여유용량(esp_vfs_fat_info 그대로 전달). 미마운트
 * 등으로 실패하면 false, out 값은 안 건드림(호출부가 이전 값 유지하거나 0 처리) */
bool sd_storage_get_capacity(uint64_t *out_total_bytes, uint64_t *out_free_bytes);

/* 2026-09-10(사용자 설계 — [[project_cntl_sd_reliability_redesign_2026_09_10]]) — 하드웨어
 * 카드감지 핀이 없어 자동 핫스왑 감지는 못 하므로, 사용자가 명시적으로 누르는 "재연결
 * 시도" 버튼용. 언마운트(마운트돼 있었다면) 후 sd_storage_init()과 동일한 시퀀스를 다시
 * 돌림 — 같은 카드를 PC에서 복구해 재장착했든, 새 카드로 바꿨든 둘 다 커버 */
esp_err_t sd_storage_reconnect(void);

/* "포맷" 버튼용 — 마운트된 상태에서만 호출 가능(esp_vfs_fat_sdcard_format 요구사항),
 * 포맷 후 stats/photos 폴더까지 재생성. 되돌릴 수 없으니 호출부가 확인팝업을 거쳐야 함 */
esp_err_t sd_storage_format(void);

/* 지금 SD를 아예 못 쓰는 상태(미마운트)인지 — 주화면 상태표시용 */
bool sd_storage_is_mounted(void);
