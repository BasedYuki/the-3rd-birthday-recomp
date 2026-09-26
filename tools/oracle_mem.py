"""Read a range of PPSSPP guest memory to a file (via the WebSocket debugger).

    python tools/oracle_mem.py <hexaddr> <hexsize> <out.bin>
"""

import base64
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppsspp_ws import Dbg

addr, size, out = int(sys.argv[1], 16), int(sys.argv[2], 16), sys.argv[3]
d = Dbg(45679)
buf = bytearray()
CH = 0x10000
for a in range(addr, addr + size, CH):
    n = min(CH, addr + size - a)
    r = d.request("memory.read", address=a, size=n, timeout=20)
    buf += base64.b64decode(r["base64"])
open(out, "wb").write(buf)
print(f"read {len(buf):#x} bytes from {addr:#010x} -> {out}")
