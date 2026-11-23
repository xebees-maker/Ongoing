/**
 * @file    cloud_link.h
 * @brief   바깥 MQTT 브로커 연결 — 셀룰러(CGNAT) 현장에서 원격 접속용
 *
 * 2026-11-22(사용자 결정) — 셀룰러 라우터 뒤에서는 바깥에서 콘으로 들어올 수 없어서(CGNAT), 콘이 관리형 MQTT 브로커로
 * TLS 연결을 걸어 둠. 폰의 웹 화면(정적 호스팅)은 브로커에 WebSocket으로 붙고, 웹 요청은 MQTT로 콘에 옴(다음 단계).
 * 1단계: 연결·LWT(끊기면 브로커가 "offline")·"online" 알림 + 내부 RAM 실측 로그.
 * 3단계(11-22): 읽기 터널 — ff/<site>/req/<cid>로 온 웹 요청을 콘 웹 서버(127.0.0.1)에 넘기고 ff/<site>/res/<cid>로 응답
 * (형식은 cloud_link.c 터널 주석). GET과 로그인·세션 되찾기만 — 쓰기는 4단계.
 *
 * 설정은 LittleFS 파일 cloud.cfg(설정은 파일로 — NVS 안 씀). 줄 단위 key=value:
 *   uri=mqtts://<호스트>:<포트>   (없으면 연결 안 함)
 *   user=<사용자>  pass=<비밀번호>  (없으면 생략)
 *   site=<현장 이름>               (토픽 앞부분·클라이언트 ID — 콘 MAC은 쓰지 않음)
 * /admin/upload?file=cloud.cfg로 바꾼 뒤 재시작하면 반영.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* 연결 태스크 시작 — 부팅 때 한 번(LittleFS 마운트 뒤). 네트워크 준비는 cloud_link_network_ready()로 알림 */
void cloud_link_init(void);

/* esp_netif 시작 뒤 한 번 — 그 전에는 연결하지 않음 */
void cloud_link_network_ready(void);

/* 브로커에 연결돼 있는지 */
bool cloud_link_is_connected(void);

/* 2026-11-23 — 원격 접속 QR 주소(웹앱 + 접속 정보). base_len = 앞의 웹앱 주소 길이(화면 글자엔 이것만). cloud.cfg가 없으면 false */
bool cloud_link_web_qr_url(char *out, size_t n, size_t *base_len);
