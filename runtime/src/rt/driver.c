/* Bring-up driver for the recompiled C (Phase 3 differential).
 *
 * Loads a statically-linked PSP ELF into guest memory, seeds the CpuState from the entry
 * register state the PPSSPP reference trace recorded ("# init" line in the trace), registers
 * every generated function, and runs the entry function with tracing on. Execution stops
 * cleanly at the first HLE call (sr_hle_call longjmps), exactly where the reference trace is
 * truncated for comparison.
 *
 * Usage: driver <elf> <ref-trace> <out-trace>
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"
#ifndef SR_VERSION
#define SR_VERSION "0.0.0"
#endif
#define SR_VERSION_STR SR_VERSION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* The recompiled game code lives in game.dll, built on the player's PC from their own disc
 * (tools/prepare.py); load it and give it the runtime's table (recomp.h SrApi). */
extern int sr_sched_on; extern int32_t sr_timeslice;
uint32_t sr_lwl(uint32_t, uint32_t); uint32_t sr_lwr(uint32_t, uint32_t);
void sr_swl(uint32_t, uint32_t); void sr_swr(uint32_t, uint32_t);
static int load_game(void) {
    const char *name = getenv("SR_GAME_DLL");
    HMODULE m = LoadLibraryA(name && *name ? name : "game.dll");
    if (!m) {
        MessageBoxA(NULL, "The game is not prepared yet.\n\nStart \"The 3rd Birthday.exe\" (the launcher) and choose your "
                    "disc image: it builds the game from it once.", "The 3rd Birthday", MB_ICONINFORMATION);
        return 0;
    }
    int (*attach)(const struct SrApi *, int) = (int (*)(const struct SrApi *, int))(void *)GetProcAddress(m, "sr_game_attach");
    void (*reg)(void) = (void (*)(void))(void *)GetProcAddress(m, "sr_game_register_all");
    extern int (*g_game_mods_hook)(CpuState *, uint32_t);
    g_game_mods_hook = (int (*)(CpuState *, uint32_t))(void *)GetProcAddress(m, "sr_game_mods_hook");
    static struct SrApi api;
    api.mem = &g_mem; api.dbg = &g_sr_dbg; api.sched_on = &sr_sched_on; api.timeslice = &sr_timeslice;
    api.oor = sr_oor; api.lwl = sr_lwl; api.lwr = sr_lwr; api.swl = sr_swl; api.swr = sr_swr;
    api.vread = sr_vread; api.vwrite = sr_vwrite;
    api.vfpu_rcp = sr_vfpu_rcp; api.vfpu_rsqrt = sr_vfpu_rsqrt; api.vfpu_sqrt = sr_vfpu_sqrt;
    api.vfpu_sin = sr_vfpu_sin; api.vfpu_cos = sr_vfpu_cos; api.vfpu_exp2 = sr_vfpu_exp2; api.vfpu_ext = sr_vfpu_ext;
    api.reg = sr_register; api.dispatch = dispatch; api.begin_dbg = sr_begin_dbg; api.end_dbg = sr_end_dbg;
    api.func_hook = sr_func_hook; api.syscall = sr_syscall; api.yield = sr_yield;
    api.unimplemented = sr_unimplemented; api.toast = gui_toast;
    if (!attach || !reg || !attach(&api, SR_API_VERSION)) {
        MessageBoxA(NULL, "game.dll is from another version of the port.\n\nStart \"The 3rd Birthday.exe\" (the launcher): "
                    "it prepares the game again.", "The 3rd Birthday", MB_ICONWARNING);
        return 0;
    }
    reg();
    {   /* cheats: the instruction-patch flags in game.dll (none in a DLL from before cheats) */
        extern uint8_t *g_patch_flags;
        uint8_t *(*pf)(void) = (uint8_t *(*)(void))(void *)GetProcAddress(m, "sr_game_patch_flags");
        g_patch_flags = pf ? pf() : NULL;
    }
    return 1;
}

/* Last-resort crash reporter: a silent host fault (access violation etc.) otherwise kills
 * the process with nothing in the log. Maps a faulting address inside the guest arena back
 * to its guest address. */
static LONG WINAPI sr_crash_filter(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    fprintf(stderr, "HOST CRASH: exception 0x%08lx at host %p\n",
            er->ExceptionCode, er->ExceptionAddress);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const uint8_t *fa = (const uint8_t *)er->ExceptionInformation[1];
        fprintf(stderr, "  %s of host %p", er->ExceptionInformation[0] ? "write" : "read", fa);
        if (g_mem && fa >= g_mem - 0x04000000 && fa < g_mem + 0x04000000)
            fprintf(stderr, " (guest 0x%08x)", (uint32_t)(fa - g_mem) + 0x08000000u);
        fprintf(stderr, "\n");
    }
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;   /* still crash (after reporting) */
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static uint8_t *read_file(const char *path, long *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = (uint8_t *)malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) { fprintf(stderr, "short read\n"); exit(2); }
    fclose(f);
    *out_len = n;
    return d;
}

static uint32_t load_elf(const uint8_t *elf) {
    if (memcmp(elf, "\x7f""ELF", 4) != 0) { fprintf(stderr, "not an ELF\n"); exit(2); }
    uint32_t e_entry = rd32(elf + 24);
    uint32_t e_phoff = rd32(elf + 28);
    uint16_t phentsize = rd16(elf + 42);
    uint16_t phnum = rd16(elf + 44);
    sr_mem_init();
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *ph = elf + e_phoff + (uint32_t)i * phentsize;
        uint32_t p_type = rd32(ph + 0), p_off = rd32(ph + 4), p_vaddr = rd32(ph + 8);
        uint32_t p_filesz = rd32(ph + 16);
        if (p_type == 1 && p_filesz)  /* PT_LOAD */
            sr_load_segment(p_vaddr, elf + p_off, p_filesz);
    }
    return e_entry;
}

static void seed_from_init(const char *trace_path, CpuState *s) {
    FILE *f = fopen(trace_path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", trace_path); exit(2); }
    char buf[8192];
    int found = 0;
    while (fgets(buf, sizeof(buf), f)) {
        if (strncmp(buf, "# init", 6) != 0)
            continue;
        found = 1;
        char *tok = strtok(buf + 6, " \t\n");
        while (tok) {
            char *eq = strchr(tok, '=');
            if (eq) {
                *eq = 0;
                uint32_t val = (uint32_t)strtoul(eq + 1, NULL, 16);
                if (tok[0] == 'r') s->r[atoi(tok + 1)] = val;
                else if (tok[0] == 'f' && tok[1] >= '0' && tok[1] <= '9') s->fi[atoi(tok + 1)] = val;
                else if (strcmp(tok, "hi") == 0) s->hi = val;
                else if (strcmp(tok, "lo") == 0) s->lo = val;
                else if (strcmp(tok, "fcr31") == 0) s->fcr31 = val;
            }
            tok = strtok(NULL, " \t\n");
        }
        break;
    }
    fclose(f);
    if (!found) { fprintf(stderr, "no '# init' in %s\n", trace_path); exit(2); }
}

/* Started with no arguments (a double-click, a shortcut): run the game from the executable's
 * folder. launch.cfg (written by scripts/build.sh) holds "image=", "base=", "entry=" and
 * optionally "iso="; without iso= the first *.iso in the folder or up to three folders above
 * is used. Logs go to run.err / run.out, and a console window opened only for us is closed. */
static int launch_defaults(int *pargc, char ***pargv) {
    static char exe[MAX_PATH], image[MAX_PATH] = "image.bin", base[32] = "08804000", entry[32] = "",
                iso[MAX_PATH] = "";
    static char *args[9];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    char dir[MAX_PATH]; snprintf(dir, sizeof(dir), "%s", exe);
    char *sl = strrchr(dir, '\\'); if (sl) *sl = 0;
    SetCurrentDirectoryA(dir);
    FILE *f = fopen("launch.cfg", "r");
    if (!f) return 0;
    char line[600];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "image=", 6)) snprintf(image, sizeof(image), "%s", line + 6);
        else if (!strncmp(line, "base=", 5)) snprintf(base, sizeof(base), "%s", line + 5);
        else if (!strncmp(line, "entry=", 6)) snprintf(entry, sizeof(entry), "%s", line + 6);
        else if (!strncmp(line, "iso=", 4)) snprintf(iso, sizeof(iso), "%s", line + 4);
    }
    fclose(f);
    if (!entry[0]) return 0;
    if (!getenv("PSP_ISO")) {
        if (!iso[0]) {
            char pat[MAX_PATH + 16], up[MAX_PATH] = ".";
            for (int depth = 0; depth < 4 && !iso[0]; depth++) {
                WIN32_FIND_DATAA fd;
                snprintf(pat, sizeof(pat), "%s/*.iso", up);
                HANDLE h = FindFirstFileA(pat, &fd);
                if (h != INVALID_HANDLE_VALUE) { snprintf(iso, sizeof(iso), "%s/%s", up, fd.cFileName); FindClose(h); }
                snprintf(pat, sizeof(pat), "%s/..", up); snprintf(up, sizeof(up), "%s", pat);
            }
        }
        if (!iso[0] || GetFileAttributesA(iso) == INVALID_FILE_ATTRIBUTES) {
            char msg[MAX_PATH + 200];
            snprintf(msg, sizeof(msg), "The disc image of the game was not found%s%s.\n\n"
                     "Start \"The 3rd Birthday.exe\" (the launcher) and choose your .iso.",
                     iso[0] ? ":\n" : "", iso);
            if (getenv("SR_OFFSCREEN")) {               /* tests: no dialog */
                FILE *e = fopen("run.err", "w"); if (e) { fprintf(e, "%s\n", msg); fclose(e); }
            } else MessageBoxA(NULL, msg, "The 3rd Birthday", MB_ICONERROR);
            exit(1);
        }
        if (iso[0]) { static char e[MAX_PATH + 16]; snprintf(e, sizeof(e), "PSP_ISO=%s", iso); _putenv(e); }
    }
    if (!getenv("PSP_VFPU_TABLES") && GetFileAttributesA("assets/vfpu") == INVALID_FILE_ATTRIBUTES) {
        static const char *const cand[] = { "../../runtime/assets/vfpu", "../../assets/vfpu" };
        for (unsigned i = 0; i < 2; i++)
            if (GetFileAttributesA(cand[i]) != INVALID_FILE_ATTRIBUTES) {
                static char e[96]; snprintf(e, sizeof(e), "PSP_VFPU_TABLES=%s", cand[i]); _putenv(e); break;
            }
    }
    freopen("run.err", "w", stderr);
    freopen("run.out", "w", stdout);
    setvbuf(stderr, NULL, _IONBF, 0);   /* the log is complete even after a crash */
    {   /* a console window that exists only for this process (double-click): close it */
        DWORD pids[2];
        /* hidden, not freed: FreeConsole leaves the redirected stderr writing nowhere */
        if (GetConsoleProcessList(pids, 2) == 1) ShowWindow(GetConsoleWindow(), SW_HIDE);
    }
    args[0] = exe; args[1] = "--image"; args[2] = image; args[3] = base; args[4] = entry;
    args[5] = "init.trace"; args[6] = "none"; args[7] = "--gui"; args[8] = NULL;
    *pargc = 8; *pargv = args;
    return 1;
}

int main(int argc, char **argv) {
    uint32_t entry;
    const char *ref_trace, *out;
    if (argc == 1 && !launch_defaults(&argc, &argv)) {
        MessageBoxA(NULL, "launch.cfg is missing next to the game executable.\nBuild the game with scripts/build.ps1.",
                    "The 3rd Birthday", MB_ICONERROR);
        return 2;
    }

    SetUnhandledExceptionFilter(sr_crash_filter);
    { extern void ctrlsrv_start(void); ctrlsrv_start(); }   /* SR_CTRL test channel */

    /* Image mode: a pre-relocated flat image (e.g. a rebased PRX from tools/prxload.py) is
     * loaded at <base> and run from <entry>. Required for relocatable PRXs, which must be
     * rebased + relocated before they have concrete addresses.
     *   driver --image <image.bin> <base-hex> <entry-hex> <ref-trace> <out-trace>  */
    if (argc >= 7 && strcmp(argv[1], "--image") == 0) {
        long len;
        uint8_t *img = read_file(argv[2], &len);
        uint32_t base = (uint32_t)strtoul(argv[3], NULL, 16);
        entry = (uint32_t)strtoul(argv[4], NULL, 16);
        ref_trace = argv[5];
        out = argv[6];
        sr_mem_init();
        sr_load_segment(base, img, (uint32_t)len);
        goto have_image;
    }
    if (argc < 4) {
        fprintf(stderr, "usage: driver <elf> <ref-trace> <out-trace>\n"
                        "       driver --image <image.bin> <base-hex> <entry-hex> <ref-trace> <out-trace>\n");
        return 2;
    }
    {
        long len;
        uint8_t *elf = read_file(argv[1], &len);
        entry = load_elf(elf);
    }
    ref_trace = argv[2];
    out = argv[3];
have_image:;

    CpuState s;
    memset(&s, 0, sizeof(s));
    s.vfpuCtrl[0] = 0xe4;  /* SPREFIX = identity */
    s.vfpuCtrl[1] = 0xe4;  /* TPREFIX = identity */
    s.vfpuCtrl[2] = 0;     /* DPREFIX = none */
    seed_from_init(ref_trace, &s);
    s.pc = entry;

    if (!load_game()) return 2;
    /* "none" as the out path disables tracing -- much faster for HLE bring-up, where the goal
     * is to reach the next unimplemented import rather than diff a trace. */
    if (strcmp(out, "none") != 0 && sr_trace_open(out, "cgtest", entry) != 0) {
        fprintf(stderr, "cannot open out trace %s\n", out);
        return 2;
    }
    RecompFn fn = sr_lookup(entry);
    if (!fn) { fprintf(stderr, "no recompiled function at entry 0x%08x\n", entry); return 2; }

    int use_sched = 0;
    for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--sched") == 0) use_sched = 1;
    for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--gui") == 0) { gui_init("The 3rd Birthday  (PC port v" SR_VERSION_STR ")"); use_sched = 1; }

    if (use_sched) {
        /* Run with the cooperative scheduler so the game's threads interleave (the boot busy-
         * waits on a sibling). sched_run uses s as the live register file for whichever thread
         * runs; it returns when no thread is runnable, or the process exits at an unimplemented
         * import inside a thread fiber. */
        sched_init(&s);
        sched_run(entry, s.r[4], s.r[5]);
    } else if (setjmp(g_hle_jmp) == 0) {
        fn(&s);
    }
    sr_trace_close();
    fprintf(stderr, "done (hit_hle=%d)\n", sr_hit_hle);
    return 0;
}
