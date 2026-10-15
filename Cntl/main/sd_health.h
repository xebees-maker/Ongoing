#pragma once
/**
 * @file    sd_health.h
 * @brief   SD 상태 감시(2026-10-14, 사용자 설계 — SD 개선 1단계 "알리기와 기록").
 *
 * 배경: 콘 SD가 이틀 동안 새 클러스터에 쓴 데이터·디렉터리 항목을 잃었는데 쓰기 함수는 모두 성공을 돌려줘서 아무 보고도
 * 없었음(Docs/한일 146). 그래서:
 *  - 어디서든 이상을 보고하는 창구 하나(sd_health_report) — 처음 이상이면 "고장" 상태 → 모든 SD 쓰기 중지, 화면 5009 +
 *    ntfy 푸시(기존 경로: ui_main의 LVGL 틱이 sd_health_take_pending()으로 가져가 mark_sd_io_fail).
 *  - 고장 상태는 LittleFS에 남아 재부팅해도 유지(망가진 카드에 조용히 다시 쓰지 않게) — 재연결/포맷 검증 통과 때만 풀림.
 *  - 1시간마다 FAT1/FAT2 비교(원본 섹터), 사후 분석 기록(카드 CID, 최근 이상 20건)과 하루 쓰기 카운터(효과 측정용).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SDH_EV_VERIFY_STATS = 1,   /* 통계 원시 기록: 쓰고 다시 읽은 내용이 다름 */
    SDH_EV_VERIFY_AGG,         /* 통계 집계 파일: 쓰고 다시 읽은 내용이 다름 */
    SDH_EV_VERIFY_PHOTO,       /* 사진: 저장 뒤 다시 읽은 크기·CRC가 다름(또는 파일이 없음) */
    SDH_EV_MISSING_PHOTO,      /* 사진 목록엔 있는데 파일이 없음 */
    SDH_EV_BAD_AGG_RECORD,     /* 집계 파일 안에 그 주에 속하지 않는 칸(0xFF·쓰레기) */
    SDH_EV_FAT_MISMATCH,       /* 정기 점검: FAT1과 FAT2가 다름(두 번 확인) */
    SDH_EV_TEST,               /* 시험용 명령(임시 API) */
    /* 2026-10-15(MPIO 카드 손상 — 0번 섹터·디렉터리·통계가 8KB 단위로 바뀌었는데 아무 보고 없었음) — 아래는 값을 바꾸지 말고 뒤에만 추가 */
    SDH_EV_LAYOUT,             /* 0번 섹터·부트 섹터를 못 읽거나 서명(55AA)이 없음 — 예전엔 FAT 검사가 말없이 건너뜀 */
    SDH_EV_BOOT_AREA_CHANGED,  /* 콘이 쓰지 않는 영역(0번 섹터 ~ FAT 앞, FSInfo 제외)이 마운트 때와 다름 */
    SDH_EV_BAD_DIR_ENTRY,      /* 사진·통계 폴더에 이름이 깨진 항목 */
    SDH_EV_BAD_STATS_RECORD,   /* 통계 원시 기록에 그 주에 속하지 않는 레코드(0xFF·쓰레기) */
    SDH_EV_COUNT
} sd_health_event_t;

typedef enum {
    SDH_C_FSYNC = 0,   /* fsync(또는 닫기로 확정) 횟수 */
    SDH_C_CREATE,      /* 새 파일 만들기 */
    SDH_C_RENAME,      /* 이름 바꾸기 */
    SDH_C_UNLINK,      /* 지우기 */
    SDH_C_BYTES,       /* 쓴 바이트 */
    SDH_C_VERIFY_OK,   /* 다시 읽어 확인 통과 */
    SDH_C_FAT_CHECK,   /* FAT 정기 비교 횟수 */
    SDH_C_COUNT
} sd_health_counter_t;

/* main에서 LittleFS 마운트 뒤 한 번 — 저장된 기록을 읽고, 1시간 타이머 시작 */
void sd_health_init(void);

/* 이상 보고(어느 태스크에서든). path는 앞 39자만 기록 */
void sd_health_report(sd_health_event_t ev, const char *path, uint32_t offset);

/* false면 SD에 쓰지 말 것(고장 상태) */
bool sd_health_writes_allowed(void);

/* 재연결/포맷 뒤 검증을 통과했을 때(ui_main clear_sd_io_fail) — 고장 상태 해제 */
void sd_health_clear(void);

/* LVGL 틱에서 — 화면·푸시로 올릴 새 이상이 있으면 true와 한 줄 설명 */
bool sd_health_take_pending(char *reason, size_t reason_len);

void sd_health_count(sd_health_counter_t c, uint32_t n);

/* 2026-10-15 — sd_storage가 마운트·포맷 성공 직후 부름: 부트 영역 기준값을 기록 태스크에서 새로 잡음 */
void sd_health_on_mount(void);

/* 2026-10-15(시험, 임시 — 할 일 AF에서 제거) — 1시간 점검을 지금 한 번(기록 태스크로).
 * flip=true면 먼저 예약 영역의 안 쓰는 섹터(파티션 시작+2) 첫 바이트를 뒤집어 씀 — 부트 영역 감시 양성 시험(한 번 더 부르면 되돌아감) */
void sd_health_check_now(bool flip);

/* sd_storage가 마운트 직전에 읽은 카드 CID 한 줄 */
void sd_health_set_card(const char *cid_line);

/* 웹(/api/sd/health) — JSON 한 덩어리를 buf에 씀, 길이 반환 */
int sd_health_json(char *buf, size_t cap);
