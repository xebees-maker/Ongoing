"""시리얼 두 개를 계속 파일로 기록(리셋 안 함). 줄마다 PC 시각을 붙임. 사용: serlog.py COM12 c.txt COM13 b.txt"""
import serial, sys, threading, time, datetime
def run(port, path):
    while True:
        try:
            # 포트를 열 때 리셋이 걸리지 않게 DTR/RTS를 먼저 끄고 엶
            s = serial.Serial()
            s.port = port; s.baudrate = 115200; s.timeout = 0.5
            s.dtr = True; s.rts = False   # USB-Serial-JTAG은 DTR이 꺼져 있으면 출력이 멈춤, RTS만 끄면 리셋 안 걸림
            s.open()
            buf = b''
            with open(path, 'a', encoding='utf-8', buffering=1) as f:
                while True:
                    buf += s.read(4096)
                    while b'\n' in buf:
                        line, buf = buf.split(b'\n', 1)
                        ts = datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]
                        f.write(ts + ' ' + line.decode('utf-8', 'replace').rstrip('\r') + '\n')
        except Exception as e:
            with open(path, 'a', encoding='utf-8') as f: f.write('#### serial error: %s\n' % e)
            time.sleep(2)
args = sys.argv[1:]
for i in range(0, len(args), 2):
    threading.Thread(target=run, args=(args[i], args[i + 1]), daemon=True).start()
while True: time.sleep(60)
