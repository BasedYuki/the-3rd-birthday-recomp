/* Cheats (Esc menu > Cheats), saved in cheats.cfg next to the executable.
 *
 * Two kinds, both from the community CWCheat database for the US release (PPSSPP's cheat.db):
 *  - instruction patches (versions/<game>.toml [patches]): the recompiled code holds the original
 *    and the patched instruction and checks a flag in game.dll (sr_game_patch_flags) each time;
 *  - data cheats: a value written into the game's own memory every vblank (only addresses inside
 *    the game module's data, which sit at the same place as on a PSP; heap addresses can differ
 *    in the port, so cheats on those are left out). */

#include "recomp.h"
#include "game_addrs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { K_PATCH, K_U32, K_F32 };
static const struct { const char *id, *label; int kind, patch; uint32_t addr; uint32_t value; } k_cheat[] = {
    { "no_damage",      "No damage (infinite health)", K_PATCH, PATCH_NO_DAMAGE,    0, 0 },
    { "inf_ammo",       "Infinite ammo",               K_PATCH, PATCH_INF_AMMO,     0, 0 },
    { "inf_grenades",   "Infinite grenades",           K_PATCH, PATCH_INF_GRENADES, 0, 0 },
    { "inf_liberation", "Infinite Liberation",         K_F32,   0, ADDR_LIBERATION_GAUGE, 0x447A0000u },   /* 1000.0 */
    { "max_bp",         "Max BP (999,999)",            K_U32,   0, ADDR_BP,               999999u },
};
enum { NCHEAT = sizeof(k_cheat) / sizeof(k_cheat[0]) };
static int s_on[NCHEAT], s_loaded;

uint8_t *g_patch_flags;   /* game.dll's sr_patch_on (driver.c load_game); NULL with an older DLL */

static const char *cfg_path(void) {   /* SR_CHEATS_CFG: test runs keep the player's file */
    const char *e = getenv("SR_CHEATS_CFG");
    return e && *e ? e : "cheats.cfg";
}
static void load(void) {
    s_loaded = 1;
    FILE *f = fopen(cfg_path(), "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '='); if (!eq || line[0] == '#') continue;
        *eq = 0;
        char *id = line; while (*id == ' ') id++;
        size_t n = strlen(id); while (n && id[n - 1] == ' ') id[--n] = 0;
        for (int i = 0; i < NCHEAT; i++) if (!strcmp(id, k_cheat[i].id)) s_on[i] = atoi(eq + 1) != 0;
    }
    fclose(f);
}
static void save(void) {
    FILE *f = fopen(cfg_path(), "w");
    if (!f) return;
    fprintf(f, "# cheats (Esc menu > Cheats): 1 = on\n");
    for (int i = 0; i < NCHEAT; i++) fprintf(f, "%s = %d\n", k_cheat[i].id, s_on[i]);
    fclose(f);
}

/* every vblank (gui.c gui_vblank) */
void sr_cheats_vblank(void) {
    if (!s_loaded) load();
    for (int i = 0; i < NCHEAT; i++) {
        if (k_cheat[i].kind == K_PATCH) {
            if (g_patch_flags) g_patch_flags[k_cheat[i].patch] = (uint8_t)s_on[i];
        } else if (s_on[i]) {
            MEM_W32(k_cheat[i].addr, k_cheat[i].value);
        }
    }
}

/* for the Cheats page (gui.c) */
int  sr_cheat_count(void) { return NCHEAT; }
int  sr_cheat_available(int i) { return k_cheat[i].kind != K_PATCH || g_patch_flags != NULL; }
void sr_cheat_text(int i, char *out, int cap) {
    if (!s_loaded) load();
    if (!sr_cheat_available(i)) snprintf(out, (size_t)cap, "%s\t(prepare the game again)", k_cheat[i].label);
    else snprintf(out, (size_t)cap, "%s\t%s", k_cheat[i].label, s_on[i] ? "On" : "Off");
}
void sr_cheat_toggle(int i) {
    if (i < 0 || i >= NCHEAT || !sr_cheat_available(i)) return;
    s_on[i] = !s_on[i];
    fprintf(stderr, "cheats: %s %s\n", k_cheat[i].id, s_on[i] ? "on" : "off");
    save();
    sr_cheats_vblank();
}
void sr_cheats_all_off(void) {
    for (int i = 0; i < NCHEAT; i++) s_on[i] = 0;
    save(); sr_cheats_vblank();
}
