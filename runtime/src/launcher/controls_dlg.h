/* The launcher's "Change buttons" window: every action of controls.cfg with two keys (keyboard or
 * mouse button) and a controller button, the same bindings as the game's Esc > Controls page.
 * Included by launcher.c (uses its cfg_* helpers, fonts, S() and theme_draw_button).
 *
 * Keys are stored by SDL's scancode names (physical key positions, as the game reads them): the
 * launcher turns the Windows scan code of the pressed key into that name. "," is stored as
 * "Comma" so it can't split the "key, key | button" list. */

static const struct { const char *id, *label, *def; } k_ctl[] = {
    { "cross", "Cross", "X, Space | a" }, { "circle", "Circle", "Z,  | b" },
    { "square", "Square", "C,  | x" }, { "triangle", "Triangle", "V,  | y" },
    { "l", "L (aim)", "Q, Mouse Right | lefttrigger" }, { "r", "R (fire)", "E, Mouse Left | righttrigger" },
    { "start", "Start", "Return,  | start" }, { "select", "Select", "Tab,  | back" },
    { "dpad_up", "D-pad up", "Up,  | dpup" }, { "dpad_down", "D-pad down", "Down,  | dpdown" },
    { "dpad_left", "D-pad left", "Left,  | dpleft" }, { "dpad_right", "D-pad right", "Right,  | dpright" },
    { "move_up", "Move forward", "W,  | " }, { "move_down", "Move back", "S,  | " },
    { "move_left", "Move left", "A,  | " }, { "move_right", "Move right", "D,  | " },
    { "camera_up", "Camera up", "I,  | " }, { "camera_down", "Camera down", "K,  | " },
    { "camera_left", "Camera left", "J,  | " }, { "camera_right", "Camera right", "L,  | " },
    { "camera_reset", "Camera behind Aya", "O,  | rightstick" },
};
enum { NCTL = sizeof(k_ctl) / sizeof(k_ctl[0]) };
/* controller buttons: SDL's name in controls.cfg, the name shown (Xbox layout, as in the game) */
static const char *const k_padb[][2] = {
    { "", "(none)" }, { "a", "A" }, { "b", "B" }, { "x", "X" }, { "y", "Y" },
    { "leftshoulder", "LB" }, { "rightshoulder", "RB" }, { "lefttrigger", "LT" }, { "righttrigger", "RT" },
    { "back", "View" }, { "start", "Menu" }, { "leftstick", "LS click" }, { "rightstick", "RS click" },
    { "dpup", "D-pad up" }, { "dpdown", "D-pad down" }, { "dpleft", "D-pad left" }, { "dpright", "D-pad right" },
    { "misc1", "Share / Capture" }, { "touchpad", "Touchpad" },
    { "paddle1", "Paddle 1" }, { "paddle2", "Paddle 2" }, { "paddle3", "Paddle 3" }, { "paddle4", "Paddle 4" },
};
enum { NPADB = sizeof(k_padb) / sizeof(k_padb[0]) };

/* Windows scan code (set 1) -> SDL scancode name; [1] = with the extended-key flag */
static const char *scan_name(int sc, int ext) {
    static const char *const base[0x59] = {
        [0x01] = "Escape", [0x02] = "1", [0x03] = "2", [0x04] = "3", [0x05] = "4", [0x06] = "5", [0x07] = "6",
        [0x08] = "7", [0x09] = "8", [0x0A] = "9", [0x0B] = "0", [0x0C] = "-", [0x0D] = "=", [0x0E] = "Backspace",
        [0x0F] = "Tab", [0x10] = "Q", [0x11] = "W", [0x12] = "E", [0x13] = "R", [0x14] = "T", [0x15] = "Y",
        [0x16] = "U", [0x17] = "I", [0x18] = "O", [0x19] = "P", [0x1A] = "[", [0x1B] = "]", [0x1C] = "Return",
        [0x1D] = "Left Ctrl", [0x1E] = "A", [0x1F] = "S", [0x20] = "D", [0x21] = "F", [0x22] = "G", [0x23] = "H",
        [0x24] = "J", [0x25] = "K", [0x26] = "L", [0x27] = ";", [0x28] = "'", [0x29] = "`", [0x2A] = "Left Shift",
        [0x2B] = "\\", [0x2C] = "Z", [0x2D] = "X", [0x2E] = "C", [0x2F] = "V", [0x30] = "B", [0x31] = "N",
        [0x32] = "M", [0x33] = "Comma", [0x34] = ".", [0x35] = "/", [0x36] = "Right Shift", [0x37] = "Keypad *",
        [0x38] = "Left Alt", [0x39] = "Space", [0x3A] = "CapsLock", [0x3B] = "F1", [0x3C] = "F2", [0x3D] = "F3",
        [0x3E] = "F4", [0x3F] = "F5", [0x40] = "F6", [0x41] = "F7", [0x42] = "F8", [0x43] = "F9", [0x44] = "F10",
        [0x45] = "Numlock", [0x46] = "ScrollLock", [0x47] = "Keypad 7", [0x48] = "Keypad 8", [0x49] = "Keypad 9",
        [0x4A] = "Keypad -", [0x4B] = "Keypad 4", [0x4C] = "Keypad 5", [0x4D] = "Keypad 6", [0x4E] = "Keypad +",
        [0x4F] = "Keypad 1", [0x50] = "Keypad 2", [0x51] = "Keypad 3", [0x52] = "Keypad 0", [0x53] = "Keypad .",
        [0x56] = "NonUSBackslash", [0x57] = "F11", [0x58] = "F12",
    };
    if (ext) switch (sc) {
        case 0x1C: return "Keypad Enter"; case 0x1D: return "Right Ctrl"; case 0x35: return "Keypad /";
        case 0x37: return "PrintScreen"; case 0x38: return "Right Alt"; case 0x47: return "Home";
        case 0x48: return "Up"; case 0x49: return "PageUp"; case 0x4B: return "Left"; case 0x4D: return "Right";
        case 0x4F: return "End"; case 0x50: return "Down"; case 0x51: return "PageDown"; case 0x52: return "Insert";
        case 0x53: return "Delete"; case 0x5B: return "Left GUI"; case 0x5C: return "Right GUI";
        case 0x5D: return "Application"; default: return NULL;
    }
    return sc > 0 && sc < 0x59 ? base[sc] : NULL;
}

static HWND s_cd, s_cd_list, s_cd_pad, s_cd_status;
static char s_cd_key[NCTL][2][32], s_cd_padn[NCTL][24];
static int  s_cd_cap = -1;                      /* key slot waiting for a key / mouse button */
enum { CD_LIST = 300, CD_SET1, CD_SET2, CD_CLEAR, CD_PAD, CD_RESET, CD_SAVE, CD_CANCEL };

static void trim_in_place(char *t) {
    char *a = t; while (*a == ' ' || *a == '\t') a++;
    memmove(t, a, strlen(a) + 1);
    size_t n = strlen(t); while (n && (t[n - 1] == ' ' || t[n - 1] == '\t')) t[--n] = 0;
}
static void cd_parse(int a, const char *v) {   /* "k0, k1 | pad" */
    char t[128]; snprintf(t, sizeof(t), "%s", v);
    char *bar = strchr(t, '|'), *pad = (char *)"";
    if (bar) { *bar = 0; pad = bar + 1; }
    char *c = strchr(t, ','), *k1 = (char *)"";
    if (c) { *c = 0; k1 = c + 1; }
    trim_in_place(t); trim_in_place(k1); trim_in_place(pad);
    snprintf(s_cd_key[a][0], 32, "%s", t); snprintf(s_cd_key[a][1], 32, "%s", k1);
    snprintf(s_cd_padn[a], 24, "%s", pad);
}
static void cd_load(int defaults) {
    const char *C = "controls.cfg"; char v[160];
    const int old = !defaults && cfg_int(C, "binds_version", 1) < 2;
    for (int a = 0; a < NCTL; a++) {
        cd_parse(a, !defaults && cfg_read(C, k_ctl[a].id, v, sizeof(v)) ? v : k_ctl[a].def);
        /* the game gives an older file the mouse fire / aim buttons: show what it will use */
        if (old && !strcmp(k_ctl[a].id, "r") && !s_cd_key[a][1][0]) snprintf(s_cd_key[a][1], 32, "Mouse Left");
        if (old && !strcmp(k_ctl[a].id, "l") && !s_cd_key[a][1][0]) snprintf(s_cd_key[a][1], 32, "Mouse Right");
    }
}
static const char *pad_shown(const char *n) {
    for (int i = 0; i < NPADB; i++) if (!_stricmp(n, k_padb[i][0])) return k_padb[i][1];
    return n;
}
static void cd_fill(void) {
    const int sel = (int)SendMessageA(s_cd_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    SendMessageA(s_cd_list, LVM_DELETEALLITEMS, 0, 0);
    for (int a = 0; a < NCTL; a++) {
        LVITEMA it = {0}; it.mask = LVIF_TEXT; it.iItem = a; it.pszText = (char *)k_ctl[a].label;
        SendMessageA(s_cd_list, LVM_INSERTITEMA, 0, (LPARAM)&it);
        const char *col[3] = { s_cd_key[a][0][0] ? s_cd_key[a][0] : "-", s_cd_key[a][1][0] ? s_cd_key[a][1] : "-",
                               s_cd_padn[a][0] ? pad_shown(s_cd_padn[a]) : "-" };
        for (int j = 0; j < 3; j++) {
            it.iSubItem = j + 1; it.pszText = (char *)col[j];
            SendMessageA(s_cd_list, LVM_SETITEMTEXTA, (WPARAM)a, (LPARAM)&it);
        }
    }
    if (sel >= 0) {
        LVITEMA it = {0}; it.stateMask = it.state = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageA(s_cd_list, LVM_SETITEMSTATE, (WPARAM)sel, (LPARAM)&it);
        SendMessageA(s_cd_list, LVM_ENSUREVISIBLE, (WPARAM)sel, FALSE);
    }
}
static int cd_sel(void) { return (int)SendMessageA(s_cd_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED); }
static void cd_show_pad(void) {
    const int a = cd_sel(); int idx = 0;
    if (a >= 0) for (int i = 0; i < NPADB; i++) if (!_stricmp(s_cd_padn[a], k_padb[i][0])) idx = i;
    SendMessageA(s_cd_pad, CB_SETCURSEL, (WPARAM)idx, 0);
    EnableWindow(s_cd_pad, a >= 0);
}
static void cd_status(const char *t) { SetWindowTextA(s_cd_status, t); }
static void cd_assign_key(const char *name) {   /* a key / mouse button leaves any other action */
    const int a = cd_sel();
    if (a < 0 || s_cd_cap < 0) return;
    for (int i = 0; i < NCTL; i++) for (int k = 0; k < 2; k++)
        if (!_stricmp(s_cd_key[i][k], name)) s_cd_key[i][k][0] = 0;
    snprintf(s_cd_key[a][s_cd_cap], 32, "%s", name);
    s_cd_cap = -1;
    char t[128]; snprintf(t, sizeof(t), "%s: %s", k_ctl[a].label, name); cd_status(t);
    cd_fill();
}
/* called from the launcher's message loop: while waiting, keys and mouse buttons become the binding */
static int cd_capture_msg(const MSG *m) {
    if (s_cd_cap < 0 || !s_cd) return 0;
    const char *name = NULL;
    switch (m->message) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        const int sc = (int)((m->lParam >> 16) & 0xFF), ext = (int)((m->lParam >> 24) & 1);
        if (sc == 0x01 && !ext) { s_cd_cap = -1; cd_status("Cancelled."); return 1; }
        name = scan_name(sc, ext);
        if (!name) { cd_status("That key can't be used - press another one (Esc cancels)."); return 1; }
        break;
    }
    case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR: return 1;
    case WM_LBUTTONDOWN: name = "Mouse Left"; break;
    case WM_RBUTTONDOWN: name = "Mouse Right"; break;
    case WM_MBUTTONDOWN: name = "Mouse Middle"; break;
    case WM_XBUTTONDOWN: name = HIWORD(m->wParam) == XBUTTON1 ? "Mouse 4" : "Mouse 5"; break;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: case WM_XBUTTONDBLCLK: return 1;
    default: return 0;
    }
    cd_assign_key(name);
    return 1;
}
static void cd_save(void) {
    const char *C = "controls.cfg";
    for (int a = 0; a < NCTL; a++) {
        char v[128]; snprintf(v, sizeof(v), "%s, %s | %s", s_cd_key[a][0], s_cd_key[a][1], s_cd_padn[a]);
        cfg_write(C, k_ctl[a].id, v, 1);
    }
    cfg_write(C, "binds_version", "2", 1);
}
static void cd_close(void) {
    s_cd_cap = -1;
    EnableWindow(s_win, TRUE);
    DestroyWindow(s_cd); s_cd = NULL;
    SetForegroundWindow(s_win);
}

static HBRUSH s_cd_bgbrush(void) { static HBRUSH b; if (!b) b = CreateSolidBrush(RGB(20, 6, 8)); return b; }

static LRESULT CALLBACK cd_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case CD_SET1: case CD_SET2:
            if (cd_sel() < 0) { cd_status("Select an action in the list first."); return 0; }
            s_cd_cap = LOWORD(wp) == CD_SET1 ? 0 : 1;
            cd_status("Press a key or click a mouse button for it...   (Esc cancels)");
            return 0;
        case CD_CLEAR: {
            const int a = cd_sel(); if (a < 0) return 0;
            s_cd_key[a][0][0] = s_cd_key[a][1][0] = 0; s_cd_padn[a][0] = 0; cd_fill(); cd_show_pad();
            return 0;
        }
        case CD_PAD:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                const int a = cd_sel(), i = (int)SendMessageA(s_cd_pad, CB_GETCURSEL, 0, 0);
                if (a < 0 || i < 0) return 0;
                if (i > 0) for (int o = 0; o < NCTL; o++) if (!_stricmp(s_cd_padn[o], k_padb[i][0])) s_cd_padn[o][0] = 0;
                snprintf(s_cd_padn[a], 24, "%s", k_padb[i][0]); cd_fill();
            }
            return 0;
        case CD_RESET: cd_load(1); cd_fill(); cd_show_pad(); cd_status("Defaults restored - Save to keep them."); return 0;
        case CD_SAVE: cd_save(); cd_close(); return 0;
        case CD_CANCEL: cd_close(); return 0;
        }
        break;
    case WM_NOTIFY: {
        const NMHDR *h = (const NMHDR *)lp;
        if (h->idFrom == CD_LIST && h->code == LVN_ITEMCHANGED) { s_cd_cap = -1; cd_show_pad(); }
        if (h->idFrom == CD_LIST && h->code == NM_DBLCLK && cd_sel() >= 0) {
            s_cd_cap = 0; cd_status("Press a key or click a mouse button for it...   (Esc cancels)");
        }
        break;
    }
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wp, TH_TEXT); SetBkColor((HDC)wp, RGB(20, 6, 8));
        return (LRESULT)s_cd_bgbrush();
    case WM_CTLCOLORLISTBOX: case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, TH_TEXT); SetBkColor((HDC)wp, TH_FIELD);
        return (LRESULT)(s_fieldbrush ? s_fieldbrush : GetStockObject(BLACK_BRUSH));
    case WM_ERASEBKGND: {
        RECT r; GetClientRect(w, &r); FillRect((HDC)wp, &r, s_cd_bgbrush()); return 1;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *d = (const DRAWITEMSTRUCT *)lp;
        if (d->CtlType == ODT_BUTTON) { theme_draw_button(d, d->CtlID == CD_SAVE ? s_bold : s_font, d->CtlID == CD_SAVE); return TRUE; }
        break;
    }
    case WM_CLOSE: cd_close(); return 0;
    }
    return DefWindowProcA(w, m, wp, lp);
}

static HWND cd_mk(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), s_cd,
                             (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    return c;
}
static void controls_dialog(void) {
    if (s_cd) { SetForegroundWindow(s_cd); return; }
    static int reg = 0;
    if (!reg) {
        INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_LISTVIEW_CLASSES }; InitCommonControlsEx(&ic);
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = cd_proc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "ThirdBirthdayControls";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassA(&wc); reg = 1;
    }
    const int CW = 660, CH = 600;
    RECT r = { 0, 0, S(CW), S(CH) }, pr;
    AdjustWindowRect(&r, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE);
    GetWindowRect(s_win, &pr);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    s_cd = CreateWindowExA(WS_EX_DLGMODALFRAME, "ThirdBirthdayControls", "Change buttons", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                           pr.left + (pr.right - pr.left - ww) / 2, pr.top + (pr.bottom - pr.top - wh) / 2, ww, wh,
                           s_win, NULL, GetModuleHandleA(NULL), NULL);
    s_cd_list = cd_mk(WC_LISTVIEWA, "", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP,
                      14, 14, CW - 28, 400, CD_LIST);
    SendMessageA(s_cd_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SendMessageA(s_cd_list, LVM_SETBKCOLOR, 0, TH_FIELD); SendMessageA(s_cd_list, LVM_SETTEXTBKCOLOR, 0, TH_FIELD);
    SendMessageA(s_cd_list, LVM_SETTEXTCOLOR, 0, TH_TEXT);
    static const char *const head[4] = { "Action", "Key", "Key", "Controller" };
    static const int cw[4] = { 190, 140, 140, 130 };
    for (int i = 0; i < 4; i++) {
        LVCOLUMNA c = {0}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = (char *)head[i]; c.cx = S(cw[i]);
        SendMessageA(s_cd_list, LVM_INSERTCOLUMNA, (WPARAM)i, (LPARAM)&c);
    }
    cd_mk("BUTTON", "Set key 1", BS_OWNERDRAW | WS_TABSTOP, 14, 426, 110, 30, CD_SET1);
    cd_mk("BUTTON", "Set key 2", BS_OWNERDRAW | WS_TABSTOP, 132, 426, 110, 30, CD_SET2);
    cd_mk("BUTTON", "Clear", BS_OWNERDRAW | WS_TABSTOP, 250, 426, 80, 30, CD_CLEAR);
    cd_mk("STATIC", "Controller:", 0, 356, 432, 80, 20, 0);
    s_cd_pad = cd_mk("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 440, 428, CW - 454, 300, CD_PAD);
    for (int i = 0; i < NPADB; i++) SendMessageA(s_cd_pad, CB_ADDSTRING, 0, (LPARAM)k_padb[i][1]);
    s_cd_status = cd_mk("STATIC", "Select an action, then Set key and press a key or click a mouse button "
                                  "(or double-click the action).", 0, 14, 470, CW - 28, 40, 0);
    cd_mk("STATIC", "Mouse buttons work while the mouse is captured in the game (click the game window). "
                    "The sticks stay: left = move, right = camera.", 0, 14, 510, CW - 28, 36, 0);
    cd_mk("BUTTON", "Reset to defaults", BS_OWNERDRAW | WS_TABSTOP, 14, 556, 150, 32, CD_RESET);
    cd_mk("BUTTON", "Save", BS_OWNERDRAW | WS_TABSTOP, CW - 234, 556, 110, 32, CD_SAVE);
    cd_mk("BUTTON", "Cancel", BS_OWNERDRAW | WS_TABSTOP, CW - 114, 556, 100, 32, CD_CANCEL);
    cd_load(0); cd_fill(); cd_show_pad();
    EnableWindow(s_win, FALSE);
    ShowWindow(s_cd, getenv("SR_OFFSCREEN") ? SW_SHOWNOACTIVATE : SW_SHOW);
}
