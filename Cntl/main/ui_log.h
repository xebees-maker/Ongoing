#pragma once

#include <stddef.h>
#include <stdbool.h>

/* 통계 탭 화면에 띄우는 소형 로그 뷰어 — 관심 있는 지점(사진 요청/응답/디코드 등)만 골라서
 * 여기 씀. esp_log_set_vprintf처럼 전체를 가로채지 않음(2026-08-01, 사용자 지시 — 화면이
 * 작아서 다 넣으면 못 읽음). LVGL에 의존하지 않아서 esp_now_photo.c 같은 하위 모듈에서도
 * 바로 쓸 수 있음 — 실제 화면 렌더링은 ui_main.c가 ui_log_get_snapshot()으로 가져가서 함. */
void ui_log_init(void);
void ui_log_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* out에 최근 로그 스냅샷을 널종료 문자열로 채움(최대 out_cap-1바이트) */
void ui_log_get_snapshot(char *out, size_t out_cap);

/* 실제 실패(메모리 할당/디코드/캐시 저장 등)만 이걸로 남김 — ui_log_add과 똑같이 로그에도
 * 쌓이지만, 추가로 "확인 안 한 에러 있음" 플래그를 세워서 ui_main.c가 화면에 토스트로
 * 바로 띄울 수 있게 함(2026-08-01 — 로그에만 남기고 화면엔 아무 표시 없던 게 오늘 헤맨
 * 근본 원인이라는 지적 반영, 실패 지점은 로그가 아니라 화면에서 바로 보여야 함).
 * code는 4자리 에러 코드(아래 UI_ERR_* 참고) — 메시지 앞에 "[NNNN] "으로 붙어서 로그/토스트에
 * 그대로 나옴, 나중에 코드만 보고 뭔지 바로 알아볼 용도(2026-08-01, 사용자 지시 — 엄격한
 * 규칙은 없고 개수가 적어서 앞자리로만 대충 구분: 1xxx 메모리 할당, 2xxx 통신 전송,
 * 3xxx 사진 수신/표시, 4xxx CAM 응답 실패, 5xxx 폰트/시스템 초기화, 9xxx 테스트용) */
#define UI_ERR_CACHE_TOO_BIG        1001  /* 사진이 캐시 슬롯 고정 용량보다 큼 */
#define UI_ERR_CACHE_NO_BUF         1002  /* 캐시 슬롯 버퍼가 없음(초기 할당 실패) */
#define UI_ERR_RECV_BUF_ALLOC       1003  /* 수신 버퍼 초기 할당 실패 */
#define UI_ERR_CACHE_SLOT_ALLOC     1004  /* 캐시 슬롯 초기 할당 실패 */
#define UI_ERR_PANEL_BUF_ALLOC      1005  /* 판넬 디코드 버퍼 초기 할당 실패 */

#define UI_ERR_SEND_PHOTO_REQ       2001  /* 사진 요청(PHOTO_REQUEST) 전송 실패 */
#define UI_ERR_SEND_CAPTURE_REQ     2002  /* 지금촬영 요청 전송 실패 */
#define UI_ERR_SEND_LIST_REQ        2003  /* 목록 요청 전송 실패 */
#define UI_ERR_SEND_DELETE_REQ      2004  /* 삭제 요청 전송 실패 */
#define UI_ERR_SEND_DELETE_ALL_REQ  2005  /* 전체삭제 요청 전송 실패 */
#define UI_ERR_REQUEST_BUSY         2006  /* 이미 수신 중이라 새 요청 무시됨 */
#define UI_ERR_NOT_PAIRED           2007  /* 요청 시점에 이미 언페어링 상태(desync 포함) —
                                             전송 자체를 안 하고 재연결을 시도함 */
#define UI_ERR_SLEEP_NOW_FAILED     2008  /* CAM이 SLEEP_NOW_REQUEST(재요청)를 임계치 이상
                                             반복해도 CNTL이 SLEEP_NOW를 못 전달함(2026-08-11) */

#define UI_ERR_META_TOO_BIG         3001  /* CAM이 보낸 사진이 고정 수신 버퍼보다 큼 */
#define UI_ERR_CHUNK_MISSING        3002  /* 청크 누락 — 재조립 실패 */
#define UI_ERR_CRC_MISMATCH         3003  /* CRC 불일치 — 재조립 실패 */
#define UI_ERR_DECODE_FAIL          3004  /* JPEG 디코드 실패(사진 표시 불가) */
#define UI_ERR_LIST_COUNT_MISMATCH  3005  /* 목록 항목 일부 유실 — 재요청/포기 */
#define UI_ERR_FETCH_NORESPONSE     3006  /* 사진 가져오기 요청 후 CAM 무응답(진행 정체) */
#define UI_ERR_LIST_NORESPONSE      3007  /* 목록 갱신 요청 후 CAM 무응답(타임아웃) */
#define UI_ERR_PHOTO_SELECTION_STALE 3008 /* 도착한 사진의 file_id가 지금 선택된 항목과 다름
                                             — 이전에 밀려난(대체된) 요청의 뒤늦은 응답.
                                             화면에는 안 그리고 지금 선택된 항목을 재요청함
                                             (2026-08-05, 선택-도착 불일치 경쟁 상태 방지용
                                             방어장치 — 정상적으론 거의 안 떠야 함, 뜨면
                                             모달 차단이 뚫린 것이므로 실제 버그로 취급) */

#define UI_ERR_DELETE_FAILED        4001  /* CAM이 삭제 실패로 응답 */
#define UI_ERR_DELETE_ALL_FAILED    4002  /* CAM이 전체삭제 실패로 응답 */
#define UI_ERR_CAPTURE_FAILED       4003  /* CAM이 촬영 실패로 응답 */
#define UI_ERR_CAPTURE_NORESPONSE   4004  /* 지금촬영 요청 후 CAM 무응답 */
#define UI_ERR_CONFIG_NORESPONSE    4005  /* 설정(CAM_CONFIG_SET) 적용 요청 후 CAM 무응답 */

#define UI_ERR_FONT_FILE_MISSING    5001  /* 폰트 파일 없음(stat 실패) */
#define UI_ERR_FONT_BUF_ALLOC       5002  /* 폰트 파일 로드용 PSRAM 할당 실패 */
#define UI_ERR_FONT_FILE_OPEN       5003  /* 폰트 파일 열기 실패 */
#define UI_ERR_FONT_CREATE          5004  /* TinyTTF 폰트 인스턴스 생성 실패 */
#define UI_ERR_HTTPD_START          5005  /* 웹서버(httpd_start) 시작 실패 */
#define UI_ERR_RTC_SET_FAILED       5006  /* 설정 화면 수동 시각설정 — RTC 하드웨어 쓰기 실패 */
#define UI_ERR_CONFIG_FILE_MISMATCH 5007  /* device_config.bin 버전/크기 불일치 — 기본값으로
                                             폴백됨(저장된 설정 유실). 2026-08-11 — 예전엔
                                             ESP_LOGW만 남기고 화면엔 표시가 없어서, 다른 설정을
                                             하나라도 Apply하면 그 폴백 기본값이 새 파일에
                                             그대로 눌러써져 영구화되는 사고로 이어짐(응답성이
                                             조용히 2로 굳어버림) — 화면에도 반드시 보이게 함 */

void ui_log_add_err(int code, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* 확인 안 한 에러가 있으면 out에 채우고 true+플래그 클리어, 없으면 false */
bool ui_log_get_pending_error(char *out, size_t out_cap);

/* 지금까지 쌓인(중복 제거된) 에러 코드 목록을 out_codes에 채우고 개수 반환 — 로고
 * 탭했을 때 팝업으로 전부 보여주는 용도(2026-08-01). out_codes 배열은 최소 이 크기로 */
#define UI_ERR_HISTORY_CAP 16
int ui_log_get_error_history(int *out_codes, int max);

/* code에 대응하는 짧은 설명 문자열(찾는 코드가 없으면 "알 수 없는 에러") */
const char *ui_log_err_desc(int code);

/* 2026-08-11 — 워닝 레벨(사용자 지시). 에러(UI_ERR_*)와 별개 체계: 에러는 아이콘이
 * 재부팅 전까지 계속 남지만, 워닝은 사용자가 팝업으로 확인하면 그 즉시 이력이 지워지고
 * 로고 아이콘도 정상으로 돌아옴 — "잠깐 있었지만 지금은 괜찮을 수도 있는" 신호라는
 * 성격 차이를 반영. 코드 네임스페이스는 에러와 안 겹치게 6xxx대(현재 유일한 용도:
 * SLEEP_NOW 재요청) */
#define UI_WARN_SLEEP_NOW_NORESPONSE 6001  /* CAM이 SLEEP_NOW 재요청(SLEEP_NOW_REQUEST)을
                                              보냄 — 이전에 보낸 SLEEP_NOW가 유실됐을 가능성 */

void ui_log_add_warn(int code, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
bool ui_log_get_pending_warn(char *out, size_t out_cap);
#define UI_WARN_HISTORY_CAP 16
int ui_log_get_warn_history(int *out_codes, int max);
const char *ui_log_warn_desc(int code);
/* 워닝 팝업을 닫을 때 호출 — 이력을 비워서 로고 아이콘이 정상으로 돌아가게 함(에러와
 * 다른 점, 위 주석 참고) */
void ui_log_clear_warn_history(void);
