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
        "l": (0x4C, 0x26, 0), "o": (0x4F, 0x18, 0), "esc": (0x1B, 0x01, 0), "f12": (0x7B, 0x58, 0), "b": (0x42, 0x30, 0)}
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
    if os.path.exists(os.path.join(OUT, "controls.cfg")): os.remove(os.path.join(OUT, "controls.cfg"))   # defaults
    g = drive.Port(OUT, drive.SAVES[save][0], extra_env={"SR_KEYLOG": "1", "SR_VPAD": "1", "SR_TWINSTICK": "1"})
    errp = os.path.join(OUT, "port.err")
    rep = []
    def check(label, ok, detail=""):
        rep.append(f"{'PASS' if ok else 'FAIL'}  {label}  {detail}"); print(rep[-1], flush=True)
    try:
        time.sleep(8)
        g._cmd("vaxis 4 -32768"); g._cmd("vaxis 5 -32768")   # triggers at rest
        h = hwnd_of(g.p.pid)
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
            g._cmd(f"vaxis {ax} -32768"); time.sleep(0.3); _, pos = log_since(errp, pos)   # virtual axis rest = trigger 0
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
        # -- remapping (Esc menu -> Controls), into the test's own controls.cfg
        cfg = os.path.join(OUT, "controls.cfg")   # Square is remapped: Cross would start things at the title
        def held_bits(press, release):
            nonlocal pos
            _, pos = log_since(errp, pos)
            press(); time.sleep(0.3)
            t, pos = log_since(errp, pos)
            release(); time.sleep(0.3); _, pos = log_since(errp, pos)
            return [int(m, 16) for m in re.findall(r"keys: buttons 0x([0-9a-f]+)", t)]
        def open_controls():
            kb.tap("esc"); time.sleep(0.5); kb.tap("down"); kb.tap("enter"); time.sleep(0.5)
        def close_menus():
            kb.tap("esc"); kb.tap("esc"); time.sleep(0.8)
        open_controls(); kb.tap("down"); kb.tap("down"); g.shot("controls_page")
        kb.tap("enter"); kb.tap("b"); time.sleep(0.3); g.shot("controls_rebound"); close_menus()
        check("remap: Square key 1 -> B saved", "square = B, " in open(cfg).read())
        seen = held_bits(lambda: kb.down("b"), lambda: kb.up("b"))
        check("remap: B now gives Square", 0x8000 in seen, f"seen {[hex(v) for v in seen]}")
        seen = held_bits(lambda: kb.down("c"), lambda: kb.up("c"))
        check("remap: C no longer gives Square", 0x8000 not in seen, f"seen {[hex(v) for v in seen]}")
        open_controls(); kb.tap("down"); kb.tap("down"); kb.tap("right"); kb.tap("right"); kb.tap("enter")
        g._cmd("vbtn 3 1"); time.sleep(0.2); g._cmd("vbtn 3 0"); time.sleep(0.3); close_menus()
        txt = open(cfg).read()
        check("remap: pad Y bound to Square, taken from Triangle", "square = B,  | y" in txt and "triangle = V,  | \n" in txt)
        seen = held_bits(lambda: g._cmd("vbtn 3 1"), lambda: g._cmd("vbtn 3 0"))
        check("remap: pad Y now gives Square", 0x8000 in seen and 0x1000 not in seen, f"seen {[hex(v) for v in seen]}")
        open_controls(); kb.tap("up"); kb.tap("up"); kb.tap("enter"); time.sleep(0.3); close_menus()
        seen = held_bits(lambda: kb.down("c"), lambda: kb.up("c"))
        check("remap: Reset to defaults restores C", 0x8000 in seen and "square = C,  | x" in open(cfg).read(),
              f"seen {[hex(v) for v in seen]}")
        # -- mouse buttons: bindable on the Controls page (key columns), act once the mouse is captured.
        #    Clicks are SDL events from the control port (vmouse): Windows messages posted to an
        #    unfocused window don't count as clicks, and real ones would take the user's mouse.
        check("defaults: left mouse = R, right mouse = L",
              "r = E, Mouse Left | righttrigger" in open(cfg).read() and "l = Q, Mouse Right | lefttrigger" in open(cfg).read())
        open_controls(); kb.tap("down"); kb.tap("down"); kb.tap("right"); kb.tap("enter"); time.sleep(0.3)
        g._cmd("vmouse 4 1"); time.sleep(0.1); g._cmd("vmouse 4 0"); time.sleep(0.3)   # SDL button 4 = Mouse 4
        g.shot("controls_mouse"); close_menus()
        check("remap: Square key 2 -> Mouse 4 saved", "square = C, Mouse 4 | x" in open(cfg).read())
        _, pos = log_since(errp, pos)
        g._cmd("vmouse 1 1"); g._cmd("vmouse 1 0")
        time.sleep(0.5)
        t, pos = log_since(errp, pos)
        captured = "mouse captured" in t
        check("first click captures the mouse", captured)
        if captured:
            seen = held_bits(lambda: g._cmd("vmouse 1 1"), lambda: g._cmd("vmouse 1 0"))
            check("captured: left mouse gives R (fire)", 0x200 in seen, f"seen {[hex(v) for v in seen]}")
            seen = held_bits(lambda: g._cmd("vmouse 4 1"), lambda: g._cmd("vmouse 4 0"))
            check("captured: Mouse 4 gives Square", 0x8000 in seen, f"seen {[hex(v) for v in seen]}")
            kb.tap("esc"); time.sleep(0.5); kb.tap("esc"); time.sleep(0.5)   # release the mouse, close the menu
    finally:
        g.close()
    # -- files from before mouse bindings, and the launcher's format ("Comma" for the , key)
    open(cfg, "w").write("cross = X, Space | a\ncircle = Comma,  | b\nr = E,  | righttrigger\nl = Q,  | lefttrigger\n"
                         "mouse_buttons_fire_aim = 1\n")
    g = drive.Port(OUT, drive.SAVES[save][0], extra_env={"SR_KEYLOG": "1", "SR_VPAD": "1", "SR_TWINSTICK": "1"})
    try:
        time.sleep(8)
        txt = open(cfg).read()
        check("old controls.cfg: mouse fire / aim added, binds_version 2",
              "r = E, Mouse Left | righttrigger" in txt and "l = Q, Mouse Right | lefttrigger" in txt and "binds_version = 2" in txt)
        check("Comma key name kept", "circle = Comma,  | b" in txt)
        h = hwnd_of(g.p.pid); kb = Keys(h)
        KEYS["comma"] = (0xBC, 0x33, 0)
        _, pos = log_since(errp, 0)
        kb.down("comma"); time.sleep(0.3)
        t, pos = log_since(errp, pos)
        kb.up("comma"); time.sleep(0.3)
        seen = [int(m, 16) for m in re.findall(r"keys: buttons 0x([0-9a-f]+)", t)]
        check("Comma key gives Circle", 0x2000 in seen, f"seen {[hex(v) for v in seen]}")
    finally:
        g.close()
    open(os.path.join(OUT, "report.txt"), "w").write("\n".join(rep) + "\n")


if __name__ == "__main__":
    main()
