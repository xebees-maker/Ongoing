/**
 * @file    ui_palette.h
 * @brief   16색 표준 팔레트 정의
 *          UI 전체에서 색상 참조 시 이 파일을 사용할 것
 */
#ifndef UI_PALETTE_H
#define UI_PALETTE_H

#include <stdint.h>

/* ════════════════════════════════════════════════════════════
 * 팔레트 색상 hex 값 정의
 * ════════════════════════════════════════════════════════════ */
#define PALETTE_WHITE           0xFFFFFF
#define PALETTE_BLACK           0x000000
#define PALETTE_LIGHT_GRAY      0xD3D3D3
#define PALETTE_GRAY            0x808080
#define PALETTE_DARK_GRAY       0x404040
#define PALETTE_RED             0xFF0000
#define PALETTE_GREEN           0x00FF00
#define PALETTE_YELLOW          0xFFFF00
#define PALETTE_BLUE            0x0000FF
#define PALETTE_MAGENTA         0xFF00FF
#define PALETTE_PURPLE          0x800080
#define PALETTE_ORANGE          0xFF8000
#define PALETTE_CYAN            0x00FFFF
#define PALETTE_TEAL            0x008080
#define PALETTE_OLIVE           0x808000
#define PALETTE_LIME            0x00FF80

/* ════════════════════════════════════════════════════════════
 * 팔레트 아이템 구조체
 * ════════════════════════════════════════════════════════════ */
typedef struct {
    const char *name;
    uint32_t    color;
} ColorItem;

/* ════════════════════════════════════════════════════════════
 * 팔레트 배열 (extern — ui_palette.c에서 정의)
 * ════════════════════════════════════════════════════════════ */
extern const ColorItem COLOR_PALETTE[];
extern const int       COLOR_PALETTE_COUNT;

#endif /* UI_PALETTE_H */
