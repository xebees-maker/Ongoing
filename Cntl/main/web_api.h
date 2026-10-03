/**
 * @file    web_api.h
 * @brief   웹 화면 API(SPA용) — 2026-10-03(할 일 AD). 화면 API는 인증 + 웹 세션(콘 잠금)을 거침
 */
#pragma once

#include "esp_http_server.h"

/* GET /api/dashboard 등 등록 */
void web_api_register_handlers(httpd_handle_t server);
