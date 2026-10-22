"""Load the game save in PPSSPP with u32 patches applied to the decrypted buffer as the game
reads it (same break as oracle_save_dump.py), then let it run hands-off and report whether
PPSSPP stopped (an invalid jump etc. puts its CPU into stepping).

    python tools/oracle_save_patch.py <hexoff>=<val>[,...] [seconds]
"""

import base64
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppsspp_ws import Dbg

patches = [(int(k, 16), int(v, 0)) for k, v in (kv.split("=") for kv in sys.argv[1].split(","))]
secs = int(sys.argv[2]) if len(sys.argv) > 2 else 60
d = Dbg(45679)
BP = 0x08B1072C


def rd(addr, n):
    return base64.b64decode(d.request("memory.read", address=addr, size=n, timeout=20)["base64"])


d.request("cpu.breakpoint.add", address=BP, enabled=True)
i = 0
patched = False
end = time.time() + 300
while time.time() < end and not patched:
    if not d.request("cpu.status").get("stepping"):
        d.ticket += 1
        d._send(json.dumps(dict(event="input.buttons.press", ticket=str(d.ticket),
                                button="cross" if i % 3 else "start", duration=6)))
        i += 1
        time.sleep(0.7)
        continue
    regs = d.request("cpu.getAllRegs")["categories"][0]
    s1 = dict(zip(regs["registerNames"], regs["uintValues"]))["s1"]
    base, size = struct.unpack("<II", rd(s1 + 8, 8))
    if size == 0x20000 and struct.unpack("<I", rd(base, 4))[0] == 0x14:
        for off, val in patches:
            d.request("memory.write", address=base + off, base64=base64.b64encode(struct.pack("<I", val)).decode())
        print("patched", [(hex(o), hex(v)) for o, v in patches], "at buffer", hex(base), flush=True)
        patched = True
        d.request("cpu.breakpoint.remove", address=BP)
    d.request("cpu.resume", expect="cpu.resume")
if not patched:
    print("save load not seen")
    sys.exit(1)
t0 = time.time()
while time.time() - t0 < secs:
    st = d.request("cpu.status")
    if st.get("stepping"):
        regs = d.request("cpu.getAllRegs")["categories"][0]
        r = dict(zip(regs["registerNames"], regs["uintValues"]))
        print(f"PPSSPP STOPPED after {time.time() - t0:.0f} s: pc=0x{st.get('pc', 0):08x} ra=0x{r['ra']:08x}")
        sys.exit(2)
    time.sleep(2)
print(f"PPSSPP still running after {secs} s")
