"""Log calls to given guest functions in PPSSPP (breakpoint, print a0..t1 and ra, resume) while
a drive.py scenario plays. Import stubs work too (e.g. sceIoDevctl 0x08c198d4).

    python tools/oracle_trace_calls.py <scenario> <hexaddr>[,<hexaddr>|w<hexaddr>...] [--save user|clear]
(w<addr>: stop on writes to that word; the line shows the writing pc.)
"""

import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive
import scenarios
from ppsspp_ws import Dbg

scen = sys.argv[1]
addrs = [int(a, 16) for a in sys.argv[2].split(",") if not a.startswith("w")]
waddrs = [int(a[1:], 16) for a in sys.argv[2].split(",") if a.startswith("w")]   # w<hex>: write breakpoints
save = sys.argv[sys.argv.index("--save") + 1] if "--save" in sys.argv else "user"
out = os.path.join(drive.SCRATCH, "drive", "trace_" + scen)
os.makedirs(out, exist_ok=True)
g = drive.Ppsspp(out, drive.SAVES[save][1] or drive.SAVES[save][0])
stop = threading.Event()
log = open(os.path.join(out, "calls.txt"), "w")


def tracer():
    d = Dbg(45679)
    for a in addrs:
        while not stop.is_set():
            try:
                d.request("cpu.breakpoint.add", address=a, enabled=True); break
            except RuntimeError:
                time.sleep(0.2)
    for a in waddrs:
        while not stop.is_set():
            try:
                d.request("memory.breakpoint.add", address=a, size=4, write=True, read=False, change=False,
                          enabled=True, log=False); break
            except RuntimeError:
                time.sleep(0.2)
    while not stop.is_set():
        try:
            st = d.request("cpu.status")
            if st.get("stepping") and (st.get("pc") in addrs or waddrs):
                c = d.request("cpu.getAllRegs")["categories"][0]
                r = dict(zip(c["registerNames"], c["uintValues"]))
                line = (f"{time.time():.2f} pc=0x{st['pc']:08x} ra=0x{r['ra']:08x} a0={r['a0']:08x} a1={r['a1']:08x} "
                        f"a2={r['a2']:08x} a3={r['a3']:08x} t0={r['t0']:08x} t1={r['t1']:08x}")
                if os.environ.get("DUMP_A0"):   # DUMP_A0=<hex len>: also save the memory at a0
                    import base64
                    raw = base64.b64decode(d.request("memory.read", address=r["a0"],
                                                     size=int(os.environ["DUMP_A0"], 16))["base64"])
                    open(os.path.join(out, f"a0_{r['a0']:08x}_{int(time.time() * 1000)}.bin"), "wb").write(raw)
                print(line, flush=True); log.write(line + "\n"); log.flush()
                d.request("cpu.resume", expect="cpu.resume")
            time.sleep(0.01)
        except Exception:
            time.sleep(0.05)


t = threading.Thread(target=tracer, daemon=True)
t.start()
try:
    getattr(scenarios, scen)(g)
finally:
    stop.set()
    g.close()
