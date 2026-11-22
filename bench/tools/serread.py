# Read a FUD serial port for SECS seconds, sending each CMD once after 1 s.
# usage: serread.py PORT SECS [CMD ...]      e.g. serread.py /dev/cu.usbmodemXXXX 6 state gps
import serial, sys, time
port, secs = sys.argv[1], float(sys.argv[2]); cmds = sys.argv[3:]
s = serial.Serial(port, 115200, timeout=0.2)
t0 = time.time(); sent = False
while time.time() - t0 < secs:
    if cmds and not sent and time.time() - t0 > 1.0:
        for c in cmds: s.write((c + "\n").encode()); time.sleep(0.3)
        sent = True
    d = s.read(4096)
    if d: sys.stdout.write(d.decode("utf-8", "replace")); sys.stdout.flush()
