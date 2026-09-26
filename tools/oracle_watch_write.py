"""Break in PPSSPP when a guest address is written; print pc, registers and a backtrace.

    python tools/oracle_watch_write.py <hexaddr> [hits]
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppsspp_ws import Dbg

addr = int(sys.argv[1], 16)
hits = int(sys.argv[2]) if len(sys.argv) > 2 else 1
d = Dbg(45679)
d.request("memory.breakpoint.add", address=addr, size=4, write=True, read=False, change=False,
          enabled=True, log=False)
try:
    for h in range(hits):
        end = time.time() + 30
        got = None
        while time.time() < end and not got:
            for m in d.drain(1.0):
                if m.get("event") == "cpu.stepping":
                    got = m
        if not got:
            print("no hit within 30 s")
            break
        print(f"--- hit {h}: {got}")
        regs = d.request("cpu.getAllRegs")
        gpr = regs["categories"][0]
        print("  " + " ".join(f"{n}={v}" for n, v in zip(gpr["registerNames"], gpr["uintValues"])))
        bt = d.request("hle.backtrace")
        for f in bt.get("frames", []):
            print(f"  frame entry=0x{f.get('entry', 0):08x} pc=0x{f.get('pc', 0):08x} sp=0x{f.get('sp', 0):08x}")
        d.request("cpu.resume", expect="cpu.resume")
finally:
    d.request("memory.breakpoint.remove", address=addr, size=4)
    st = d.request("cpu.status")
    if st.get("stepping"):
        d.request("cpu.resume", expect="cpu.resume")
