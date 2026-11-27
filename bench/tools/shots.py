# Screenshots: wait BOOT s, then "go <screen>" + "shot" for each screen, saving OUTDIR/<name>.txt.
# usage: shots.py PORT OUTDIR BOOT go_home go_status ...   (needs a bench build, FUD_BENCH)
import serial, sys, time
port, outdir, boot = sys.argv[1], sys.argv[2], float(sys.argv[3]); screens = sys.argv[4:]
s = serial.Serial(port, 115200, timeout=0.2)
t0=time.time()
while time.time()-t0 < boot: s.read(4096)
for sc in screens:
    cmds = sc.split("+")
    for c in cmds: s.write((c.replace("_"," ")+"\n").encode()); time.sleep(1.2)
    s.reset_input_buffer(); s.write(b"shot\n"); buf=b""; end=time.time()+15
    while time.time()<end and b"[shot] END" not in buf: buf+=s.read(65536)
    name = cmds[-1].replace("_","-")
    open(f"{outdir}/{name}.txt","wb").write(buf); print(name, len(buf))
