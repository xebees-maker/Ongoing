"""
콘 웹앱(SPA) 빌드 — 2026-10-03(할 일 AD).

app.html의 /*@STRINGS@*/ 자리에 콘 ui_strings.c의 한/영 문구 표 전체를 넣고 gzip해서 out/app.html.gz를 만듦.
문구를 손으로 옮기지 않으므로 콘 문구가 바뀌어도 웹이 그대로 따라감(설계: 문구 키 = 콘 STR_ 이름).

사용: python build_web.py            → out/app.html, out/app.html.gz
배포: curl -b <쿠키> -X POST --data-binary @out/app.html.gz "http://<콘>/admin/upload?file=app.html.gz"
"""
import gzip
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
STRINGS_C = os.path.join(HERE, '..', 'main', 'ui_strings.c')
SRC = os.path.join(HERE, 'app.html')
OUT_DIR = os.path.join(HERE, 'out')
MARKER = '/*@STRINGS@*/'
# 2026-10-05 — 에러·경고 코드 → 설명 문구 키. 콘 ui_main.c의 err/warn_code_to_desc_str와 ui_log.h의 코드 번호에서 뽑음(손으로 안 옮김)
ERRDESC_MARKER = '/*@ERRDESC@*/'
UI_LOG_H = os.path.join(HERE, '..', 'main', 'ui_log.h')
UI_MAIN_C = os.path.join(HERE, '..', 'main', 'ui_main.c')


def c_literal_bytes(body):
    """따옴표 안쪽 C 문자열 내용 → bytes(\\xNN, \\n, \\t, \\", \\\\, 8진수 처리)"""
    out = bytearray()
    i = 0
    while i < len(body):
        ch = body[i]
        if ch != '\\':
            out += ch.encode('utf-8')
            i += 1
            continue
        nxt = body[i + 1]
        if nxt == 'x':
            j = i + 2
            while j < len(body) and j < i + 4 and body[j] in '0123456789abcdefABCDEF':
                j += 1
            out.append(int(body[i + 2:j], 16))
            i = j
        elif nxt in '01234567':
            j = i + 1
            while j < len(body) and j < i + 4 and body[j] in '01234567':
                j += 1
            out.append(int(body[i + 1:j], 8))
            i = j
        else:
            out += {'n': b'\n', 't': b'\t', '"': b'"', '\\': b'\\', "'": b"'", 'r': b'\r'}.get(nxt, nxt.encode())
            i += 2
    return bytes(out)


def parse_strings(path):
    """[STR_X] = { "ko" "..." , "en" } 형태(여러 줄·이어 붙인 리터럴 포함) → {STR_X: [ko, en]}"""
    src = open(path, encoding='utf-8').read()
    src = re.sub(r'/\*.*?\*/', lambda m: ' ' * len(m.group(0)) if '"' not in m.group(0) else ' ', src, flags=re.S)
    table = {}
    for m in re.finditer(r'\[(STR_\w+)\]\s*=\s*\{', src):
        key = m.group(1)
        i = m.end()
        parts, cur = [], bytearray()
        while i < len(src):
            c = src[i]
            if c == '"':
                j = i + 1
                while src[j] != '"':
                    j += 2 if src[j] == '\\' else 1
                cur += c_literal_bytes(src[i + 1:j])
                i = j + 1
            elif c == ',':
                parts.append(bytes(cur))
                cur = bytearray()
                i += 1
            elif c == '}':
                parts.append(bytes(cur))
                break
            elif c == '/' and src[i:i + 2] == '//':
                i = src.index('\n', i)
            else:
                i += 1
        parts = [p for p in parts if p is not None]
        if len(parts) >= 2:
            table[key] = [parts[0].decode('utf-8'), parts[1].decode('utf-8')]
    return table


def parse_err_desc():
    """{"E": {코드: STR_키}, "W": {...}, "E_UNKNOWN": 키, "W_UNKNOWN": 키}"""
    codes = {m.group(1): int(m.group(2)) for m in re.finditer(r'#define\s+(UI_(?:ERR|WARN)_\w+)\s+(\d+)', open(UI_LOG_H, encoding='utf-8').read())}
    src = open(UI_MAIN_C, encoding='utf-8').read()
    out = {}
    for kind, fn in (('E', 'err_code_to_desc_str'), ('W', 'warn_code_to_desc_str')):
        body = re.search(r'static ui_str_id_t ' + fn + r'\(int code\)\s*\{(.*?)\n\}', src, re.S)
        if not body:
            sys.exit('%s not found in ui_main.c' % fn)
        out[kind] = {codes[m.group(1)]: m.group(2) for m in re.finditer(r'case\s+(UI_\w+):\s*return\s+(STR_\w+);', body.group(1))}
        dm = re.search(r'default:\s*return\s+(STR_\w+);', body.group(1))
        out[kind + '_UNKNOWN'] = dm.group(1) if dm else ''
    return out


def main():
    table = parse_strings(STRINGS_C)
    html = open(SRC, encoding='utf-8').read()
    errdesc = parse_err_desc()
    bad = sorted(set(k for kind in ('E', 'W') for k in errdesc[kind].values()) - set(table))
    if bad:
        sys.exit('error description keys not in ui_strings.c: ' + ', '.join(bad))
    if ERRDESC_MARKER not in html:
        sys.exit('marker %s not found in app.html' % ERRDESC_MARKER)
    html = html.replace(ERRDESC_MARKER, json.dumps(errdesc, separators=(',', ':')))
    # 2026-10-03 — SPA가 쓰는 콘 문구 키가 표에 있는지(없으면 화면에 키 이름이 그대로 보임 — STR_BTN_OK 사고)
    used = set(re.findall(r"'(STR_[A-Z0-9_]+)'", html)) | set(re.findall(r'data-s="(STR_[A-Z0-9_]+)"', html))
    missing = sorted(k for k in used if k not in table)
    if missing:
        sys.exit('string keys not in ui_strings.c: ' + ', '.join(missing))
    if MARKER not in html:
        sys.exit('marker %s not found in app.html' % MARKER)
    html = html.replace(MARKER, json.dumps(table, ensure_ascii=False, separators=(',', ':')))
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(os.path.join(OUT_DIR, 'app.html'), 'w', encoding='utf-8') as f:
        f.write(html)
    # 2026-10-03 — 깨진 JS가 콘에 올라간 적이 있어(heredoc이 '\n'을 줄바꿈으로 바꿈) 스크립트 문법 검사. node가 없으면 건너뜀
    import shutil
    import subprocess
    if shutil.which('node'):
        chk = "const h=require('fs').readFileSync(process.argv[1],'utf8');" \
              "[...h.matchAll(/<script>([\\s\\S]*?)<\\/script>/g)].forEach(x=>new Function(x[1]));"
        r = subprocess.run(['node', '-e', chk, os.path.join(OUT_DIR, 'app.html')], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit('JS syntax error:\n' + r.stderr[-800:])
    else:
        print('node not found - JS syntax check skipped')
    gz = gzip.compress(html.encode('utf-8'), compresslevel=9, mtime=0)
    with open(os.path.join(OUT_DIR, 'app.html.gz'), 'wb') as f:
        f.write(gz)
    print('strings=%d html=%dB gz=%dB' % (len(table), len(html.encode('utf-8')), len(gz)))


if __name__ == '__main__':
    main()
