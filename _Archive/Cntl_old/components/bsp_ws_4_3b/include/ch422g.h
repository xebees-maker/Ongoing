/**
 * @file    ch422g.h
 * @brief   CH422G I2C IO 익스팬더 드라이버 — Waveshare ESP32-S3-Touch-LCD-4.3B용
 *
 * 이 칩은 일반적인 "슬레이브주소+레지스터오프셋" 구조가 아니라, 기능별로 완전히 다른
 * I2C 슬레이브 주소를 씀(칩 자체엔 진짜 레지스터 주소가 없음 — 그 "주소"가 곧 명령):
 *   0x24 = Mode(설정) 레지스터
 *   0x23 = OD_OUT(오픈드레인 출력, OC0~OC3)
 *   0x38 = IO_OUT(푸시풀 출력, IO0~IO7)
 *   0x26 = IO_IN (IO0~IO7을 입력으로 읽음)
 * 값 출처: Waveshare 공식 데모(github.com/waveshareteam/ESP32-S3-Touch-LCD-4.3B,
 * examples/ESP-IDF/05_IO_Test/lib/CH422G/CH422G.h, 09_lvgl_v9_demo/components/
 * waveshare_rgb_lcd_port.c) 그대로 확인.
 *
 * 이 보드에서 IO0~IO7의 실제 배선:
 *   IO0=DI0(절연입력0)  IO1=터치리셋  IO2=백라이트  IO3=LCD리셋
 *   IO4=SD카드 CS       IO5=DI1(절연입력1)  IO6/IO7=미사용
 * OC0/OC1 = 절연 출력 DO0/DO1(SSR 구동용), OC2/OC3은 이 보드에서 안 쓰임.
 *
 * 중요: IO뱅크는 전부 하나의 Mode.IO_OE 비트를 공유하는 양방향 버스라, DI0/DI1을
 * 입력으로 읽으려면 뱅크 전체를 잠깐 입력모드로 바꿔야 함 — 그 순간 백라이트/LCD리셋/
 * SD_CS 핀도 같이 "입력"이 되어 잠깐 하이임피던스 상태가 된다(전압은 유지되는 경우가
 * 많지만 보장 안 됨). ch422g_read_di()는 이 상태를 최소 시간만 유지하고 즉시 이전
 * 출력값으로 복원하지만, SD카드 트랜잭션과 겹치지 않게 호출부에서 주의할 것.
 * OC0~OC3(SSR)은 별도 오픈드레인 뱅크라 이 문제가 없음.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* IO뱅크(0x38/0x26, 푸시풀) 비트 배치 — 이 보드 고정 배선 */
#define CH422G_IO_DI0          (0x01)  ///< IO0 — 절연 입력 0
#define CH422G_IO_TOUCH_RST    (0x02)  ///< IO1 — 터치(GT911) 리셋
#define CH422G_IO_BACKLIGHT    (0x04)  ///< IO2 — LCD 백라이트
#define CH422G_IO_LCD_RST      (0x08)  ///< IO3 — LCD 리셋
#define CH422G_IO_SD_CS        (0x10)  ///< IO4 — SD카드 CS(active-low)
#define CH422G_IO_DI1          (0x20)  ///< IO5 — 절연 입력 1

/* OD뱅크(0x23, 오픈드레인) 비트 배치 — SSR 구동용 */
#define CH422G_OD_DO0          (0x01)  ///< OC0 — 절연 출력 0 (SSR1)
#define CH422G_OD_DO1          (0x02)  ///< OC1 — 절연 출력 1 (SSR2)

/* Mode(0x24) 비트 — ch422g_set_io_raw()로 GT911 리셋 시퀀스를 바이트 그대로
 * 재현할 때 필요해서 공개함(평소엔 ch422g_set_io/set_do가 알아서 재설정함) */
#define CH422G_MODE_IO_OE      (0x01)  ///< IO0~7 출력모드
#define CH422G_MODE_OD_EN      (0x04)  ///< OC0~3 오픈드레인 출력 활성화

/**
 * @brief CH422G 초기화 — I2C 버스에 내부 디바이스 핸들 4개(Mode/OD_OUT/IO_OUT/IO_IN) 등록.
 *        IO뱅크는 출력모드로, 백라이트/터치리셋/LCD리셋 해제/SD_CS deselect 상태로 시작.
 * @param bus 공유 I2C 버스(터치 GT911과 공유)
 */
esp_err_t ch422g_init(i2c_master_bus_handle_t bus);

/**
 * @brief IO뱅크(백라이트/LCD리셋/터치리셋/SD_CS) 중 지정한 비트(들)를 켜거나 끔.
 *        내부 섀도우 상태에 반영 후 전체 바이트를 다시 씀 — DI0/DI1 비트는 건드리지 않음.
 * @param bits CH422G_IO_* 비트마스크(여러 개 OR 가능)
 * @param level true=high(1), false=low(0)
 */
esp_err_t ch422g_set_io(uint8_t bits, bool level);

/**
 * @brief Mode/IO_OUT에 절대값을 그대로 씀(섀도우 OR/AND 병합 없이 덮어씀) — GT911 리셋처럼
 *        Waveshare 공식 시퀀스의 정확한 바이트값·순서를 그대로 재현해야 하는 초기 부팅
 *        구간 전용. 이후에는 ch422g_set_io()로 정상적으로 비트 단위 제어하면 됨.
 * @param mode_value Mode 레지스터(0x24)에 쓸 값(CH422G_MODE_* 조합)
 * @param io_value   IO_OUT 레지스터(0x38)에 쓸 값(CH422G_IO_* 조합) — 섀도우도 이 값으로 갱신
 */
esp_err_t ch422g_set_io_raw(uint8_t mode_value, uint8_t io_value);

/**
 * @brief 절연 입력 DI0/DI1을 읽음 — 잠깐 IO뱅크를 입력모드로 바꿨다가 즉시 이전 출력
 *        상태로 복원. SD카드 트랜잭션 중에는 호출하지 말 것.
 * @param out_di0 DI0 레벨(1=high)
 * @param out_di1 DI1 레벨(1=high)
 */
esp_err_t ch422g_read_di(bool *out_di0, bool *out_di1);

/**
 * @brief 절연 출력 DO0/DO1(SSR 구동용) 중 지정한 비트를 켜거나 끔 — OD 뱅크, IO뱅크와
 *        완전히 독립적이라 백라이트/SD_CS 등에 영향 없음.
 * @param bits CH422G_OD_* 비트마스크(여러 개 OR 가능)
 * @param level true=high(1, 통전), false=low(0)
 */
esp_err_t ch422g_set_do(uint8_t bits, bool level);

#ifdef __cplusplus
}
#endif
