"""Run a drive.py scenario on PPSSPP with debug logging for some channels and collect its log
lines from the WebSocket debugger (the ini is restored afterwards).

    python tools/oracle_log.py <scenario> SCEIO,SCEUTIL,SCEKERNEL [--save user|clear]
"""

import os
import re
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive
import scenarios
from ppsspp_ws import Dbg

scen = sys.argv[1]
chans = sys.argv[2].split(",")
save = sys.argv[sys.argv.index("--save") + 1] if "--save" in sys.argv else "user"
ini = os.path.join(drive.ROOT, "memstick", "PSP", "SYSTEM", "ppsspp.ini")
orig = open(ini, encoding="utf-8", newline="").read()
t = orig
for c in chans:
    t = re.sub(rf"(?m)^{c}Level = \d+", f"{c}Level = 5", t)
open(ini, "w", encoding="utf-8", newline="").write(t)
out = os.path.join(drive.SCRATCH, "drive", "log_" + scen)
os.makedirs(out, exist_ok=True)
try:
    g = drive.Ppsspp(out, drive.SAVES[save][1] or drive.SAVES[save][0])
    g.ini_text = orig                       # Ppsspp.close restores this text
    stop = threading.Event()
    logf = open(os.path.join(out, "ppsspp_log.txt"), "w", encoding="utf-8")

    def collect():
        d = Dbg(45679)
        while not stop.is_set():
            for m in d.drain(0.5):
                if m.get("event") == "log":
                    logf.write(f"{m.get('channel', '')} {m.get('level', '')} {m.get('message', '').rstrip()}\n")
            logf.flush()

    th = threading.Thread(target=collect, daemon=True)
    th.start()
    try:
        getattr(scenarios, scen)(g)
    finally:
        stop.set(); time.sleep(0.6)
        g.close()
finally:
    open(ini, "w", encoding="utf-8", newline="").write(orig)
print("log:", os.path.join(out, "ppsspp_log.txt"))
