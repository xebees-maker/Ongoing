/**
 * @file    romanize.h
 * @brief   한글 → 로마자(콘 화면 표시용)
 *
 * 2026-10-05(사용자 결정) — 웹에서 한글로 넣은 별명을 콘 화면(영문 비트맵 글꼴)에 보일 때만 로마자로 바꿈. 저장은 입력한 그대로.
 * 국어의 로마자 표기법 낱자 대응(음절마다 초성·중성·종성), 발음에 따른 변화 규칙은 없음("신라" → "sinra").
 * 예: "송풍기" → "songpunggi", "릴레이2" → "rilrei2". ASCII는 그대로, 한글 음절 밖의 다른 문자는 '?'.
 */
#pragma once

#include <stddef.h>

/* in(UTF-8) → out(ASCII, 항상 NUL로 끝남). 반환: 쓴 길이. out_cap이 모자라면 음절 단위로 자름 */
size_t romanize_hangul(const char *in, char *out, size_t out_cap);
