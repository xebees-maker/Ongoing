"""상시 시리얼 로그 수집기(콘·브·캠) — 리셋 없이(DTR/RTS 끔) 받아 보드별·날짜별 파일로 저장.

2026-10-08(사용자 지시) — 캠이 끊긴 원인을 사후에 가르기 위해 PC에 연결된 보드 로그를 늘 받아 둠.
- 보드는 USB 일련번호(= 칩 MAC)로 찾음: COM 번호가 바뀌거나 딥슬립·재부팅으로 포트가 사라졌다 나타나도 이어서 받음.
- 파일: <로그 폴더>/<이름>/<YYYY-MM-DD>.log, 줄마다 PC 시각(밀리초). KEEP_DAYS보다 오래된 파일은 지움.
- 플래시할 때: <로그 폴더>/<이름>.pause 파일이 있으면 그 포트를 닫고 기다림(지우면 다시 받음).
사용: python serial_logger.py [로그 폴더]   (기본 C:/Projects/serial_logs)
"""
import os, sys, time, threading, datetime
import serial
import serial.tools.list_ports as list_ports

BOARDS = {                      # USB 일련번호 → 이름
    'E8:F6:0A:8E:86:68': 'cntl',
    'B8:1F:3F:C8:B5:84': 'bridge',
    '28:84:85:49:86:40': 'cam',
}
LOG_ROOT = sys.argv[1] if len(sys.argv) > 1 else r'C:/Projects/serial_logs'
KEEP_DAYS = 7


def find_port(sn):
    for p in list_ports.comports():
        if (p.serial_number or '').upper() == sn:
            return p.device
    return None


def prune(folder):
    cutoff = time.time() - KEEP_DAYS * 86400
    for f in os.listdir(folder):
        fp = os.path.join(folder, f)
        if f.endswith('.log') and os.path.getmtime(fp) < cutoff:
            try:
                os.remove(fp)
            except OSError:
                pass


def run(sn, name):
    folder = os.path.join(LOG_ROOT, name)
    os.makedirs(folder, exist_ok=True)
    pause_flag = os.path.join(LOG_ROOT, name + '.pause')
    out, out_day, buf, last_prune = None, None, b'', 0.0

    def write(text):
        nonlocal out, out_day, last_prune
        now = datetime.datetime.now()
        day = now.strftime('%Y-%m-%d')
        if day != out_day:
            if out:
                out.close()
            out = open(os.path.join(folder, day + '.log'), 'a', encoding='utf-8', buffering=1)
            out_day = day
        out.write(now.strftime('%H:%M:%S.%f')[:-3] + ' ' + text + '\n')
        if time.time() - last_prune > 3600:
            last_prune = time.time()
            prune(folder)

    while True:
        if os.path.exists(pause_flag):
            time.sleep(1)
            continue
        dev = find_port(sn)
        if not dev:
            time.sleep(1)
            continue
        try:
            s = serial.Serial()
            s.port, s.baudrate, s.timeout = dev, 115200, 0.3
            s.dtr = False
            s.rts = False
            s.open()
        except Exception:
            time.sleep(1)
            continue
        write('---- [logger] port open %s' % dev)
        try:
            while not os.path.exists(pause_flag):
                buf += s.read(4096)
                while b'\n' in buf:
                    line, buf = buf.split(b'\n', 1)
                    write(line.decode('utf-8', 'replace').rstrip('\r'))
        except Exception as e:
            write('---- [logger] port lost (%s)' % type(e).__name__)
        try:
            s.close()
        except Exception:
            pass
        if os.path.exists(pause_flag):
            write('---- [logger] paused')


if __name__ == '__main__':
    os.makedirs(LOG_ROOT, exist_ok=True)
    for sn, name in BOARDS.items():
        threading.Thread(target=run, args=(sn, name), daemon=True).start()
    while True:
        time.sleep(3600)
