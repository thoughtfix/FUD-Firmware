# Convert a saved "shot" dump (RGB332 rows) to a 2x PNG.
# usage: shot2png.py SHOT.txt OUT.png
import sys, zlib, struct
txt=open(sys.argv[1],errors="replace").read(); out=sys.argv[2]
i=txt.rfind("[shot] BEGIN"); hdr=txt[i:].splitlines()[0].split(); w,h=int(hdr[2]),int(hdr[3])
rows={}
for ln in txt[i:].splitlines():
    if ln.startswith("R") and ":" in ln:
        k,v=ln.split(":",1); rows[int(k[1:])]=v.strip()
def rgb(v): r=(v>>5)&7; g=(v>>2)&7; b=v&3; return bytes((r*255//7,g*255//7,b*255//3))
S=2; raw=b""
for y in range(h):
    hx=rows.get(y,"00"*w); line=b"".join(rgb(int(hx[2*x:2*x+2] or "0",16))*S for x in range(w))
    raw+=(b"\x00"+line)*S
def ch(t,d): c=struct.pack(">I",len(d))+t+d; return c+struct.pack(">I",zlib.crc32(t+d)&0xffffffff)
png=b"\x89PNG\r\n\x1a\n"+ch(b"IHDR",struct.pack(">IIBBBBB",w*S,h*S,8,2,0,0,0))+ch(b"IDAT",zlib.compress(raw))+ch(b"IEND",b"")
open(out,"wb").write(png); print(out,w,h)
