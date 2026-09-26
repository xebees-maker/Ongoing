/**
 * @file    bsp_esp32s3_cam.h
 * @brief   Waveshare ESP32-S3-CAM-OVxxxx 보드 정의
 *
 * 아래 GPIO는 추측이 아니라 Waveshare 공식 GitHub 예제(waveshareteam/ESP32-S3-CAM-OVxxxx)의
 * 실제 동작하는 코드에서 그대로 가져온 값이다:
 *   - 카메라(DVP)/SCCB 핀: examples/ESP-IDF-v5.5.1/01_simple_video_server/sdkconfig
 * 이 보드는 기성품이라 사용자가 직접 배선한 게 아니라 공장에서 이미 배선된 상태 —
 * bsp_c3_pico.h(LOLIN C3 Pico, 사용자가 직접 브레드보드 배선)와 달리 이 핀들은
 * "확정값"이지 TODO 플레이스홀더가 아니다. 다만 IDF v6.0.1에서 esp_video 컴포넌트가
 * 그대로 받아지는지, 그리고 실기(카메라 촬영/SD 마운트)는 아직 검증 안 됨 —
 * project_cam_node 메모리 참고.
 */
#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════
 * I2C 공유 버스 — 카메라 SCCB(센서 제어)와 IO 익스팬더(CH32V003)가 같은 버스를 씀
 * (서로 다른 I2C 슬레이브 주소라 충돌 없음: 센서는 OV5640=0x3C/OV3660=0x3C류,
 * 익스팬더는 0x24 — io_expander_ch32v003.h 참고)
 * ════════════════════════════════════════════════════════════ */
#define BSP_CAM_I2C_PORT      0
#define BSP_CAM_I2C_SCL       GPIO_NUM_7
#define BSP_CAM_I2C_SDA       GPIO_NUM_8
#define BSP_CAM_I2C_FREQ_HZ   100000

/* ════════════════════════════════════════════════════════════
 * 카메라 DVP 인터페이스 (esp32-camera의 camera_config_t에 그대로 대입)
 * ════════════════════════════════════════════════════════════ */
#define BSP_CAM_DVP_XCLK      38
/* 재검증 중(2026-07-22): Waveshare 공식 예제(esp_video 기반)는 이 보드를 20MHz로 씀 —
 * esp32-camera 드라이버의 set_pll() 고정비율이 딱 20MHz를 가정한 값이라 이론상 미스매치가
 * 없어야 하는 지점. 예전 "20MHz NO-SOI" 결과는 구버전 61440B 버퍼 + COM17(전원 결함 보드)
 * 조합에서 나온 걸로 의심됨 — 2MB 버퍼 + COM5(정상 보드)로 재테스트 */
#define BSP_CAM_DVP_XCLK_FREQ_HZ  20000000
#define BSP_CAM_DVP_PCLK      41
#define BSP_CAM_DVP_VSYNC     17
#define BSP_CAM_DVP_DE        18   /* esp_video 프레임워크는 HREF 대신 DE(data enable) 사용 */
#define BSP_CAM_DVP_D0        45
#define BSP_CAM_DVP_D1        47
#define BSP_CAM_DVP_D2        48
#define BSP_CAM_DVP_D3        46
#define BSP_CAM_DVP_D4        42
#define BSP_CAM_DVP_D5        40
#define BSP_CAM_DVP_D6        39
#define BSP_CAM_DVP_D7        21
#define BSP_CAM_SENSOR_RESET_PIN  (-1)  /* 미사용(연결 안 됨), Waveshare 예제와 동일 */
#define BSP_CAM_SENSOR_PWDN_PIN   (-1)  /* 미사용(연결 안 됨), Waveshare 예제와 동일 */

/* SD카드(SDMMC 1-bit, CLK=16/CMD=43/D0=44) — 2026-09-26 CAM에서 SD 사용을 없애서 핀 정의 삭제
 * (사진은 촬영 즉시 CNTL로 푸시, 저장은 CNTL SD). */

/* ════════════════════════════════════════════════════════════
 * IO 익스팬더(CH32V003, I2C 주소 0x24) 핀 배정 — 2026-09-26 스키매틱(References/
 * ESP32-S3-CAM-XXXX-schematic.pdf) 우측 핀아웃표 확인값:
 *   EXIO0=TP_RST  EXIO1=LCD_RST  EXIO2=SD_CS  EXIO3=CAM_PWDN
 *   EXIO4=PA_CTRL(스피커 앰프, cam_speaker.c)  EXIO5=BAT_EN  EXIO6=PWR_LED  EXIO7=CHG_DET
 * 예전엔 Waveshare SD 예제를 따라 IO2/IO6을 "SD 인에이블"로 켰는데, 실제로는 IO2=SD_CS,
 * IO6=전원 LED였음 — SD 제거로 IO2는 안 건드리고, IO6은 상태 LED로 씀.
 * ════════════════════════════════════════════════════════════ */
#define BSP_CAM_IO_EXPANDER_PWR_LED_PIN      6  /* CH32V003_IO_6 = EXIO6 = PWR_LED */

/* status_led_init_custom()/status_led_set_pattern()에 쓸 PWR_LED 키 — 실제 GPIO와 안 겹치게
 * GPIO_NUM_MAX 이상 값(status_led.h 참고) */
#define BSP_CAM_PWR_LED_STATUS_ID  ((gpio_num_t)(GPIO_NUM_MAX + BSP_CAM_IO_EXPANDER_PWR_LED_PIN))

/* 2026-08-23 — 스키매틱 핀아웃표 확인값: EXIO5 = BAT_EN. PWR 버튼은 배터리를 레귤레이터에
 * 일시적으로만 연결하고, 소프트웨어가 부팅 초기에 이 핀을 켜야 버튼을 놔도 전원이
 * 자체유지(래치)됨 — 이게 없어서 버튼 누르고 있을 때만 켜져 있던 버그, CAML에서 발견+수정
 * 후 실기 검증 완료(project_caml_bat_en_latch_missing_resolved 메모리 참고) */
#define BSP_CAM_IO_EXPANDER_BAT_EN_PIN        5  /* CH32V003_IO_5 */

/**
 * @brief 보드 레벨 초기화 — 공유 I2C 버스 생성 + IO 익스팬더 초기화 + BAT_EN(전원 유지)
 *        켜기 + PWR_LED 쓰기 태스크 생성. 카메라 초기화는 별도 호출.
 */
esp_err_t bsp_esp32s3_cam_init(void);

/**
 * @brief PWR_LED(EXIO6) 켜기/끄기 — 블로킹 안 함. I2C 쓰기는 전용 태스크가 함(태스크 알림으로
 *        최신 값만 전달, 밀린 중간값은 버림). WiFi 콜백/esp_timer 문맥에서 불러도 됨.
 *        status_led_init_custom()의 write_fn으로 쓰는 용도.
 */
void bsp_esp32s3_cam_pwr_led_set(bool on);

/**
 * @brief 딥슬립 직전 호출 — PWR_LED를 즉시(블로킹) 끄고 이후 bsp_esp32s3_cam_pwr_led_set()은
 *        무시. IO 익스팬더 출력은 ESP32가 자는 동안에도 유지되므로 켜진 채 자면 계속 켜져 있음.
 */
void bsp_esp32s3_cam_pwr_led_shutdown(void);

/**
 * @brief bsp_esp32s3_cam_init()이 만든 공유 I2C 버스 핸들 반환 — 카메라 SCCB 초기화 시
 *        새 버스를 또 만들지 않고 이걸 재사용해야 함(같은 핀에 마스터 두 개 못 만듦).
 */
i2c_master_bus_handle_t bsp_esp32s3_cam_get_i2c_bus(void);

#ifdef __cplusplus
}
#endif
