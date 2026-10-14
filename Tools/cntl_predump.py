"""콘 RAM 로그 확보 — 콘을 리셋(플래시·재시작·SD 포맷/재마운트)하기 전에 반드시 먼저 실행.

2026-10-14(사용자 지시) — 불특정 시점에 나는 에러를 잡으려고 콘에 RAM 로그(Dev Log, W/E 링)를 두었는데, Claude가 로그를 읽지
않고 콘을 여러 번 리셋해서 사후 분석 자료를 지웠음. 그래서 리셋 전에 이 스크립트로 받아 두는 것을 절차로 강제함
(Tools/wait_flash.py가 콘 포트면 자동으로 부르고, .claude/settings.json의 훅이 다른 경로의 리셋을 막음).

받는 것: /api/devlog_dump(Dev Log 전체), /api/devlog_dump?lvl=we(경고·에러 2000줄), /api/alarms(알림 목록)
저장: <로그 폴더>/cntl_dumps/<YYYYmmdd_HHMMSS>/ , 성공하면 <로그 폴더>/cntl_dumps/LAST_OK 를 갱신(훅이 이 시각을 봄)
실패(콘 응답 없음 등)하면 종료 코드 1 — 호출부는 리셋하지 말 것. 콘이 정말 죽어 있으면 시리얼 로그(serial_logs/cntl)로
대신하고, 그 판단은 사용자에게 먼저 알린 뒤에만.
사용: python cntl_predump.py [콘 주소] [쿠키 파일]   (기본 http://192.168.0.19 , <로그 폴더>/cntl_cookies.txt)
"""
import os, sys, time, datetime, http.cookiejar, urllib.request

LOG_ROOT = r'C:/Projects/serial_logs'
BASE = sys.argv[1] if len(sys.argv) > 1 else 'http://192.168.0.19'
COOKIES = sys.argv[2] if len(sys.argv) > 2 else os.path.join(LOG_ROOT, 'cntl_cookies.txt')
ITEMS = [('devlog.txt', '/api/devlog_dump'), ('devlog_we.txt', '/api/devlog_dump?lvl=we'), ('alarms.json', '/api/alarms')]


def main():
    cj = http.cookiejar.MozillaCookieJar()
    if os.path.exists(COOKIES):
        cj.load(COOKIES, ignore_discard=True, ignore_expires=True)
    op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cj))
    out_dir = os.path.join(LOG_ROOT, 'cntl_dumps', datetime.datetime.now().strftime('%Y%m%d_%H%M%S'))
    os.makedirs(out_dir, exist_ok=True)
    for name, path in ITEMS:
        try:
            data = op.open(BASE + path, timeout=30).read()
        except Exception as e:
            print('PREDUMP_FAIL %s: %s' % (path, e))
            return 1
        if not data:
            print('PREDUMP_FAIL %s: empty' % path)
            return 1
        with open(os.path.join(out_dir, name), 'wb') as f:
            f.write(data)
        print('  %s %d bytes' % (name, len(data)))
    with open(os.path.join(LOG_ROOT, 'cntl_dumps', 'LAST_OK'), 'w', encoding='utf-8') as f:
        f.write(out_dir + '\n' + time.strftime('%Y-%m-%d %H:%M:%S') + '\n')
    print('PREDUMP_OK ' + out_dir)
    return 0


if __name__ == '__main__':
    sys.exit(main())
