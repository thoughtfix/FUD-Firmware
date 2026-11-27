# Two-device bench run: log RX and TX ports together and send timed commands.
# usage: duo.py RX_PORT TX_PORT T=dev:cmd ...   e.g. "3=tx:flipper 30" "40=rx:state"
import serial, sys, time
rx = serial.Serial(sys.argv[1], 115200, timeout=0.05); tx = serial.Serial(sys.argv[2], 115200, timeout=0.05)
plan = [(float(a.split('=')[0]), a.split('=')[1]) for a in sys.argv[3:]]
t0 = time.time(); bufs = {"RX": b"", "TX": b""}; end = plan[-1][0] + 8
while time.time() - t0 < end:
    t = time.time() - t0
    while plan and t >= plan[0][0]:
        _, c = plan.pop(0); dev, cmd = c.split(':', 1)
        (tx if dev == "tx" else rx).write((cmd + "\n").encode()); print("%6.1f >>> %s %s" % (t, dev.upper(), cmd), flush=True)
    for name, s in (("RX", rx), ("TX", tx)):
        bufs[name] += s.read(4096)
        while b"\n" in bufs[name]:
            ln, bufs[name] = bufs[name].split(b"\n", 1)
            print("%6.1f %s %s" % (t, name, ln.decode("utf-8", "replace").rstrip()), flush=True)
