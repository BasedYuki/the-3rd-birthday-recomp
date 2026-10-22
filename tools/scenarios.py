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
