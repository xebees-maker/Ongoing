#pragma once
/**
 * @file    web_access_log.h
 * @brief   2026-10-17(A 목록 5) — 웹 접속 기록: 요청마다 IP·방법·주소·상태·응답 크기를 Dev Log(I "ACCESS")에 남김.
 *          새 웹 처리기는 없음 — httpd_resp_send / send_chunk / set_status를 링커로 가로채서(main/CMakeLists.txt --wrap) 응답이 끝날 때
 *          기록하고, 없는 주소(404)는 httpd 오류 처리기로 기록. 같은 IP·같은 주소·같은 상태는 60초에 한 줄만(그 사이 횟수는 다음 줄에 "+N")
 */
#include "esp_http_server.h"

/* httpd_start 직후 한 번 */
void web_access_log_register(httpd_handle_t server);
