"""Scenarios for tools/drive.py: the same button sequence on the port and on PPSSPP."""


def boot_idle(g):
    """Boot with no input; a shot every 4 s (boot logos, notices, attract)."""
    for i in range(16):
        g.wait(4)
        g.shot(f"t{(i + 1) * 4:03d}")


def title_flow(g):
    """Dismiss the autosave notice, then watch what follows without input."""
    g.wait(7); g.shot("a_notice")
    g.press("cross"); g.wait(1)
    for i in range(12):
        g.wait(3)
        g.shot(f"b{(i + 1) * 3:02d}")


def boot_to_title(g):
    g.wait(7); g.press("cross")          # autosave notice
    g.wait(4); g.press("start")          # skip the opening movie
    g.wait(4)


def ngplus_menus(g):
    """Title -> Load Game -> slot -> clear-data carry-over -> difficulty -> Episode 00 prompt."""
    boot_to_title(g); g.shot("s00_title")
    for i in range(1, 9):
        g.press("cross"); g.wait(3); g.shot(f"s{i:02d}_x")


def to_title_menu(g):
    """Boot to the title menu (New Game / Load Game / Extra), Load Game highlighted."""
    boot_to_title(g)
    g.press("cross"); g.wait(2)          # "press start" logo
    g.press("cross"); g.wait(2)          # menu


def load_dialog(g):
    """Load Game: the save list; move down, back out, open again, pick slot 2."""
    to_title_menu(g); g.shot("a_menu")
    g.press("cross"); g.wait(2); g.shot("b_list")
    g.press("down"); g.wait(1); g.shot("c_down")
    g.press("circle"); g.wait(2); g.shot("d_back")
    g.press("cross"); g.wait(2); g.shot("e_list_again")
    g.press("down"); g.wait(1); g.press("cross"); g.wait(4); g.shot("f_loaded_slot2")
    for i in range(4):
        g.press("cross"); g.wait(3); g.shot(f"g{i}")


def load_cancel(g):
    """Load Game -> the save list -> Circle (cancel): what does the game do?"""
    to_title_menu(g); g.shot("a_menu")
    g.press("cross"); g.wait(4); g.shot("b_list")
    g.press("circle", 10); g.wait(1); g.shot("c_cancel1")
    g.wait(2); g.shot("d_cancel3")
    g.wait(4); g.shot("e_cancel7")
    g.press("cross"); g.wait(3); g.shot("f_x")


def new_game(g):
    """Title -> New Game: difficulty, save-slot choice (system Save screen), opening."""
    to_title_menu(g)
    g.press("up"); g.wait(1); g.shot("a_newgame_sel")
    for i in range(8):
        g.press("cross"); g.wait(3); g.shot(f"b{i}")


def load_scene(g):
    """Title -> Load Game -> pick the save (PPSSPP: system list + "Load completed"), then watch
    the scene the save (patched with --scene N) starts in, no further input."""
    to_title_menu(g)
    g.press("cross"); g.wait(4)          # Load Game -> save list
    g.press("cross"); g.wait(3)          # pick slot 1
    if g.name == "ppsspp":
        g.press("cross"); g.wait(2)      # "Load completed"
    for t in (5, 10, 15, 20, 25, 30, 40, 50):
        g.wait(5 if t <= 30 else 10)
        g.shot(f"t{t:02d}")


def scene_quick(g):
    """load_scene with fewer shots (batch comparisons of many scenes)."""
    to_title_menu(g)
    g.press("cross"); g.wait(4)
    g.press("cross"); g.wait(3)
    if g.name == "ppsspp":
        g.press("cross"); g.wait(2)
    for t in (6, 12, 20, 30):
        g.wait(6 if t <= 12 else 8 if t == 20 else 10)
        g.shot(f"t{t:02d}")


def scene_long(g):
    """A scene for 90 s after the load (fades, slow camera intros)."""
    to_title_menu(g)
    g.press("cross"); g.wait(4)
    g.press("cross"); g.wait(3)
    if g.name == "ppsspp":
        g.press("cross"); g.wait(2)
    for t in (10, 30, 50, 70, 90):
        g.wait(10 if t == 10 else 20)
        g.shot(f"t{t:02d}")


def data_install(g):
    """Title -> Extra -> Data Install -> Begin installation."""
    to_title_menu(g)
    g.press("down"); g.wait(1)
    g.press("cross"); g.wait(2.5)        # Extra
    g.press("cross"); g.wait(3); g.shot("a_prompt")
    g.press("cross"); g.wait(4); g.shot("b_begin")
    for t in (10, 30, 60):
        g.wait(10 if t == 10 else 20 if t == 30 else 30); g.shot(f"c{t:02d}")


def install_prompt_dump(g):
    """Stop at the Data Install prompt and dump the install objects (diff port vs PPSSPP)."""
    import os
    to_title_menu(g)
    g.press("down"); g.wait(1)
    g.press("cross"); g.wait(2.5)
    g.press("cross"); g.wait(3); g.shot("a_prompt")
    for base, n in ((0x08c50000, 0x1000), (0x08caf000, 0x1000), (0x08c2d000, 0x1000)):
        open(os.path.join(g.out, f"mem_{base:08x}.bin"), "wb").write(b"".join(g.read(base + o, 0x800) for o in range(0, n, 0x800)))


def install_begin_dump(g):
    import os
    to_title_menu(g)
    g.press("down"); g.wait(1)
    g.press("cross"); g.wait(2.5)
    g.press("cross"); g.wait(3)
    g.press("cross"); g.wait(1.0); g.shot("b_begin")
    for base, n in ((0x08c50000, 0x1000), (0x08caf000, 0x1000), (0x08c2d000, 0x1000)):
        open(os.path.join(g.out, f"mem_{base:08x}.bin"), "wb").write(b"".join(g.read(base + o, 0x800) for o in range(0, n, 0x800)))


def quick_title(g):
    to_title_menu(g); g.shot("menu")


def load_play(g):
    """Load Game -> first slot -> play on for a while (used with --save installed: reads 3RDINS.BIN)."""
    to_title_menu(g)
    g.press("cross"); g.wait(3); g.shot("a_list")
    g.press("cross"); g.wait(6); g.shot("b_loaded")
    for i in range(5):
        g.press("cross"); g.wait(5); g.shot(f"c{i}")


def data_install_size(g):
    """data_install, then report the size of 3RDINS.BIN each emulator wrote."""
    import os
    data_install(g)
    g.wait(30)
    for root in (r"K:\The 3rd Birthday project\memstick", r"K:\The 3rd Birthday project\build\ULUS10567\memstick_drive"):
        p = os.path.join(root, "PSP", "SAVEDATA", "ULUS10567DATA", "3RDINS.BIN")
        if os.path.exists(p): print(g.name, p, os.path.getsize(p), flush=True)
