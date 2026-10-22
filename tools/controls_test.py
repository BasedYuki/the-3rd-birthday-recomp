"""Controls test: real Windows key messages to the port's window and a virtual SDL gamepad
(SR_VPAD), each checked against what the port logs it passed to the game.

    python tools/controls_test.py [--save user]
Writes scratchpad/controls/report.txt and in-game screenshots.
"""

import ctypes
import os
import re
import sys
import time
from ctypes import wintypes

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import drive

OUT = os.path.join(drive.SCRATCH, "controls")
user32 = ctypes.WinDLL("user32", use_last_error=True)
WM_KEYDOWN, WM_KEYUP = 0x100, 0x101
# key name -> (virtual key, scancode, extended)
KEYS = {"enter": (0x0D, 0x1C, 0), "tab": (0x09, 0x0F, 0), "x": (0x58, 0x2D, 0), "space": (0x20, 0x39, 0),
        "z": (0x5A, 0x2C, 0), "c": (0x43, 0x2E, 0), "v": (0x56, 0x2F, 0), "q": (0x51, 0x10, 0),
        "e": (0x45, 0x12, 0), "up": (0x26, 0x48, 1), "down": (0x28, 0x50, 1), "left": (0x25, 0x4B, 1),
        "right": (0x27, 0x4D, 1), "w": (0x57, 0x11, 0), "a": (0x41, 0x1E, 0), "s": (0x53, 0x1F, 0),
        "d": (0x44, 0x20, 0), "i": (0x49, 0x17, 0), "j": (0x4A, 0x24, 0), "k": (0x4B, 0x25, 0),
        "l": (0x4C, 0x26, 0), "o": (0x4F, 0x18, 0), "esc": (0x1B, 0x01, 0), "f12": (0x7B, 0x58, 0)}
KEY_BITS = {"enter": 0x8, "tab": 0x1, "x": 0x4000, "space": 0x4000, "z": 0x2000, "c": 0x8000, "v": 0x1000,
            "q": 0x100, "e": 0x200, "up": 0x10, "down": 0x40, "left": 0x80, "right": 0x20}
KEY_STICK = {"w": (128, 1), "s": (128, 255), "a": (1, 128), "d": (255, 128)}
# SDL gamepad buttons / axes
PAD_BITS = {0: ("A/south", 0x4000), 1: ("B/east", 0x2000), 2: ("X/west", 0x8000), 3: ("Y/north", 0x1000),
            4: ("Back", 0x1), 6: ("Start", 0x8), 9: ("LB", 0x100), 10: ("RB", 0x200), 11: ("Dpad up", 0x10),
            12: ("Dpad down", 0x40), 13: ("Dpad left", 0x80), 14: ("Dpad right", 0x20)}


def hwnd_of(pid):
    found = []
    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, _):
        p = wintypes.DWORD(); user32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(h): found.append(h)
        return True
    user32.EnumWindows(cb, 0)
    return found[0] if found else None


class Keys:
    def __init__(self, hwnd): self.h = hwnd

    def _msg(self, name, up):
        vk, sc, ext = KEYS[name]
        lp = 1 | (sc << 16) | (ext << 24) | ((0xC0000000) if up else 0)
        user32.PostMessageW(self.h, WM_KEYUP if up else WM_KEYDOWN, vk, ctypes.c_long(lp & 0xFFFFFFFF if not up else lp - (1 << 32)))

    def down(self, n): self._msg(n, False)
    def up(self, n): self._msg(n, True)

    def tap(self, n, hold=0.25):
        self.down(n); time.sleep(hold); self.up(n); time.sleep(0.25)


def log_since(path, pos):
    with open(path, errors="replace") as f:
        f.seek(pos); t = f.read(); return t, f.tell()


def main():
    save = "user"
    if "--save" in sys.argv: save = sys.argv[sys.argv.index("--save") + 1]
    os.makedirs(OUT, exist_ok=True)
    g = drive.Port(OUT, drive.SAVES[save][0], extra_env={"SR_KEYLOG": "1", "SR_VPAD": "1", "SR_TWINSTICK": "1"})
    errp = os.path.join(OUT, "port.err")
    rep = []
    def check(label, ok, detail=""):
        rep.append(f"{'PASS' if ok else 'FAIL'}  {label}  {detail}"); print(rep[-1], flush=True)
    try:
        time.sleep(8)
        h = hwnd_of(g.p.pid)
        user32.SetForegroundWindow(h)
        kb = Keys(h)
        pos = 0
        _, pos = log_since(errp, 0)
        # -- keyboard buttons
        for name, bit in KEY_BITS.items():
            kb.down(name); time.sleep(0.3)
            t, pos = log_since(errp, pos)
            seen = [int(m, 16) for m in re.findall(r"keys: buttons 0x([0-9a-f]+)", t)]
            kb.up(name); time.sleep(0.3)
            t2, pos = log_since(errp, pos)
            check(f"key {name:6s} -> 0x{bit:04x}", bit in seen, f"seen {[hex(v) for v in seen]}")
        # -- keyboard stick
        for name, (ex, ey) in KEY_STICK.items():
            kb.down(name); time.sleep(0.3)
            t, pos = log_since(errp, pos)
            st = re.findall(r"stick (\d+),(\d+)", t)
            kb.up(name); time.sleep(0.3); _, pos = log_since(errp, pos)
            ok = any(abs(int(x) - ex) <= 2 and abs(int(y) - ey) <= 2 for x, y in st)
            check(f"key {name} -> stick {ex},{ey}", ok, f"seen {st[-3:]}")
        # -- virtual gamepad buttons
        for b, (nm, bit) in PAD_BITS.items():
            g._cmd(f"vbtn {b} 1"); time.sleep(0.3)
            t, pos = log_since(errp, pos)
            seen = [int(m, 16) for m in re.findall(r"keys: buttons 0x([0-9a-f]+)", t)]
            g._cmd(f"vbtn {b} 0"); time.sleep(0.3); _, pos = log_since(errp, pos)
            check(f"pad {nm:10s} -> 0x{bit:04x}", bit in seen, f"seen {[hex(v) for v in seen]}")
        for ax, nm, bit in ((4, "LT", 0x100), (5, "RT", 0x200)):
            g._cmd(f"vaxis {ax} 32767"); time.sleep(0.3)
            t, pos = log_since(errp, pos)
            seen = [int(m, 16) for m in re.findall(r"keys: buttons 0x([0-9a-f]+)", t)]
            g._cmd(f"vaxis {ax} 0"); time.sleep(0.3); _, pos = log_since(errp, pos)
            check(f"pad {nm} trigger -> 0x{bit:04x}", bit in seen, f"seen {[hex(v) for v in seen]}")
        for ax, val, exp in ((0, -32768, (0, 128)), (0, 32767, (255, 128)), (1, -32768, (128, 0)), (1, 32767, (128, 255))):
            g._cmd(f"vaxis {ax} {val}"); time.sleep(0.3)
            t, pos = log_since(errp, pos)
            st = re.findall(r"stick (\d+),(\d+)", t)
            g._cmd(f"vaxis {ax} 0"); time.sleep(0.3); _, pos = log_since(errp, pos)
            ok = any(abs(int(x) - exp[0]) <= 2 and abs(int(y) - exp[1]) <= 2 for x, y in st)
            check(f"pad left stick axis{ax}={val} -> {exp}", ok, f"seen {st[-3:]}")
        # -- settings menu: Esc, and Guide on the pad
        kb.tap("esc"); time.sleep(1); g.shot("menu_esc")
        t, pos = log_since(errp, pos)
        check("Esc opens the settings menu", "input: Esc (settings menu)" in t)
        kb.tap("esc"); time.sleep(1)
        g._cmd("vbtn 5 1"); time.sleep(0.2); g._cmd("vbtn 5 0"); time.sleep(1); g.shot("menu_guide")
        t, pos = log_since(errp, pos)
        check("pad Guide opens the settings menu", "pad Guide (settings menu)" in t)
        g._cmd("vbtn 5 1"); time.sleep(0.2); g._cmd("vbtn 5 0"); time.sleep(1)
        # -- F12 screenshot
        kb.tap("f12"); time.sleep(1.5)
        t, pos = log_since(errp, pos)
        check("F12 saves a screenshot", "screenshot: Screenshot saved" in t)
    finally:
        g.close()
    open(os.path.join(OUT, "report.txt"), "w").write("\n".join(rep) + "\n")


if __name__ == "__main__":
    main()
