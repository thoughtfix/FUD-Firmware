# Stability run: send "state" every EVERY s for SECS s and print uptime, heap and any crash lines.
# usage: stab.py PORT SECS EVERY
import serial, sys, time
port, secs, every = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
s = serial.Serial(port, 115200, timeout=0.2)
t0 = time.time(); nxt = t0 + 5; buf = b""
while time.time() - t0 < secs:
    if time.time() >= nxt: s.write(b"state\n"); nxt += every
    buf += s.read(4096)
    while b"\n" in buf:
        ln, buf = buf.split(b"\n", 1); t = ln.decode("utf-8", "replace").rstrip()
        if t.startswith("[state] det"): continue
        if any(k in t for k in ("[state] up", "[state] counts", "rst:", "Guru", "panic", "Backtrace", "abort", "[boot]", "FUD", "heap", "assert")):
            print("%6.0f %s" % (time.time() - t0, t), flush=True)
