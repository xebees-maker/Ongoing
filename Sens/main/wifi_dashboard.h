/**
 * @file    wifi_dashboard.h
 * @brief   C6 센서값을 폰 브라우저로 보기 위한 임시 WiFi 대시보드 (개발/테스트용)
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_now_link.h"

void wifi_dashboard_init(void);

/**
 * @brief 매 샘플 주기마다 앱이 호출 — esp_now_node_set_readings()와 같은 채널 모델을 써서
 *        레거시(DHT22+SCD41 5채널)와 헤드리스(센서 1개, 2~3채널) 모두 같은 함수로 넘긴다.
 */
void wifi_dashboard_set_readings(sensor_kind_t sensor_kind, uint8_t chan_count,
                                  const uint8_t *chan_type, const uint8_t *chan_ok,
                                  const float *chan_val,
                                  int batt_pct, bool batt_ok, bool powered);

/* 화면 표기용 — "STA CH1 192.168.0.43" / "AP CH1 192.168.4.1" / "STA 연결 중..." 형태로 채움 */
void wifi_dashboard_get_net_status(char *buf, size_t buflen);
