"""SD 개선 효과 확인(읽기만) — 2026-10-15.

사용: python Tools/sd_effect_check.py [기준 sd_health JSON]
  콘(192.168.0.19)의 /api/sd/health, /api/stats/sync, /api/stats/restore, 사진 목록을 받아
  serial_logs/sd_effect/now_<시각>_*.json으로 저장하고 요약을 출력.
  기준 파일을 주면(예: serial_logs/sd_effect/base_20261015_1651_sd_health.json) 그 뒤로 늘어난
  SD 쓰기 카운터(fsync·create·rename·unlink·bytes)를 시간당으로 보여 줌 — 같은 날(today) 안에서만 뺌.
보는 것:
  1) SD 쓰기 횟수 — 2단계(통계 1시간 모아쓰기)·3단계(사진 칸 파일) 뒤 create/rename은 거의 0, fsync는 사진 1장당 1 + 매시 몇 번
  2) SD 이상 사건(events) — 부트 영역 변경·디렉터리·통계 레코드·사진 CRC. 새로 생겼으면 그 내용
  3) 명령마다 CS 수치(cs: 실패 0이어야 함)
  4) 내부 플래시 → SD 옮기기(sync: 실패·남은 수), 백업 마지막 시각
  5) 사진 수(칸 사진이 계속 늘어나는지)
"""
import json, os, sys, time, urllib.request, http.cookiejar

BASE = 'http://192.168.0.19'
OUT = r'C:\Projects\serial_logs\sd_effect'
cj = http.cookiejar.MozillaCookieJar(r'C:\Projects\serial_logs\cntl_cookies.txt')
cj.load(ignore_discard=True, ignore_expires=True)
op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cj))


def get(path):
    return json.loads(op.open(BASE + path, timeout=30).read())


os.makedirs(OUT, exist_ok=True)
stamp = time.strftime('%Y%m%d_%H%M')
now = {
    'sd_health': get('/api/sd/health'),
    'stats_sync': get('/api/stats/sync'),
    'stats_restore': get('/api/stats/restore'),
    'photos': get('/api/cam/photos?mac=288485498640'),
}
for k, v in now.items():
    json.dump(v, open(os.path.join(OUT, 'now_%s_%s.json' % (stamp, k)), 'w'))

h = now['sd_health']
print('card   :', h['card'])
print('fault  :', h['fault'], '| events total', h['events_total'])
for e in h['events'][:8]:
    print('   event %s t=%s off=%s %s' % (e['ev'], time.strftime('%m-%d %H:%M', time.gmtime(e['t'])), e['off'], e['path']))
print('today  :', h['today'])
print('yester :', h['yesterday'])
print('cs     :', h.get('cs'))
s = now['stats_sync']
print('sync   : n %(n)s fail %(fail)s recs %(recs)s buckets %(buckets)s last_ms %(ms)s' % s['sync'],
      '| flash unsynced', s['flash']['unsynced'], 'days', s['flash']['days'], 'write fail', s['flash']['fail'])
r = now['stats_restore']
print('backup : last', time.strftime('%m-%d %H:%M', time.gmtime(r['last_backup'])) if r['last_backup'] else 'never', '| restore state', r['state'])
print('photos : count', now['photos']['count'])

if len(sys.argv) > 1:
    b = json.load(open(sys.argv[1]))
    bt = os.path.getmtime(sys.argv[1])
    hours = max((time.time() - bt) / 3600.0, 0.01)
    if b['day'] == h['day']:
        print('\nsince baseline (%.1f h, same day):' % hours)
        for k in ('fsync', 'create', 'rename', 'unlink', 'bytes', 'verify_ok', 'fat_check'):
            d = h['today'][k] - b['today'][k]
            print('   %-10s +%-10d %8.1f /h' % (k, d, d / hours))
    else:
        print('\nbaseline is from another day (%s vs %s) - compare "yesterday" instead' % (b['day'], h['day']))
    nb = b['events_total']
    if h['events_total'] > nb:
        print('NEW SD EVENTS since baseline:', h['events_total'] - nb)
