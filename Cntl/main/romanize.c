#include "romanize.h"

#include <stdint.h>
#include <string.h>

/* 유니코드 한글 음절(U+AC00~U+D7A3) = 0xAC00 + (초성 * 21 + 중성) * 28 + 종성 */
static const char *const s_initial[19] = {
    "g", "kk", "n", "d", "tt", "r", "m", "b", "pp", "s", "ss", "", "j", "jj", "ch", "k", "t", "p", "h",
};
static const char *const s_medial[21] = {
    "a", "ae", "ya", "yae", "eo", "e", "yeo", "ye", "o", "wa", "wae", "oe", "yo", "u", "wo", "we", "wi", "yu", "eu", "ui", "i",
};
/* 받침은 대표음(ㄱ류 k, ㄷ류 t, ㅂ류 p, ㄹ l) — 겹받침도 대표음 하나 */
static const char *const s_final[28] = {
    "", "k", "k", "k", "n", "n", "n", "t", "l", "k", "m", "l", "l", "l", "p", "l",
    "m", "p", "p", "t", "t", "ng", "t", "t", "k", "t", "p", "t",
};

static size_t append(char *out, size_t len, size_t cap, const char *s)
{
    size_t n = strlen(s);
    if (len + n + 1 > cap) return (size_t)-1;
    memcpy(out + len, s, n);
    return len + n;
}

size_t romanize_hangul(const char *in, char *out, size_t out_cap)
{
    if (!out || out_cap == 0) return 0;
    size_t len = 0;
    const uint8_t *p = (const uint8_t *)(in ? in : "");
    while (*p) {
        uint32_t cp;
        int n;
        if (p[0] < 0x80)                    { cp = p[0]; n = 1; }
        else if ((p[0] & 0xE0) == 0xC0)    { cp = p[0] & 0x1F; n = 2; }
        else if ((p[0] & 0xF0) == 0xE0)    { cp = p[0] & 0x0F; n = 3; }
        else if ((p[0] & 0xF8) == 0xF0)    { cp = p[0] & 0x07; n = 4; }
        else                                { cp = '?'; n = 1; }
        for (int k = 1; k < n; k++) {
            if ((p[k] & 0xC0) != 0x80) { cp = '?'; n = k; break; }  /* 깨진 UTF-8 — 거기까지만 한 글자로 */
            cp = (cp << 6) | (p[k] & 0x3F);
        }
        p += n;

        size_t next;
        if (cp < 0x80) {
            char one[2] = { (char)cp, 0 };
            next = append(out, len, out_cap, one);
        } else if (cp >= 0xAC00 && cp <= 0xD7A3) {
            uint32_t idx = cp - 0xAC00;
            next = append(out, len, out_cap, s_initial[idx / 588]);
            if (next != (size_t)-1) next = append(out, next, out_cap, s_medial[(idx % 588) / 28]);
            if (next != (size_t)-1) next = append(out, next, out_cap, s_final[idx % 28]);
        } else {
            next = append(out, len, out_cap, "?");
        }
        if (next == (size_t)-1) break;  /* 자리가 모자람 — 음절 단위로 자름 */
        len = next;
    }
    out[len] = 0;
    return len;
}
