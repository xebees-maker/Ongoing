#!/usr/bin/env python3
"""
managed_components/lvgl__lvgl/src/libs/tiny_ttf/lv_tiny_ttf.c 의 kerning 버그 패치.

managed_components는 gitignore 대상이라, ESP-IDF 컴포넌트 매니저가 lvgl을 다시
받을 때마다(락파일 재해석, 클린 빌드 등) 이 파일의 로컬 수정이 통째로 사라진다.
2026-06-16에 처음 고쳤다가 2026-06-21에 정확히 같은 버그로 재발한 적이 있음 —
그래서 패치를 git에 들어가는 이 스크립트로 관리하고, 루트 CMakeLists.txt가 매
configure마다 멱등적으로(이미 패치돼 있으면 그대로 둠) 재적용한다.

버그 내역(둘 다 LVGL 9 TinyTTF 구현 버그, 우리가 만든 코드 아님):
1. ttf_get_glyph_pair_kerning_width()가 kerning 엔트리를 해제할 때
   glyph_cache를 release해서(kerning_cache가 맞음) glyph 캐시를 오염시킴 —
   화면 글자 자간이 깨지는 원인.
2. tiny_ttf_glyph_cache_create_cb()의 NONE/NORMAL 분기 조건이 반전되어 있었음.
   두 분기 모두 같은 값을 계산하므로 조건 자체를 제거.
"""
import sys

PATCHES = [
    (
        "lv_cache_release(dsc->glyph_cache, kerning_entry, NULL);",
        "lv_cache_release(dsc->kerning_cache, kerning_entry, NULL);",
    ),
    (
        "    if(dsc->kerning != LV_FONT_KERNING_NORMAL) { /* calculate default advance */\n"
        "        dsc_out->adv_w = ttf_get_glyph_pair_kerning_width(dsc, g1, 0, advw);\n"
        "    }\n"
        "    else {\n"
        "        dsc_out->adv_w = ttf_calculate_kerning_width(dsc->scale, advw, 0);\n"
        "    }\n",
        "    /* NONE/NORMAL branches compute the same value - the branch itself was the bug (2026-06-16) */\n"
        "    dsc_out->adv_w = ttf_calculate_kerning_width(dsc->scale, advw, 0);\n",
    ),
]


def main():
    # ASCII-only stdout: CMake's execute_process on Windows captures this with the
    # console's legacy codepage, not UTF-8 - non-ASCII text here raises UnicodeEncodeError
    # and aborts the configure step.
    if len(sys.argv) != 2:
        print("usage: fix_tiny_ttf_kerning.py <path to lv_tiny_ttf.c>", file=sys.stderr)
        return 1

    path = sys.argv[1]
    try:
        with open(path, "r", encoding="utf-8") as f:
            src = f.read()
    except FileNotFoundError:
        print(f"[fix_tiny_ttf_kerning] {path} not found - lvgl component not fetched yet, skipping")
        return 0

    changed = False
    for old, new in PATCHES:
        if old in src:
            src = src.replace(old, new)
            changed = True
        elif new not in src:
            print(f"[fix_tiny_ttf_kerning] WARNING: patch target pattern not found "
                  f"(lvgl version may have changed): {old[:50]!r}...")

    if changed:
        with open(path, "w", encoding="utf-8") as f:
            f.write(src)
        print(f"[fix_tiny_ttf_kerning] patch (re)applied: {path}")
    else:
        print(f"[fix_tiny_ttf_kerning] already patched: {path}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
