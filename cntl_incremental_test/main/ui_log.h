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

#define UI_ERR_META_TOO_BIG         3001  /* CAM이 보낸 사진이 고정 수신 버퍼보다 큼 */
#define UI_ERR_CHUNK_MISSING        3002  /* 청크 누락 — 재조립 실패 */
#define UI_ERR_CRC_MISMATCH         3003  /* CRC 불일치 — 재조립 실패 */
#define UI_ERR_DECODE_FAIL          3004  /* JPEG 디코드 실패(사진 표시 불가) */

#define UI_ERR_DELETE_FAILED        4001  /* CAM이 삭제 실패로 응답 */
#define UI_ERR_DELETE_ALL_FAILED    4002  /* CAM이 전체삭제 실패로 응답 */
#define UI_ERR_CAPTURE_FAILED       4003  /* CAM이 촬영 실패로 응답 */

#define UI_ERR_FONT_FILE_MISSING    5001  /* 폰트 파일 없음(stat 실패) */
#define UI_ERR_FONT_BUF_ALLOC       5002  /* 폰트 파일 로드용 PSRAM 할당 실패 */
#define UI_ERR_FONT_FILE_OPEN       5003  /* 폰트 파일 열기 실패 */
#define UI_ERR_FONT_CREATE          5004  /* TinyTTF 폰트 인스턴스 생성 실패 */
#define UI_ERR_HTTPD_START          5005  /* 웹서버(httpd_start) 시작 실패 */

#define UI_ERR_TEST_FORCED          9001  /* 설정탭 "시험" 버튼 — 강제 발생 */

void ui_log_add_err(int code, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* 확인 안 한 에러가 있으면 out에 채우고 true+플래그 클리어, 없으면 false */
bool ui_log_get_pending_error(char *out, size_t out_cap);

/* 지금까지 쌓인(중복 제거된) 에러 코드 목록을 out_codes에 채우고 개수 반환 — 로고
 * 탭했을 때 팝업으로 전부 보여주는 용도(2026-08-01). out_codes 배열은 최소 이 크기로 */
#define UI_ERR_HISTORY_CAP 16
int ui_log_get_error_history(int *out_codes, int max);

/* code에 대응하는 짧은 설명 문자열(찾는 코드가 없으면 "알 수 없는 에러") */
const char *ui_log_err_desc(int code);
