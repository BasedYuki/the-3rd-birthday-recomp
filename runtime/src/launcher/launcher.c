/* The 3rd Birthday PC port - launcher.
 *
 * A small Win32 window next to the game executable: finds (or asks for) the disc image and
 * checks it is the US release, edits the settings the game reads at start (graphics.cfg,
 * controls.cfg), lists and backs up saves, switches texture packs and memory patches on and
 * off, tells about a newer release on GitHub, and starts the game. Everything it changes is
 * also reachable from the game's own Esc menu or by editing the files; the launcher is a
 * convenience, the game never needs it.
 */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_IE 0x0600
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <commctrl.h>
#include <math.h>

#ifndef SR_VERSION
#define SR_VERSION "0.0.0"
#endif
#define REPO "BasedYuki/the-3rd-birthday-recomp"
#define REPO_W L"BasedYuki/the-3rd-birthday-recomp"
#define GAME_EXE "ULUS10567.exe"
#define GAME_ID "ULUS-10567"

/* ---- small config files ------------------------------------------------------------------ */

/* Read "key=value" (graphics.cfg) or "key = value" (controls.cfg) lines; '#' lines ignored. */
static int cfg_read(const char *file, const char *key, char *out, int cap) {
    FILE *f = fopen(file, "r");
    if (!f) return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        char k[128]; int n = (int)(eq - line);
        while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) n--;
        if (n <= 0 || n >= (int)sizeof(k)) continue;
        memcpy(k, line, (size_t)n); k[n] = 0;
        if (_stricmp(k, key)) continue;
        char *v = eq + 1; while (*v == ' ' || *v == '\t') v++;
        snprintf(out, (size_t)cap, "%s", v);
        out[strcspn(out, "\r\n")] = 0;
        found = 1;
    }
    fclose(f);
    return found;
}
static int cfg_int(const char *file, const char *key, int def) {
    char v[64]; return cfg_read(file, key, v, sizeof(v)) ? atoi(v) : def;
}
static double cfg_float(const char *file, const char *key, double def) {
    char v[64]; return cfg_read(file, key, v, sizeof(v)) ? atof(v) : def;
}
/* Set one key, keeping every other line as it was. spaced: "key = value" style. */
static void cfg_write(const char *file, const char *key, const char *value, int spaced) {
    static char lines[256][512]; int n = 0, done = 0;
    FILE *f = fopen(file, "r");
    if (f) {
        while (n < 256 && fgets(lines[n], sizeof(lines[n]), f)) n++;
        fclose(f);
    }
    for (int i = 0; i < n; i++) {
        if (lines[i][0] == '#') continue;
        char *eq = strchr(lines[i], '=');
        if (!eq) continue;
        char k[128]; int m = (int)(eq - lines[i]);
        while (m > 0 && (lines[i][m - 1] == ' ' || lines[i][m - 1] == '\t')) m--;
        if (m <= 0 || m >= (int)sizeof(k)) continue;
        memcpy(k, lines[i], (size_t)m); k[m] = 0;
        if (!_stricmp(k, key)) { snprintf(lines[i], sizeof(lines[i]), spaced ? "%s = %s\n" : "%s=%s\n", key, value); done = 1; }
    }
    if (!done && n < 256) snprintf(lines[n++], sizeof(lines[0]), spaced ? "%s = %s\n" : "%s=%s\n", key, value);
    f = fopen(file, "w");
    if (!f) return;
    for (int i = 0; i < n; i++) fputs(lines[i], f);
    fclose(f);
}
static void cfg_set_int(const char *file, const char *key, int v, int spaced) {
    char b[32]; snprintf(b, sizeof(b), "%d", v); cfg_write(file, key, b, spaced);
}

/* ---- disc image ---------------------------------------------------------------------------- */

static char s_iso[MAX_PATH];

/* The US release: "ULUS-10567" appears in UMD_DATA.BIN near the start of the disc. */
static int iso_is_game(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static char buf[1 << 20];
    int ok = 0;
    for (int chunk = 0; chunk < 8 && !ok; chunk++) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        for (size_t i = 0; i + 10 <= n; i++) if (buf[i] == 'U' && !memcmp(buf + i, GAME_ID, 10)) { ok = 1; break; }
        if (n < sizeof(buf)) break;
    }
    fclose(f);
    return ok;
}
static void iso_find(void) {
    s_iso[0] = 0;
    char v[MAX_PATH];
    if (cfg_read("launch.cfg", "iso", v, sizeof(v)) && GetFileAttributesA(v) != INVALID_FILE_ATTRIBUTES) {
        snprintf(s_iso, sizeof(s_iso), "%s", v); return;
    }
    char up[MAX_PATH] = ".", pat[MAX_PATH + 16];
    for (int depth = 0; depth < 4; depth++) {   /* this folder and up to three above, like the game */
        WIN32_FIND_DATAA fd;
        snprintf(pat, sizeof(pat), "%s/*.iso", up);
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            char full[MAX_PATH * 2]; snprintf(full, sizeof(full), "%s/%s", up, fd.cFileName);
            GetFullPathNameA(full, sizeof(s_iso), s_iso, NULL);
            FindClose(h); return;
        }
        snprintf(pat, sizeof(pat), "%s/..", up); snprintf(up, sizeof(up), "%s", pat);
    }
}

/* ---- update check ---------------------------------------------------------------------------- */

static char s_latest[32];
static long long ver_num(const char *v) {
    while (*v && (*v < '0' || *v > '9')) v++;
    long long n = 0;
    for (int part = 0; part < 3; part++) {
        long p = 0;
        if (*v >= '0' && *v <= '9') { char *e; p = strtol(v, &e, 10); v = e; }
        n = n * 100000 + p;
        if (*v == '.') v++;
    }
    return n;
}
static HWND s_win, s_status;
#define WM_APP_UPDATE (WM_APP + 1)
#define WM_APP_PREP   (WM_APP + 2)     /* wparam: percent (or -1 failed, 101 done) */
static DWORD WINAPI update_thread(void *arg) {
    (void)arg;
    HINTERNET ses = WinHttpOpen(L"ThirdBirthdayPC-Launcher/" SR_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    int found = 0;
    if (ses) {
        WinHttpSetTimeouts(ses, 5000, 5000, 5000, 8000);
        HINTERNET con = WinHttpConnect(ses, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", L"/repos/" REPO_W L"/releases/latest", NULL,
                                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
        if (req && WinHttpSendRequest(req, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L,
                                      WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, NULL)) {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &sz, NULL);
            if (status == 200) {
                static char body[65536]; DWORD got = 0, n = 0;
                while (got < sizeof(body) - 1 && WinHttpReadData(req, body + got, (DWORD)(sizeof(body) - 1 - got), &n) && n) got += n;
                body[got] = 0;
                const char *t = strstr(body, "\"tag_name\"");
                if (t && (t = strchr(t + 10, '"')) != NULL) {
                    t++;
                    int i = 0; char tag[32];
                    while (t[i] && t[i] != '"' && i < 31) { tag[i] = t[i]; i++; }
                    tag[i] = 0;
                    if (ver_num(tag) > ver_num(SR_VERSION)) { snprintf(s_latest, sizeof(s_latest), "%s", tag); found = 1; }
                    else found = -1;
                }
            } else if (status == 404) found = -1;   /* no release published yet */
        }
        if (req) WinHttpCloseHandle(req);
        if (con) WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
    }
    PostMessageA(s_win, WM_APP_UPDATE, (WPARAM)found, 0);
    return 0;
}

/* ---- saves --------------------------------------------------------------------------------- */

#define MS "memstick"
static char s_saves[32][64];               /* folder names, ULUS10567... */
static int  s_nsaves = 0;

/* PARAM.SFO string value */
static int sfo_str(const char *path, const char *want, char *out, int cap) {
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static unsigned char b[16384];
    size_t n = fread(b, 1, sizeof(b), f);
    fclose(f);
    if (n < 20 || memcmp(b, "\0PSF", 4)) return 0;
    unsigned keys = b[8] | b[9] << 8 | b[10] << 16 | (unsigned)b[11] << 24;
    unsigned data = b[12] | b[13] << 8 | b[14] << 16 | (unsigned)b[15] << 24;
    unsigned cnt = b[16] | b[17] << 8;
    for (unsigned i = 0; i < cnt && 20 + i * 16 + 16 <= n; i++) {
        const unsigned char *e = b + 20 + i * 16;
        unsigned ko = e[0] | e[1] << 8, len = e[4] | e[5] << 8, doff = e[12] | e[13] << 8 | e[14] << 16;
        if (keys + ko >= n || data + doff + len > n) continue;
        if (strcmp((const char *)b + keys + ko, want)) continue;
        snprintf(out, (size_t)cap, "%.*s", (int)len, (const char *)b + data + doff);
        for (char *c = out; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
        return 1;
    }
    return 0;
}

static void copy_dir_files(const char *from, const char *to) {
    char pat[MAX_PATH * 2], a[MAX_PATH * 2], b[MAX_PATH * 2];
    CreateDirectoryA(to, NULL);
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof(pat), "%s/*", from);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        snprintf(a, sizeof(a), "%s/%s", from, fd.cFileName);
        snprintf(b, sizeof(b), "%s/%s", to, fd.cFileName);
        CopyFileA(a, b, FALSE);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* ---- window ---------------------------------------------------------------------------------- */

enum {
    ID_ISO_TEXT = 100, ID_ISO_BROWSE, ID_PLAY,
    ID_RES, ID_FPS60, ID_WIDE, ID_GLOW, ID_SHOWFPS, ID_SKIPINTRO, ID_UPDATECHECK,
    ID_MOUSE, ID_MOUSESENS, ID_MOUSEINV, ID_KBDSMOOTH,
    ID_SAVES, ID_SAVE_OPEN, ID_SAVE_BACKUP, ID_SAVE_RESTORE,
    ID_TEXPACKS, ID_TEXDUMP, ID_PATCHES, ID_PATCH_TOGGLE, ID_MODS_OPEN,
    ID_RELEASES, ID_UNLOCK, ID_PROGRESS, ID_MOUSEBTN, ID_CTLDLG,
};
static HFONT s_font, s_bold;
static int  s_preparing;
static void play_button_text(void);
static float s_dpi = 1.0f;                 /* layout scale: the layout is in 96-DPI pixels */
#define S(v) ((int)((v) * s_dpi + 0.5f))
#define WIN_W 1210                         /* client size (96-DPI pixels); the settings start at OX */
#define WIN_H 708
#define OX    430
#include "theme.h"
static Panel s_panels[8]; static int s_npanels = 0;
static void theme_refresh(void) {
    RECT rc; GetClientRect(s_win, &rc);
    theme_compose(rc.right, rc.bottom, s_iso, s_panels, s_npanels, s_dpi, S(OX));
    HICON ic = theme_icon(s_iso);
    if (ic) { SendMessageA(s_win, WM_SETICON, ICON_BIG, (LPARAM)ic); SendMessageA(s_win, WM_SETICON, ICON_SMALL, (LPARAM)ic); }
    InvalidateRect(s_win, NULL, TRUE);
}

static HWND mk(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id) {
    const int push = !strcmp(cls, "BUTTON") && ((style & 0xF) == BS_PUSHBUTTON || (style & 0xF) == BS_DEFPUSHBUTTON);
    if (push) style = (style & ~0xFu) | BS_OWNERDRAW;          /* drawn by theme_draw_button */
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x + OX), S(y), S(w), S(h), s_win,
                             (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    if (!push) SetWindowTheme(c, L"", L"");                    /* classic controls take the theme's colours */
    return c;
}
static HWND check(const char *text, int x, int y, int w, int id, int on) {
    HWND c = mk("BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, 22, id);
    SendMessageA(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    return c;
}
static void group(const char *text, int x, int y, int w, int h) {   /* a panel in the background */
    if (s_npanels < 8) s_panels[s_npanels++] = (Panel){ x + OX, y, w, h, text };
}

static void iso_show(void) {
    char t[MAX_PATH + 64];
    if (!s_iso[0]) snprintf(t, sizeof(t), "No disc image found - choose your copy of the game (.iso).");
    else snprintf(t, sizeof(t), "%s%s", s_iso, iso_is_game(s_iso) ? "" : "   (not the US release ULUS10567!)");
    SetDlgItemTextA(s_win, ID_ISO_TEXT, t);
    EnableWindow(GetDlgItem(s_win, ID_PLAY), s_iso[0] != 0 && !s_preparing);
}

static void saves_fill(void) {
    HWND lb = GetDlgItem(s_win, ID_SAVES);
    SendMessageA(lb, LB_RESETCONTENT, 0, 0);
    s_nsaves = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(MS "/PSP/SAVEDATA/ULUS10567*", &fd);
    if (h == INVALID_HANDLE_VALUE) { SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)"(no saves yet)"); return; }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || s_nsaves >= 32) continue;
        char sfo[MAX_PATH], title[128] = "", detail[256] = "", line[512];
        snprintf(sfo, sizeof(sfo), MS "/PSP/SAVEDATA/%s/PARAM.SFO", fd.cFileName);
        sfo_str(sfo, "SAVEDATA_TITLE", title, sizeof(title));
        sfo_str(sfo, "SAVEDATA_DETAIL", detail, sizeof(detail));
        const char *kind = strstr(fd.cFileName, "SYSDATA") ? "system data" :
                           !strcmp(fd.cFileName, "ULUS10567DATA") ? "install data" : title[0] ? title : "save";
        /* the newest file inside: the folder's own date changes when anything in it is touched */
        FILETIME newest = fd.ftLastWriteTime;
        {   char pat2[MAX_PATH]; WIN32_FIND_DATAA f2;
            snprintf(pat2, sizeof(pat2), MS "/PSP/SAVEDATA/%s/*", fd.cFileName);
            HANDLE h2 = FindFirstFileA(pat2, &f2);
            if (h2 != INVALID_HANDLE_VALUE) {
                int first = 1;
                do {
                    if (f2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    if (first || CompareFileTime(&f2.ftLastWriteTime, &newest) > 0) newest = f2.ftLastWriteTime;
                    first = 0;
                } while (FindNextFileA(h2, &f2));
                FindClose(h2);
            }
        }
        FILETIME loc; FileTimeToLocalFileTime(&newest, &loc);
        SYSTEMTIME st; FileTimeToSystemTime(&loc, &st);
        snprintf(line, sizeof(line), "%-18s  %04u-%02u-%02u %02u:%02u   %s %s%s", fd.cFileName + 9,
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, kind, detail[0] ? "- " : "", detail);
        SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)line);
        snprintf(s_saves[s_nsaves++], 64, "%s", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void patches_fill(void) {
    HWND lb = GetDlgItem(s_win, ID_PATCHES);
    SendMessageA(lb, LB_RESETCONTENT, 0, 0);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("patches/*.txt*", &fd);
    if (h == INVALID_HANDLE_VALUE) { SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)"(no patches in patches/)"); return; }
    do {
        const char *off = strstr(fd.cFileName, ".txt.off");
        char line[300];
        snprintf(line, sizeof(line), "[%s]  %.*s", off ? "off" : "on ", off ? (int)(off - fd.cFileName) : (int)strlen(fd.cFileName) - 4, fd.cFileName);
        SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)line);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static int checked(int id) { return SendMessageA(GetDlgItem(s_win, id), BM_GETCHECK, 0, 0) == BST_CHECKED; }

static void build_ui(void) {
    const int L = 16, W = 760;
    char t[128];
    snprintf(t, sizeof(t), "The 3rd Birthday  -  PC port v%s", SR_VERSION);
    group(NULL, L - 6, 4, W + 12, 64);
    HWND h = mk("STATIC", t, 0, L, 12, 440, 30, 0); SendMessageA(h, WM_SETFONT, (WPARAM)s_bold, TRUE);
    s_status = mk("STATIC", "Checking for updates...", 0, L, 42, 440, 20, 0);
    HWND pb = mk(PROGRESS_CLASSA, "", 0, L, 62, 440, 6, ID_PROGRESS);
    SendMessageA(pb, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageA(pb, PBM_SETBARCOLOR, 0, RGB(200, 20, 30)); SendMessageA(pb, PBM_SETBKCOLOR, 0, RGB(30, 10, 12));
    ShowWindow(pb, SW_HIDE);
    mk("BUTTON", "Releases page", BS_PUSHBUTTON, W - 280, 16, 120, 30, ID_RELEASES);
    HWND play = mk("BUTTON", "Play", BS_DEFPUSHBUTTON | WS_TABSTOP, W - 150, 10, 150, 48, ID_PLAY);
    SendMessageA(play, WM_SETFONT, (WPARAM)s_bold, TRUE);

    group("Disc image", L - 6, 72, W + 12, 56);
    mk("STATIC", "", SS_PATHELLIPSIS, L + 6, 96, W - 130, 20, ID_ISO_TEXT);
    mk("BUTTON", "Choose...", BS_PUSHBUTTON, W - 110, 92, 110, 26, ID_ISO_BROWSE);

    const char *G = "graphics.cfg", *C = "controls.cfg";
    group("Graphics", L - 6, 138, 370, 210);
    mk("STATIC", "Internal resolution:", 0, L + 6, 162, 140, 20, 0);
    HWND res = mk("COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, L + 150, 158, 200, 200, ID_RES);
    const char *opts[] = { "Auto (match the display)", "1x  (480 x 272, PSP)", "2x  (960 x 544)", "3x  (1440 x 816)",
                           "4x  (1920 x 1088)", "5x  (2400 x 1360)", "6x  (2880 x 1632)" };
    for (int i = 0; i < 7; i++) SendMessageA(res, CB_ADDSTRING, 0, (LPARAM)opts[i]);
    SendMessageA(res, CB_SETCURSEL, (WPARAM)cfg_int(G, "render_scale", 0), 0);
    check("60 fps (generated in-between frames)", L + 6, 190, 340, ID_FPS60, cfg_int(G, "frame_interpolation", 1));
    check("Widescreen: fill the window (wider view)", L + 6, 214, 340, ID_WIDE, cfg_int(G, "widescreen", 0));
    check("Glow effect (bloom, original)", L + 6, 238, 340, ID_GLOW, cfg_int(G, "glow", 1));
    check("FPS counter", L + 6, 262, 340, ID_SHOWFPS, cfg_int(G, "show_fps", 0));
    check("Skip the logos and opening movie", L + 6, 286, 340, ID_SKIPINTRO, cfg_int(G, "skip_intro", 0));
    check("Check for updates at start", L + 6, 310, 340, ID_UPDATECHECK, cfg_int(G, "update_check", 1));

    group("Controls", 390, 138, W - 368, 210);
    check("Mouse camera (click the game window)", 402, 162, 360, ID_MOUSE, cfg_int(C, "mouse_camera", 1));
    mk("STATIC", "Mouse sensitivity:", 0, 402, 190, 130, 20, 0);
    snprintf(t, sizeof(t), "%.2f", cfg_float(C, "mouse_sensitivity", 1.0));
    mk("EDIT", t, WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 540, 187, 70, 24, ID_MOUSESENS);
    check("Invert mouse up/down", 402, 216, 360, ID_MOUSEINV, cfg_int(C, "mouse_invert_y", 0));
    check("Smooth WASD turning", 402, 240, 360, ID_KBDSMOOTH, cfg_int(C, "keyboard_smoothing", 1));
    check("Mouse buttons (fire / aim, or as you set them)", 402, 264, 360, ID_MOUSEBTN, cfg_int(C, "mouse_buttons_fire_aim", 1));
    mk("BUTTON", "Change buttons...", BS_PUSHBUTTON | WS_TABSTOP, 402, 296, 160, 30, ID_CTLDLG);
    mk("STATIC", "Keyboard, mouse and controller", 0, 574, 296, 200, 18, 0);
    mk("STATIC", "(also in the game: Esc > Controls)", 0, 574, 314, 200, 18, 0);

    group("Saves  (backed up automatically before each save)", L - 6, 358, W + 12, 170);
    mk("LISTBOX", "", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP, L + 6, 382, W - 150, 132, ID_SAVES);
    mk("BUTTON", "Open folder", BS_PUSHBUTTON, W - 130, 382, 130, 28, ID_SAVE_OPEN);
    mk("BUTTON", "Back up all now", BS_PUSHBUTTON, W - 130, 416, 130, 28, ID_SAVE_BACKUP);
    mk("BUTTON", "Restore a backup...", BS_PUSHBUTTON, W - 130, 450, 130, 28, ID_SAVE_RESTORE);
    mk("BUTTON", "Unlock all extras", BS_PUSHBUTTON, W - 130, 484, 130, 28, ID_UNLOCK);

    group("Mods  (see mods/README.md)", L - 6, 538, W + 12, 150);
    check("Use texture packs (textures/)", L + 6, 562, 300, ID_TEXPACKS, cfg_int(G, "texture_packs", 1));
    check("Dump textures (textures/dump/)", L + 6, 586, 300, ID_TEXDUMP, cfg_int(G, "texture_dump", 0));
    mk("BUTTON", "Open mod folders", BS_PUSHBUTTON, L + 6, 618, 160, 28, ID_MODS_OPEN);
    mk("LISTBOX", "", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP, 330, 562, W - 470, 112, ID_PATCHES);
    mk("BUTTON", "Patch on / off", BS_PUSHBUTTON, W - 130, 562, 130, 28, ID_PATCH_TOGGLE);

    iso_show();
    play_button_text();
    theme_refresh();
    saves_fill();
    patches_fill();
}

static void save_settings(void) {
    const char *G = "graphics.cfg", *C = "controls.cfg";
    int r = (int)SendMessageA(GetDlgItem(s_win, ID_RES), CB_GETCURSEL, 0, 0);
    if (r >= 0) cfg_set_int(G, "render_scale", r, 0);
    cfg_set_int(G, "frame_interpolation", checked(ID_FPS60), 0);
    cfg_set_int(G, "widescreen", checked(ID_WIDE), 0);
    cfg_set_int(G, "glow", checked(ID_GLOW), 0);
    cfg_set_int(G, "show_fps", checked(ID_SHOWFPS), 0);
    cfg_set_int(G, "skip_intro", checked(ID_SKIPINTRO), 0);
    cfg_set_int(G, "update_check", checked(ID_UPDATECHECK), 0);
    cfg_set_int(G, "texture_packs", checked(ID_TEXPACKS), 0);
    cfg_set_int(G, "texture_dump", checked(ID_TEXDUMP), 0);
    cfg_set_int(C, "mouse_camera", checked(ID_MOUSE), 1);
    cfg_set_int(C, "mouse_invert_y", checked(ID_MOUSEINV), 1);
    cfg_set_int(C, "keyboard_smoothing", checked(ID_KBDSMOOTH), 1);
    cfg_set_int(C, "mouse_buttons_fire_aim", checked(ID_MOUSEBTN), 1);
    char t[32]; GetDlgItemTextA(s_win, ID_MOUSESENS, t, sizeof(t));
    double v = atof(t); if (v < 0.1) v = 0.1; if (v > 10) v = 10;
    snprintf(t, sizeof(t), "%.2f", v); cfg_write(C, "mouse_sensitivity", t, 1);
}

static void open_folder(const char *rel) {
    char full[MAX_PATH];
    CreateDirectoryA(rel, NULL);
    GetFullPathNameA(rel, sizeof(full), full, NULL);
    ShellExecuteA(s_win, "open", full, NULL, NULL, SW_SHOWNORMAL);
}

static void backup_all(void) {
    SYSTEMTIME st; GetLocalTime(&st);
    char stamp[32]; snprintf(stamp, sizeof(stamp), "%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay,
                             st.wHour, st.wMinute, st.wSecond);
    int n = 0;
    CreateDirectoryA(MS "/BACKUP", NULL);
    for (int i = 0; i < s_nsaves; i++) {
        if (!strcmp(s_saves[i], "ULUS10567DATA")) continue;          /* the 200 MB install data */
        char from[MAX_PATH], root[MAX_PATH], to[MAX_PATH * 2];
        snprintf(from, sizeof(from), MS "/PSP/SAVEDATA/%s", s_saves[i]);
        snprintf(root, sizeof(root), MS "/BACKUP/%s", s_saves[i]);
        CreateDirectoryA(root, NULL);
        snprintf(to, sizeof(to), "%s/%s-manual", root, stamp);
        copy_dir_files(from, to); n++;
    }
    char msg[160]; snprintf(msg, sizeof(msg), "Backed up %d save folders to memstick\\BACKUP\\<save>\\%s-manual.", n, stamp);
    MessageBoxA(s_win, msg, "Saves", MB_ICONINFORMATION);
}

static void restore_backup(void) {
    int sel = (int)SendMessageA(GetDlgItem(s_win, ID_SAVES), LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= s_nsaves) { MessageBoxA(s_win, "Select a save in the list first.", "Restore", MB_ICONINFORMATION); return; }
    char root[MAX_PATH], full[MAX_PATH];
    snprintf(root, sizeof(root), MS "/BACKUP/%s", s_saves[sel]);
    GetFullPathNameA(root, sizeof(full), full, NULL);
    if (GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(s_win, "There are no backups of this save yet.", "Restore", MB_ICONINFORMATION); return;
    }
    /* pick a backup folder: the folder dialog starts in this save's backups */
    char pick[MAX_PATH] = "";
    BROWSEINFOA bi = {0};
    bi.hwndOwner = s_win;
    bi.lpszTitle = "Choose the backup (date-time folder) to restore";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST start = NULL;
    { wchar_t w[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, full, -1, w, MAX_PATH); SHParseDisplayName(w, NULL, &start, 0, NULL); }
    bi.pidlRoot = start;
    LPITEMIDLIST got = SHBrowseForFolderA(&bi);
    if (!got) return;
    SHGetPathFromIDListA(got, pick);
    CoTaskMemFree(got);
    if (start) CoTaskMemFree(start);
    char test[MAX_PATH * 2]; snprintf(test, sizeof(test), "%s/PARAM.SFO", pick);
    if (GetFileAttributesA(test) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(s_win, "That folder is not a save backup (no PARAM.SFO).", "Restore", MB_ICONWARNING); return;
    }
    char q[MAX_PATH * 2 + 128];
    snprintf(q, sizeof(q), "Replace the save %s with the backup\n%s ?\n\nThe current save is backed up first.", s_saves[sel], pick);
    if (MessageBoxA(s_win, q, "Restore", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    char cur[MAX_PATH], safe[MAX_PATH * 2];
    snprintf(cur, sizeof(cur), MS "/PSP/SAVEDATA/%s", s_saves[sel]);
    SYSTEMTIME st; GetLocalTime(&st);
    snprintf(safe, sizeof(safe), "%s/%04u%02u%02u-%02u%02u%02u-before-restore", root, st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    copy_dir_files(cur, safe);
    copy_dir_files(pick, cur);
    saves_fill();
    MessageBoxA(s_win, "Restored.", "Restore", MB_ICONINFORMATION);
}

/* The game has no DLC: costumes, weapons, DNA boards and cheat codes unlock through the system
 * data (ULUS10567SYSDATA) by clearing the game. unlocks/ULUS10567SYSDATA holds system data with
 * everything unlocked; installing it keeps the game saves (progress) and backs up the old one. */
static void unlock_all(void) {
    if (GetFileAttributesA("unlocks/ULUS10567SYSDATA/SYSDATA.BIN") == INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(s_win, "The unlock data (unlocks\\ULUS10567SYSDATA) is not next to the launcher.", "Unlock all extras", MB_ICONWARNING);
        return;
    }
    if (MessageBoxA(s_win, "Unlock every costume, weapon, DNA board and the cheat codes?\n\n"
                    "This replaces the system data (your settings in the game's Config go back to\n"
                    "defaults). Your game saves and progress are kept, and the current system data\n"
                    "is backed up first.", "Unlock all extras", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    SYSTEMTIME st; GetLocalTime(&st);
    char safe[MAX_PATH];
    snprintf(safe, sizeof(safe), MS "/BACKUP/ULUS10567SYSDATA/%04u%02u%02u-%02u%02u%02u-before-unlock", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
    CreateDirectoryA(MS, NULL); CreateDirectoryA(MS "/BACKUP", NULL); CreateDirectoryA(MS "/BACKUP/ULUS10567SYSDATA", NULL);
    if (GetFileAttributesA(MS "/PSP/SAVEDATA/ULUS10567SYSDATA") != INVALID_FILE_ATTRIBUTES)
        copy_dir_files(MS "/PSP/SAVEDATA/ULUS10567SYSDATA", safe);
    CreateDirectoryA(MS "/PSP", NULL); CreateDirectoryA(MS "/PSP/SAVEDATA", NULL);
    copy_dir_files("unlocks/ULUS10567SYSDATA", MS "/PSP/SAVEDATA/ULUS10567SYSDATA");
    saves_fill();
    MessageBoxA(s_win, "Done. Start the game and load your save: the briefing menu announces the unlocks.", "Unlock all extras", MB_ICONINFORMATION);
}

static void patch_toggle(void) {
    int sel = (int)SendMessageA(GetDlgItem(s_win, ID_PATCHES), LB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    char line[300]; SendMessageA(GetDlgItem(s_win, ID_PATCHES), LB_GETTEXT, (WPARAM)sel, (LPARAM)line);
    if (line[0] != '[') return;
    const int on = !strncmp(line, "[on ]", 5);
    char from[MAX_PATH], to[MAX_PATH];
    snprintf(from, sizeof(from), "patches/%s.txt%s", line + 7, on ? "" : ".off");
    snprintf(to, sizeof(to), "patches/%s.txt%s", line + 7, on ? ".off" : "");
    MoveFileA(from, to);
    patches_fill();
}

/* ---- prepare: build game.dll from the player's disc (kit/tools/prepare.py) ----------------- */

static char s_prep_text[160], s_prep_err[2048];

/* game.dll present and built by this kit? */
static int game_ready(void) {
    if (GetFileAttributesA("game.dll") == INVALID_FILE_ATTRIBUTES) return 0;
    char kit[64] = "", built[64] = "";
    FILE *f = fopen("kit/kit_id.txt", "r");
    if (!f) return 1;                          /* dev build without a kit: trust what is there */
    if (fgets(kit, sizeof(kit), f)) kit[strcspn(kit, "\r\n")] = 0;
    fclose(f);
    if (!cfg_read("game_build.txt", "kit", built, sizeof(built))) return 0;
    return !strcmp(kit, built);
}

static void play_button_text(void) {
    SetDlgItemTextA(s_win, ID_PLAY, s_preparing ? "Preparing..." : game_ready() ? "Play" : "Prepare game");
}

static DWORD WINAPI prepare_thread(void *arg) {
    (void)arg;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { PostMessageA(s_win, WM_APP_PREP, (WPARAM)-1, 0); return 0; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = NULL;
    PROCESS_INFORMATION pi;
    char cmd[MAX_PATH * 3];
    snprintf(cmd, sizeof(cmd), "\"kit\\python\\python.exe\" \"kit\\tools\\prepare.py\" --iso \"%s\" --game-dir .", s_iso);
    /* the kit's Python must not see another Python's settings */
    SetEnvironmentVariableA("PYTHONHOME", NULL); SetEnvironmentVariableA("PYTHONPATH", NULL);
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); snprintf(s_prep_err, sizeof(s_prep_err), "Could not start the kit (kit\\python\\python.exe).");
               PostMessageA(s_win, WM_APP_PREP, (WPARAM)-1, 0); return 0; }
    char buf[4096], line[512]; int ln = 0; DWORD n;
    s_prep_err[0] = 0;
    while (ReadFile(rd, buf, sizeof(buf), &n, NULL) && n) {
        for (DWORD i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\r') continue;
            if (c != '\n' && ln < (int)sizeof(line) - 1) { line[ln++] = c; continue; }
            line[ln] = 0; ln = 0;
            int pct;
            if (sscanf(line, "PROGRESS %d", &pct) == 1) {
                const char *t = line + 13; while (*t == ' ') t++;
                snprintf(s_prep_text, sizeof(s_prep_text), "%s", t);
                PostMessageA(s_win, WM_APP_PREP, (WPARAM)pct, 0);
            } else {   /* keep the tail of anything else (errors) */
                size_t L = strlen(s_prep_err);
                if (L > sizeof(s_prep_err) - 600) { memmove(s_prep_err, s_prep_err + 600, L - 599); L = strlen(s_prep_err); }
                snprintf(s_prep_err + L, sizeof(s_prep_err) - L, "%s\n", line);
            }
        }
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    PostMessageA(s_win, WM_APP_PREP, code == 0 && game_ready() ? 101 : (WPARAM)-1, 0);
    return 0;
}

static void prepare(void) {
    if (s_preparing || !s_iso[0]) return;
    if (GetFileAttributesA("kit/tools/prepare.py") == INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(s_win, "The kit folder is missing next to the launcher.", "Prepare game", MB_ICONERROR); return;
    }
    if (MessageBoxA(s_win, "The game is built once from your disc image, on this PC (about 3-10 minutes,\n"
                    "depending on the processor). Nothing is downloaded.\n\nStart now?", "Prepare game",
                    MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    s_preparing = 1;
    play_button_text();
    EnableWindow(GetDlgItem(s_win, ID_PLAY), FALSE);
    EnableWindow(GetDlgItem(s_win, ID_ISO_BROWSE), FALSE);
    ShowWindow(GetDlgItem(s_win, ID_PROGRESS), SW_SHOW);
    HANDLE t = CreateThread(NULL, 0, prepare_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

static void play(void) {
    if (!game_ready()) { prepare(); return; }
    save_settings();
    if (!s_iso[0]) return;
    cfg_write("launch.cfg", "iso", s_iso, 0);          /* the game opens exactly this disc image */
    STARTUPINFOA si = { sizeof(si) }; PROCESS_INFORMATION pi;
    char cmd[MAX_PATH] = GAME_EXE;
    if (CreateProcessA(GAME_EXE, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        PostQuitMessage(0);
    } else MessageBoxA(s_win, "Could not start " GAME_EXE " (is it next to the launcher?).", "Play", MB_ICONERROR);
}

#include "controls_dlg.h"

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_PLAY: play(); return 0;
        case ID_ISO_BROWSE: {
            char file[MAX_PATH] = "";
            OPENFILENAMEA of = { sizeof(of) };
            of.hwndOwner = w; of.lpstrFilter = "PSP disc image (*.iso)\0*.iso\0All files\0*.*\0";
            of.lpstrFile = file; of.nMaxFile = sizeof(file); of.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            of.lpstrTitle = "Your disc image of The 3rd Birthday (USA)";
            if (GetOpenFileNameA(&of)) {
                if (!iso_is_game(file) && MessageBoxA(w, "This does not look like The 3rd Birthday (USA, ULUS10567).\n"
                                                       "Use it anyway?", "Disc image", MB_YESNO | MB_ICONWARNING) != IDYES)
                    return 0;
                snprintf(s_iso, sizeof(s_iso), "%s", file);
                cfg_write("launch.cfg", "iso", file, 0);
                iso_show();
                theme_refresh();
            }
            return 0;
        }
        case ID_RELEASES: ShellExecuteA(w, "open", "https://github.com/" REPO "/releases", NULL, NULL, SW_SHOWNORMAL); return 0;
        case ID_SAVE_OPEN: open_folder(MS "\\PSP\\SAVEDATA"); return 0;
        case ID_SAVE_BACKUP: backup_all(); return 0;
        case ID_SAVE_RESTORE: restore_backup(); return 0;
        case ID_MODS_OPEN: CreateDirectoryA("patches", NULL); open_folder("textures"); open_folder("patches"); return 0;
        case ID_PATCH_TOGGLE: patch_toggle(); return 0;
        case ID_UNLOCK: unlock_all(); return 0;
        case ID_CTLDLG: save_settings(); controls_dialog(); return 0;
        }
        break;
    case WM_APP_PREP: {
        const int pct = (int)wp;
        if (pct >= 0 && pct <= 100) {
            SendMessageA(GetDlgItem(w, ID_PROGRESS), PBM_SETPOS, (WPARAM)pct, 0);
            char t[200]; snprintf(t, sizeof(t), "Preparing the game: %s", s_prep_text);
            SetWindowTextA(s_status, t);
            return 0;
        }
        s_preparing = 0;
        ShowWindow(GetDlgItem(w, ID_PROGRESS), SW_HIDE);
        EnableWindow(GetDlgItem(w, ID_ISO_BROWSE), TRUE);
        iso_show();
        play_button_text();
        if (pct == 101) {
            SetWindowTextA(s_status, "The game is ready. Press Play.");
        } else {
            SetWindowTextA(s_status, "Preparing the game failed - see the message.");
            char m[2300]; snprintf(m, sizeof(m), "Preparing the game failed.\n\n%s", s_prep_err[0] ? s_prep_err : "(no details)");
            MessageBoxA(w, m, "Prepare game", MB_ICONERROR);
        }
        return 0;
    }
    case WM_APP_UPDATE:
        if (s_preparing) return 0;
        if ((int)wp == 1) {
            char t[128]; snprintf(t, sizeof(t), "Update available: %s  (you have v%s) - see the Releases page", s_latest, SR_VERSION);
            SetWindowTextA(s_status, t);
        } else SetWindowTextA(s_status, (int)wp == -1 ? "You have the latest version." : "Could not check for updates (offline?).");
        return 0;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: {
        HDC dc = (HDC)wp; HWND c = (HWND)lp;
        if (GetDlgCtrlID(c) == ID_MOUSESENS || GetDlgCtrlID(c) == ID_RES) {   /* read-only edit / combo look */
            SetTextColor(dc, TH_TEXT); SetBkColor(dc, TH_FIELD); return (LRESULT)s_fieldbrush;
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, c == s_status && s_latest[0] ? RGB(255, 140, 60) : IsWindowEnabled(c) ? TH_TEXT : TH_DIM);
        if (!s_bgbrush) return (LRESULT)GetStockObject(BLACK_BRUSH);
        RECT r; GetWindowRect(c, &r); MapWindowPoints(NULL, w, (POINT *)&r, 2);
        SetBrushOrgEx(dc, -r.left, -r.top, NULL);            /* the background shows through */
        return (LRESULT)s_bgbrush;
    }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)wp, TH_TEXT); SetBkColor((HDC)wp, TH_FIELD);
        return (LRESULT)(s_fieldbrush ? s_fieldbrush : GetStockObject(BLACK_BRUSH));
    case WM_ERASEBKGND:
        if (s_bg) {
            HDC dc = (HDC)wp, mdc = CreateCompatibleDC(dc);
            HGDIOBJ o = SelectObject(mdc, s_bg);
            BitBlt(dc, 0, 0, s_bgw, s_bgh, mdc, 0, 0, SRCCOPY);
            SelectObject(mdc, o); DeleteDC(mdc);
            return 1;
        }
        break;
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *d = (const DRAWITEMSTRUCT *)lp;
        if (d->CtlType == ODT_BUTTON) { theme_draw_button(d, d->CtlID == ID_PLAY ? s_bold : s_font, d->CtlID == ID_PLAY); return TRUE; }
        break;
    }
    case WM_CLOSE:
        if (s_preparing && MessageBoxA(w, "The game is still being prepared. Stop and close?", "The 3rd Birthday",
                                       MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
        save_settings(); DestroyWindow(w); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(w, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show) {
    (void)prev; (void)cmdline;
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, sizeof(exe));
    char *sl = strrchr(exe, '\\'); if (sl) *sl = 0;
    SetCurrentDirectoryA(exe);                 /* settings, saves and mods live next to the game */
    CoInitialize(NULL);
    { INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_PROGRESS_CLASS }; InitCommonControlsEx(&ic); }
    NONCLIENTMETRICSA ncm = { sizeof(ncm) };
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    s_font = CreateFontIndirectA(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_BOLD; ncm.lfMessageFont.lfHeight = ncm.lfMessageFont.lfHeight * 5 / 4;
    s_bold = CreateFontIndirectA(&ncm.lfMessageFont);
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.lpszClassName = "ThirdBirthdayLauncher";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));
    RegisterClassA(&wc);
    { HDC dc = GetDC(NULL); s_dpi = GetDeviceCaps(dc, LOGPIXELSX) / 96.0f; ReleaseDC(NULL, dc); }
    if (s_dpi < 1.0f) s_dpi = 1.0f;
    RECT r = { 0, 0, S(WIN_W), S(WIN_H) };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    s_win = CreateWindowA("ThirdBirthdayLauncher", "The 3rd Birthday - Launcher", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                          CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    iso_find();
    build_ui();
    if (getenv("SR_OFFSCREEN")) SetWindowPos(s_win, NULL, -4000, 200, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(s_win, getenv("SR_OFFSCREEN") ? SW_SHOWNOACTIVATE : show);
    if (cfg_int("graphics.cfg", "update_check", 1)) { HANDLE t = CreateThread(NULL, 0, update_thread, NULL, 0, NULL); if (t) CloseHandle(t); }
    else SetWindowTextA(s_status, "Update check is off.");
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (cd_capture_msg(&msg)) continue;                    /* Change buttons: waiting for a key */
        if (s_cd && IsDialogMessageA(s_cd, &msg)) continue;
        if (IsDialogMessageA(s_win, &msg)) continue;
        TranslateMessage(&msg); DispatchMessageA(&msg);
    }
    return 0;
}
