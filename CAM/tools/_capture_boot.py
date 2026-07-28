import serial, time, sys

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
outpath = sys.argv[2] if len(sys.argv) > 2 else "_boot_log.txt"

ser = serial.Serial(port, 115200, timeout=0.5)
ser.reset_input_buffer()
ser.setRTS(True)
time.sleep(0.1)
ser.setRTS(False)

buf = b""
end = time.time() + 8
while time.time() < end:
    data = ser.read(4096)
    if data:
        buf += data
ser.close()

with open(outpath, "wb") as f:
    f.write(buf)

print(f"captured {len(buf)} bytes -> {outpath}")
