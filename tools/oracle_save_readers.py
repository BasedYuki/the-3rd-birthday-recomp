"""In PPSSPP, load the game save (pressing CROSS/START until the game's stream reader at
0x08b1072c sees the 128 KB save), then put a READ breakpoint on <hexoff> inside that buffer
and print the pc / ra / backtrace of each read.

    python tools/oracle_save_readers.py <hexoff> [hits] [size]
"""

import base64
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppsspp_ws import Dbg

off = int(sys.argv[1], 16)
hits = int(sys.argv[2]) if len(sys.argv) > 2 else 10
size = int(sys.argv[3]) if len(sys.argv) > 3 else 4
d = Dbg(45679)
BP = 0x08B1072C


def rd(addr, n):
    return base64.b64decode(d.request("memory.read", address=addr, size=n, timeout=20)["base64"])


def regs():
    c = d.request("cpu.getAllRegs")["categories"][0]
    return dict(zip(c["registerNames"], c["uintValues"]))


d.request("cpu.breakpoint.add", address=BP, enabled=True)
i = 0
base = None
end = time.time() + 300
while time.time() < end and base is None:
    if not d.request("cpu.status").get("stepping"):
        d.ticket += 1
        d._send(json.dumps(dict(event="input.buttons.press", ticket=str(d.ticket),
                                button="cross" if i % 3 else "start", duration=6)))
        i += 1
        time.sleep(0.7)
        continue
    s1 = regs()["s1"]
    b, sz = struct.unpack("<II", rd(s1 + 8, 8))
    if sz == 0x20000 and struct.unpack("<I", rd(b, 4))[0] == 0x14:
        base = b
        d.request("cpu.breakpoint.remove", address=BP)
        d.request("memory.breakpoint.add", address=base + off, size=size, read=True, write=False,
                  change=False, enabled=True, log=False)
        print(f"buffer 0x{base:08x}; watching reads of +0x{off:x} (0x{base + off:08x})", flush=True)
    d.request("cpu.resume", expect="cpu.resume")
if base is None:
    print("save load not seen"); sys.exit(1)
try:
    for h in range(hits):
        t_end = time.time() + 30
        got = False
        while time.time() < t_end and not got:
            got = bool(d.request("cpu.status").get("stepping"))
            if not got:
                time.sleep(0.2)
        if not got:
            print("no more reads within 30 s"); break
        r = regs()
        print(f"hit {h}: pc=0x{r['pc']:08x} ra=0x{r['ra']:08x} a0=0x{r['a0']:08x} a1=0x{r['a1']:08x} a2=0x{r['a2']:08x}", flush=True)
        for f in d.request("hle.backtrace").get("frames", [])[:6]:
            print(f"    frame entry=0x{f.get('entry', 0):08x} pc=0x{f.get('pc', 0):08x}")
        d.request("cpu.resume", expect="cpu.resume")
finally:
    d.request("memory.breakpoint.remove", address=base + off, size=size)
    if d.request("cpu.status").get("stepping"):
        d.request("cpu.resume", expect="cpu.resume")
