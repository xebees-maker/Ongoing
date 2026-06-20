/**
 * @file    ui_styles.h
 * @brief   UI 공통 상수 정의 — 색상, 문자열, 레이아웃 수치
 */
#ifndef UI_STYLES_H
#define UI_STYLES_H

#include "ui_palette.h"

/* ════════════════════════════════════════════════════════════
 * 색상 — 팔레트 값 참조
 * ════════════════════════════════════════════════════════════ */
#define COLOR_BG                PALETTE_LIGHT_GRAY  ///< 화면 배경
#define COLOR_SURFACE           PALETTE_GRAY        ///< 컨테이너 배경 (사용 안함, 참고용)
#define COLOR_MODAL_BG          PALETTE_BLACK       ///< Modal 오버레이
#define COLOR_MODAL_BOX         0xE0E0E0            ///< Modal 박스

#define COLOR_BTN_NORMAL        PALETTE_DARK_GRAY   ///< 버튼 기본
#define COLOR_BTN_PRESSED       PALETTE_WHITE       ///< 버튼 눌림 — 반전
#define COLOR_BTN_DEFAULT       PALETTE_DARK_GRAY   ///< 기본 버튼 (별칭)

#define COLOR_TEXT_BTN          PALETTE_WHITE       ///< 버튼 글씨
#define COLOR_TEXT_BTN_PRESSED  PALETTE_BLACK       ///< 버튼 눌림 글씨 — 반전
#define COLOR_TEXT_UI           PALETTE_BLACK       ///< UI 일반 글씨

/* ════════════════════════════════════════════════════════════
 * 문자열
 * ════════════════════════════════════════════════════════════ */
#define STR_TAB_DASHBOARD       "종합"
#define STR_TAB_CHECK           "체크"
#define STR_TAB_TIMER           "타이머"
#define STR_TAB_NODES           "노드"

#define STR_NODES_EMPTY         "탐색 중..."
#define STR_NODES_AGO_FMT       "%lu초 전"

#define STR_TITLE_LIST          "색상 팔레트"
#define STR_TITLE_CHECK         "체크 박스"
#define STR_TITLE_TIMER         "타이머"

#define STR_LIST_SELECTED       "선택: %s (#%06X)"
#define STR_LIST_NONE           "선택: (없음)"

#define STR_CHECK_OPT1          "옵션 1"
#define STR_CHECK_OPT2          "옵션 2"
#define STR_CHECK_OPT3          "옵션 3"

#define STR_BTN_START           "시작"
#define STR_BTN_STOP            "종료"
#define STR_BTN_OK              "확인"

#define STR_MODAL_MSG           "시간 종료!"

/* ════════════════════════════════════════════════════════════
 * 레이아웃 수치
 * ════════════════════════════════════════════════════════════ */
#define TABVIEW_BAR_H           36

#define ARC_SIZE                110
#define ARC_ANGLE_START         135
#define ARC_ANGLE_END           45
#define ARC_VAL_MIN             0
#define ARC_VAL_MAX             100
#define ARC_VAL_INIT            1

#define SPINBOX_W               70
#define SPINBOX_H               44
#define SPINBOX_VAL_MIN         0
#define SPINBOX_VAL_MAX         100
#define SPINBOX_VAL_INIT        1
#define SPINBOX_DIGITS          3

#define BTN_PLUS_MINUS_SIZE     44
#define BTN_PLUS_MINUS_OFFSET_X 60
#define BTN_PLUS_MINUS_OFFSET_Y (-60)
#define BTN_START_STOP_W        80
#define BTN_START_STOP_H        48
#define BTN_START_OFFSET_X      (-46)
#define BTN_STOP_OFFSET_X       46
#define BTN_BOTTOM_OFFSET_Y     (-8)
#define BTN_OK_W                80
#define BTN_OK_H                36
#define BTN_OK_OFFSET_Y         (-8)

#define MODAL_BOX_W             160
#define MODAL_BOX_H             110
#define MODAL_BOX_RADIUS        10
#define MODAL_MSG_OFFSET_Y      10

#define TIMER_INTERVAL_MS       1000

/* 색상 팔레트 리스트 레이아웃 */
#define PALETTE_LIST_MARGIN     8
#define PALETTE_LIST_H          200
#define PALETTE_LIST_OFFSET_Y   24
#define PALETTE_ITEM_H          36
#define PALETTE_ITEM_PAD        4
#define PALETTE_BOX_W           24
#define PALETTE_BOX_H           24
#define PALETTE_BOX_OFFSET_X    2
#define PALETTE_LBL_OFFSET_X    (PALETTE_BOX_W + 8)
#define PALETTE_ITEM_RADIUS     4
#define PALETTE_BOX_RADIUS      2
#define PALETTE_BORDER_W        1
#define PALETTE_PRESSED_COLOR   0xBBBBBB

/* 공통 UI 수치 */
#define UI_ALIGN_OFFSET_NONE    0
#define UI_TITLE_OFFSET_Y       4
#define UI_LABEL_BOTTOM_OFFSET  (-4)
#define UI_BORDER_NONE          0
#define UI_PAD_NONE             0
#define UI_RADIUS_NONE          0
#define UI_OPA_HALF             LV_OPA_50
#define UI_CHECKBOX_OFFSET_X    12
#define UI_CHECKBOX_SPACING_Y   36
#define UI_CHECKBOX_START_Y     30
#define UI_ARC_OFFSET_Y         34

#endif /* UI_STYLES_H */
