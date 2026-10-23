/* Data mods: memory patches from patches/*.txt next to the executable.
 *
 * Each line:   <hex address>  <u8|u16|u32|f32>  <value>  [once|always]
 *   08C3C700  u32  0x00000001  always     # re-written every vblank (the default)
 *   08C3C704  f32  1.5         once       # written once, at the first vblank
 * '#' starts a comment. Patches change game DATA in memory; addresses inside the game's code
 * do nothing, because the code is recompiled (code mods are C, see mods/README.md and
 * sr_mods_hook below, which scripts/build.sh generates when mods/ has code mods).
 *
 * Texture packs (textures/<hash>.png) are in gpu_sdl3vk/ge_gpu.c. */

#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef struct { uint32_t addr, value; uint8_t size, always; } Patch;
static Patch *s_patch = NULL;
static int s_npatch = 0, s_loaded = 0, s_once_done = 0;

static void load_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    int n = 0, lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *c = strchr(line, '#'); if (c) *c = 0;
        char type[8] = "", val[64] = "", when[16] = "always";
        unsigned long addr;
        if (sscanf(line, "%lx %7s %63s %15s", &addr, type, val, when) < 3) continue;
        Patch p = { (uint32_t)addr, 0, 0, (uint8_t)strcmp(when, "once") != 0 };
        if (!strcmp(type, "u8")) p.size = 1;
        else if (!strcmp(type, "u16")) p.size = 2;
        else if (!strcmp(type, "u32")) p.size = 4;
        else if (!strcmp(type, "f32")) p.size = 8;
        else { fprintf(stderr, "mods: %s:%d: unknown type '%s'\n", path, lineno, type); continue; }
        if (p.size == 8) { float fv = strtof(val, NULL); memcpy(&p.value, &fv, 4); }
        else p.value = (uint32_t)strtoul(val, NULL, 0);
        if (p.addr < 0x08000000u || p.addr >= 0x0A000000u) {
            fprintf(stderr, "mods: %s:%d: address %08x is outside game RAM\n", path, lineno, p.addr);
            continue;
        }
        Patch *grown = realloc(s_patch, (size_t)(s_npatch + 1) * sizeof(Patch));
        if (!grown) break;
        s_patch = grown; s_patch[s_npatch++] = p; n++;
    }
    fclose(f);
    fprintf(stderr, "mods: %s: %d patches\n", path, n);
}

static void load_all(void) {
    s_loaded = 1;
    const char *md = getenv("SR_MODDIR");   /* test runs keep the player's folder clean */
    char pat[MAX_PATH];
    snprintf(pat, sizeof(pat), "%s/patches/*.txt", md && *md ? md : ".");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char path[MAX_PATH * 2];
        snprintf(path, sizeof(path), "%s/patches/%s", md && *md ? md : ".", fd.cFileName);
        load_file(path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void apply(const Patch *p) {
    switch (p->size) {
        case 1: MEM_W8(p->addr, (uint8_t)p->value); break;
        case 2: MEM_W16(p->addr, (uint16_t)p->value); break;
        default: MEM_W32(p->addr, p->value); break;
    }
}

/* every vblank (gui.c gui_vblank) */
void sr_mods_vblank(void) {
    if (!s_loaded) load_all();
    if (!s_npatch) return;
    for (int i = 0; i < s_npatch; i++)
        if (s_patch[i].always || !s_once_done) apply(&s_patch[i]);
    s_once_done = 1;
}

/* Code mods are compiled into game.dll (with the game code, see tools/prepare.py); driver.c
 * sets this to the DLL's sr_game_mods_hook. */
int (*g_game_mods_hook)(CpuState *, uint32_t) = NULL;
int sr_mods_hook(CpuState *s, uint32_t addr) { return g_game_mods_hook ? g_game_mods_hook(s, addr) : 0; }
