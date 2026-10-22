"""Dump a decrypted game save from PPSSPP: the game reads 3RDDATA.BIN through a memory-stream
reader (0x08b1072c, s1 = stream {+8 base, +0xc size, +0x10 pos}); break there, and when the
stream is the 128 KB game save, write it out. Presses CROSS/START meanwhile so the title menu
loads the save.

    python tools/oracle_save_dump.py <out.bin> [port]     (WANT_SYS=1: the first SYSDATA read)
"""

import base64
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppsspp_ws import Dbg

out = sys.argv[1]
d = Dbg(int(sys.argv[2]) if len(sys.argv) > 2 else 45679)
BP = 0x08B1072C
want_sys = os.environ.get("WANT_SYS") == "1"   # dump the 16 KB SYSDATA read instead


def rd(addr, n):
    buf = bytearray()
    for a in range(addr, addr + n, 0x10000):
        k = min(0x10000, addr + n - a)
        buf += base64.b64decode(d.request("memory.read", address=a, size=k, timeout=20)["base64"])
    return bytes(buf)


d.request("cpu.breakpoint.add", address=BP, enabled=True)
try:
    end = time.time() + 600
    i = 0
    while time.time() < end:
        st = d.request("cpu.status")
        if not st.get("stepping"):
            d.ticket += 1   # fire and forget: v1.20 may answer a press only after it ends
            d._send(json.dumps(dict(event="input.buttons.press", ticket=str(d.ticket),
                                    button="cross" if i % 3 else "start", duration=6)))
            i += 1
            time.sleep(0.7)
            continue
        regs = d.request("cpu.getAllRegs")["categories"][0]
        s1 = dict(zip(regs["registerNames"], regs["uintValues"]))["s1"]
        base, size = struct.unpack("<II", rd(s1 + 8, 8))
        head = struct.unpack("<I", rd(base, 4))[0] if size else 0
        print(f"hit: stream base=0x{base:08x} size=0x{size:x} first=0x{head:x}", flush=True)
        if (size == 0x20000 and head == 0x14) or (want_sys and size == 0x4000 and head == 0x5):
            open(out, "wb").write(rd(base, size))
            print(f"wrote {out}", flush=True)
            break
        d.request("cpu.resume", expect="cpu.resume")
    else:
        print("no game save load seen within 10 min")
finally:
    d.request("cpu.breakpoint.remove", address=BP)
    if d.request("cpu.status").get("stepping"):
        d.request("cpu.resume", expect="cpu.resume")
