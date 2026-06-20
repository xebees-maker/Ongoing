/**
 * @file    wifi_dashboard.h
 * @brief   C6 센서값을 폰 브라우저로 보기 위한 임시 WiFi 대시보드 (개발/테스트용)
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

void wifi_dashboard_init(void);

void wifi_dashboard_set_readings(float dht_temp, float dht_humi, bool dht_ok,
                                  int co2, float scd_temp, float scd_humi, bool scd_ok,
                                  int batt_pct, bool batt_ok, bool powered);

/* 화면 표기용 — "STA CH1 192.168.0.43" / "AP CH1 192.168.4.1" / "STA 연결 중..." 형태로 채움 */
void wifi_dashboard_get_net_status(char *buf, size_t buflen);
