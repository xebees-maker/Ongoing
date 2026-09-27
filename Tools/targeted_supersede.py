"""5640 자동 사진(kind=T)의 SR_META가 콘 로그에 찍히면 1.5초 뒤 수동 촬영 — 전송 도중 대체를 확실히 일으킴. 10회.
사용: targeted_supersede.py <serlog.py가 쓰는 콘 로그 파일> <결과 파일> [콘 IP]
전제: 콘 테스트 API(/api/capture_now, /api/devlog — 할 일 24, 나중에 제거). MAC·IP는 임시 PC 기준(2026-09-27)"""
import time, urllib.request, re, os, sys
LOG = sys.argv[1]
OUT = sys.argv[2]
BASE = 'http://' + (sys.argv[3] if len(sys.argv) > 3 else '192.168.50.131')
def get(path):
    try: return urllib.request.urlopen(BASE + path, timeout=5).read().decode()
    except Exception as e: return 'ERR %s' % e
def log(s):
    with open(OUT, 'a') as f: f.write(time.strftime('%H:%M:%S ') + s + '\n')
log('devlog ' + get('/api/devlog?save=D'))
pos = os.path.getsize(LOG)
pat = re.compile(r'SR_META 28:84:85:49:85:30: .*kind=T')
n = 0
deadline = time.time() + 900
while n < 10 and time.time() < deadline:
    time.sleep(0.2)
    with open(LOG, encoding='utf-8', errors='replace') as f:
        f.seek(pos); new = f.read(); pos = f.tell()
    for line in new.splitlines():
        if pat.search(line):
            time.sleep(1.5)
            n += 1
            log('trial#%d after [%s] -> %s' % (n, line[:40], get('/api/capture_now?mac=288485498530')))
            time.sleep(20)  # 수동 사진 전송이 끝날 때까지
            with open(LOG, encoding='utf-8', errors='replace') as f:
                f.seek(0, 2); pos = f.tell()
            break
log('done devlog ' + get('/api/devlog?save=I'))
