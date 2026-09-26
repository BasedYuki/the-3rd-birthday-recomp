"""Drive PPSSPP (the oracle) from boot to just after New Game and report thread states.

Run with PPSSPP already booted into the game and its WebSocket debugger on PORT.
"""

import sys
import time

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from ppsspp_ws import Dbg

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 45679
d = Dbg(PORT)


def threads(tag):
    r = d.request("hle.thread.list")
    print(f"--- {tag}")
    for t in r["threads"]:
        print(f"  {t['id']:>4} {t['name'][:32]:32} status={t.get('status')} pc=0x{t.get('pc', 0):08x} "
              f"wait={t.get('waitType', '')}:{t.get('waitID', '')}")


def step(tag, button, wait):
    if button:
        d.press(button, 6)
    time.sleep(wait)
    threads(tag)


step("boot notice", None, 1)
step("after X on notice", "cross", 6)
step("after START (skip logo)", "start", 6)
step("title?", None, 4)
step("after UP", "up", 1)
step("after X (New Game)", "cross", 3)
step("after X (difficulty)", "cross", 6)
step("+10s", None, 10)
