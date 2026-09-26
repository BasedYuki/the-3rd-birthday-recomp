"""Press a sequence of PPSSPP buttons, then capture the window.

    python tools/oracle_step.py <out.png> [button[:wait_seconds] ...]
e.g. python tools/oracle_step.py shot.png start:3 up:1 cross:4
Buttons: cross circle square triangle start select up down left right ltrigger rtrigger.
A bare wait is written as wait:<seconds>.
"""

import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppsspp_ws import Dbg

out = sys.argv[1]
d = Dbg(45679)
for step in sys.argv[2:]:
    name, _, secs = step.partition(":")
    if name != "wait":
        d.press(name, 6)
    time.sleep(float(secs or 1))
here = os.path.dirname(os.path.abspath(__file__))
subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                os.path.join(here, "capture_window.ps1"), "-Out", os.path.abspath(out)], check=True)
