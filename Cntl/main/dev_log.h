#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* 2026-09-27(사용자 설계 — 로그 정리) — 개발 로그. ESP_LOG 출력(시리얼)을 esp_log_set_vprintf로 가로채 PSRAM 링 버퍼에
 * 줄 단위(시각/레벨/태그/내용)로 쌓음. 시리얼 출력은 그대로 유지. SD에는 저장하지 않음(사용자 결정).
 * - 저장 문턱: 이보다 낮은 레벨은 아예 포맷되지 않음(esp_log_level_set 연동 — 느려지지 않게)
 * - 보기: 레벨별 개별 선택(mask) + 태그 필터 — 이미 쌓인 줄을 다시 거름
 * 세 값은 /assets/devlog.cfg에 저장(device_config.bin은 버전 고정 바이너리라 필드 추가 시 설정이 초기화돼서 따로 둠) */

/* 레벨 번호 — esp_log_level_t와 같은 값(E=1 W=2 I=3 D=4) */
#define DEV_LOG_LVL_E 1
#define DEV_LOG_LVL_W 2
#define DEV_LOG_LVL_I 3
#define DEV_LOG_LVL_D 4

#define DEV_LOG_MASK(lvl) (1u << (lvl))

/* 태그 필터 목록 — 0은 전체, 마지막은 "그 밖의 태그"(ESP-IDF 내부 등). 코드의 기능 태그와 같은 이름 */
#define DEV_LOG_TAG_ALL   0
extern const char *const DEV_LOG_TAGS[];   /* "All","CAN","LINK","CASK","PHOTO","CAM","SENS","RELAY","UI","SYS","Other" */
#define DEV_LOG_TAG_COUNT 11
#define DEV_LOG_TAG_OTHER (DEV_LOG_TAG_COUNT - 1)

/* app_main 맨 앞(fs_init 뒤)에서 한 번 — 버퍼 할당, 설정 읽기, 레벨 적용, 가로채기 시작 */
void dev_log_init(void);

uint8_t dev_log_get_save_level(void);
void    dev_log_set_save_level(uint8_t lvl);   /* 즉시 esp_log_level_set + 파일 저장 */
uint8_t dev_log_get_view_mask(void);
void    dev_log_set_view_mask(uint8_t mask);
uint8_t dev_log_get_tag_filter(void);
void    dev_log_set_tag_filter(uint8_t idx);

/* 새 줄이 들어올 때마다 증가 — 화면이 바뀐 게 있을 때만 다시 그리도록 */
uint32_t dev_log_seq(void);

/* 보기 설정(mask/태그)으로 거른 줄을 "시각 레벨 태그 내용\n" 형식으로 out에 채움 — 최신 줄이 맨 끝, out_cap에
 * 들어가는 만큼 최신 쪽부터. 반환: 채운 길이 */
size_t dev_log_render(char *out, size_t out_cap);

/* 2026-09-30 — 보기 필터 없이 링 전체(최대 400줄)를 오래된 것부터 같은 형식으로 채움. 웹(/api/devlog_dump)에서 부름 */
size_t dev_log_dump(char *out, size_t out_cap);
/* 2026-10-09 — 경고·에러(W/E) 줄만 따로 오래(2000줄) 남긴 링 덤프 */
size_t dev_log_dump_we(char *out, size_t out_cap);
