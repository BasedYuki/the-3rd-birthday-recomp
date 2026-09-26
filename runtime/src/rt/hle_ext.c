/* Additional HLE handlers brought up for The 3rd Birthday (ULUS10567), kept out of hle.c so
 * the vendored runtime's own file stays close to upstream. Behaviour and error codes follow
 * PPSSPP's Core/HLE (GPL-2.0+), which this runtime already treats as its reference.
 *
 * Registered from sr_hle_init() via sr_hle_init_ext().
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define A0 (s->r[4])
#define A1 (s->r[5])
#define A2 (s->r[6])
#define A3 (s->r[7])
#define T0 (s->r[8])

/* From hle.c */
uint32_t sr_alloc_uid(void);
uint32_t sr_user_alloc(uint32_t size, uint32_t align);
void sr_hle_register(uint32_t nid, const char *name, HleFn fn);
uint32_t sched_current_uid(void);
int sched_current_priority(void);

enum {
    ERR_ILLEGAL_ARGUMENT  = 0x800200d2,
    ERR_NO_MEMORY         = 0x80020190,
    ERR_ILLEGAL_ATTR      = 0x80020191,
    ERR_UNKNOWN_SEMID     = 0x80020199,
    ERR_UNKNOWN_FPLID     = 0x8002019d,
    ERR_ILLEGAL_MEMSIZE   = 0x800201b7,
    ERR_ILLEGAL_SIZE      = 0x800201bc,
    ERR_ILLEGAL_COUNT     = 0x800201bd,
    ERR_WAIT_CANCEL       = 0x800201a9,
    LWMUTEX_NOT_FOUND     = 0x800201ca,
    LWMUTEX_LOCKED        = 0x800201cb,
    LWMUTEX_UNLOCKED      = 0x800201cc,
    LWMUTEX_UNDERFLOW     = 0x800201ce,
    LWMUTEX_OVERFLOW      = 0x800201cd,
};

static uint32_t h_zero(CpuState *s) { (void)s; return 0; }

/* ---- SysMem ---- */

extern uint32_t g_sdk_version;   /* hle.c, read by sceKernelGetCompiledSdkVersion */
static uint32_t h_SetCompiledSdkVersionAny(CpuState *s) { g_sdk_version = A0; return 0; }

/* ---- Fixed-size memory pools (sceKernel*Fpl) ---- */

typedef struct { uint32_t uid, addr, block, count; uint8_t *used; } Fpl;
static Fpl s_fpl[64];

static Fpl *fpl_find(uint32_t uid) {
    for (int i = 0; i < 64; i++) if (s_fpl[i].uid == uid && s_fpl[i].used) return &s_fpl[i];
    return NULL;
}

/* sceKernelCreateFpl(name, partition, attr, blockSize, numBlocks, opt) */
static uint32_t h_CreateFpl(CpuState *s) {
    uint32_t part = A1, bsize = A3, nblocks = T0, opt = s->r[9];
    if (part < 1 || part > 6) return ERR_ILLEGAL_ARGUMENT;
    if (bsize == 0 || nblocks == 0) return ERR_ILLEGAL_MEMSIZE;
    uint32_t align = 4;
    if (opt && MEM_R32(opt) >= 8) align = MEM_R32(opt + 4);
    if (align & (align - 1)) return ERR_ILLEGAL_ARGUMENT;
    if (align < 4) align = 4;
    uint32_t aligned = (bsize + align - 1) & ~(align - 1);
    if ((uint64_t)aligned * nblocks > 0xFFFFFFFFull) return ERR_ILLEGAL_MEMSIZE;
    for (int i = 0; i < 64; i++) {
        if (s_fpl[i].used) continue;
        uint32_t addr = sr_user_alloc(aligned * nblocks, align);
        if (!addr) return ERR_NO_MEMORY;
        s_fpl[i].uid = sr_alloc_uid();
        s_fpl[i].addr = addr;
        s_fpl[i].block = aligned;
        s_fpl[i].count = nblocks;
        s_fpl[i].used = (uint8_t *)calloc(nblocks, 1);
        if (getenv("SR_FPLLOG"))
            fprintf(stderr, "CreateFpl uid=0x%x addr=0x%08x block=%u n=%u\n",
                    s_fpl[i].uid, addr, aligned, nblocks);
        return s_fpl[i].uid;
    }
    return ERR_NO_MEMORY;
}

/* sceKernelAllocateFpl(uid, u32 *outBlock, timeoutPtr). No thread ever frees blocks in this
 * game (no sceKernelFreeFpl import), so an exhausted pool is a hard error, not a wait. */
static uint32_t h_AllocateFpl(CpuState *s) {
    Fpl *f = fpl_find(A0);
    if (!f) return ERR_UNKNOWN_FPLID;
    for (uint32_t i = 0; i < f->count; i++) {
        if (!f->used[i]) {
            f->used[i] = 1;
            MEM_W32(A1, f->addr + i * f->block);
            return 0;
        }
    }
    fprintf(stderr, "HLE: fpl 0x%x exhausted (%u blocks); would block forever\n", A0, f->count);
    return ERR_NO_MEMORY;
}

static uint32_t h_DeleteFpl(CpuState *s) {
    Fpl *f = fpl_find(A0);
    if (!f) return ERR_UNKNOWN_FPLID;
    free(f->used);
    memset(f, 0, sizeof(*f));
    return 0;
}

/* ---- Lightweight mutexes: state lives in the guest workarea ----
 * workarea: +0 lockLevel, +4 lockThread, +8 attr, +12 numWaitThreads, +16 uid */

#define LW_RECURSIVE 0x200

static uint32_t h_CreateLwMutex(CpuState *s) {
    uint32_t wa = A0, attr = A2; int32_t init = (int32_t)A3;
    if (attr >= 0x400) return ERR_ILLEGAL_ATTR;
    if (init < 0 || (!(attr & LW_RECURSIVE) && init > 1)) return ERR_ILLEGAL_COUNT;
    for (uint32_t i = 0; i < 32; i += 4) MEM_W32(wa + i, 0);
    MEM_W32(wa + 0, (uint32_t)init);
    MEM_W32(wa + 4, init ? sched_current_uid() : 0);
    MEM_W32(wa + 8, attr);
    MEM_W32(wa + 16, sr_alloc_uid());
    return 0;
}

static uint32_t h_DeleteLwMutex(CpuState *s) {
    uint32_t wa = A0;
    if (MEM_R32(wa + 16) == 0xFFFFFFFFu) return LWMUTEX_NOT_FOUND;
    MEM_W32(wa + 0, 0);
    MEM_W32(wa + 4, 0xFFFFFFFFu);
    MEM_W32(wa + 16, 0xFFFFFFFFu);
    sched_wake(wa);
    return 0;
}

/* sceKernelLockLwMutexCB(workarea, count, timeoutPtr): blocks on the workarea address. */
static uint32_t h_LockLwMutex(CpuState *s) {
    uint32_t wa = A0; int32_t count = (int32_t)A1;
    uint32_t attr = MEM_R32(wa + 8);
    if (count <= 0 || (count > 1 && !(attr & LW_RECURSIVE))) return ERR_ILLEGAL_COUNT;
    for (;;) {
        if (MEM_R32(wa + 16) == 0xFFFFFFFFu) return LWMUTEX_NOT_FOUND;
        int32_t level = (int32_t)MEM_R32(wa + 0);
        uint32_t owner = MEM_R32(wa + 4);
        if (level == 0) {
            MEM_W32(wa + 0, (uint32_t)count);
            MEM_W32(wa + 4, sched_current_uid());
            return 0;
        }
        if (owner == sched_current_uid()) {
            if (!(attr & LW_RECURSIVE)) return LWMUTEX_LOCKED;
            if (level + count < 0) return LWMUTEX_OVERFLOW;
            MEM_W32(wa + 0, (uint32_t)(level + count));
            return 0;
        }
        MEM_W32(wa + 12, MEM_R32(wa + 12) + 1);
        sched_block_on(wa);
        MEM_W32(wa + 12, MEM_R32(wa + 12) - 1);
    }
}

static uint32_t h_UnlockLwMutex(CpuState *s) {
    uint32_t wa = A0; int32_t count = (int32_t)A1;
    uint32_t attr = MEM_R32(wa + 8);
    if (MEM_R32(wa + 16) == 0xFFFFFFFFu) return LWMUTEX_NOT_FOUND;
    if (count <= 0 || (count > 1 && !(attr & LW_RECURSIVE))) return ERR_ILLEGAL_COUNT;
    int32_t level = (int32_t)MEM_R32(wa + 0);
    if (level == 0 || MEM_R32(wa + 4) != sched_current_uid()) return LWMUTEX_UNLOCKED;
    if (level < count) return LWMUTEX_UNDERFLOW;
    level -= count;
    MEM_W32(wa + 0, (uint32_t)level);
    if (level == 0) {
        MEM_W32(wa + 4, 0);
        if (MEM_R32(wa + 12)) { sched_wake(wa); sched_preempt(); }
    }
    return 0;
}

/* ---- Threads / callbacks ---- */

/* sceKernelReferThreadStatus(thid, SceKernelThreadInfo *info). For SDK > 2.60 the struct is
 * 108 bytes; a larger size field is rejected with ILLEGAL_SIZE (the game probes with 544 at
 * boot and handles the error, exactly as on hardware and in PPSSPP). */
static uint32_t h_ReferThreadStatus(CpuState *s) {
    uint32_t thid = A0 ? A0 : sched_current_uid(), info = A1;
    uint32_t want = MEM_R32(info);
    if (want > 108) return ERR_ILLEGAL_SIZE;
    SrThreadRunStatus rs;
    if (sched_thread_run_status(thid, &rs) < 0) return 0x80020198;   /* UNKNOWN_THID */
    uint8_t nt[108];
    memset(nt, 0, sizeof(nt));
    uint32_t *w = (uint32_t *)nt;
    w[0] = 108;
    /* name[32] at +4 left empty; attr +36 */
    w[40 / 4] = rs.status;                 /* status */
    w[60 / 4] = rs.currentPriority;        /* initialPriority */
    w[64 / 4] = rs.currentPriority;        /* currentPriority */
    w[68 / 4] = rs.waitType;
    w[72 / 4] = rs.waitId;
    w[76 / 4] = rs.wakeupCount;
    w[84 / 4] = rs.runClocksLow;
    w[88 / 4] = rs.runClocksHigh;
    for (uint32_t i = 0; i < want; i++) MEM_W8(info + i, nt[i]);
    return 0;
}

static uint32_t h_DelayThreadCB(CpuState *s) { sched_delay_current(A0); return 0; }

/* sceKernelCancelSema(semid, newCount, int *numWaitThreads) lives in hle.c's Sync table. */
extern uint32_t sr_sema_cancel(uint32_t uid, int32_t new_count, uint32_t out_waiters);
static uint32_t h_CancelSema(CpuState *s) { return sr_sema_cancel(A0, (int32_t)A1, A2); }

/* sceKernelSetAlarm(clock_us, handler, common) / sceKernelCancelAlarm(uid): see sched.c. */
void sched_set_alarm(uint32_t uid, uint32_t usec, uint32_t handler, uint32_t arg);
int sched_cancel_alarm(uint32_t uid);
static uint32_t h_SetAlarm(CpuState *s) {
    uint32_t uid = sr_alloc_uid();
    sched_set_alarm(uid, A0, A1, A2);
    return uid;
}
static uint32_t h_CancelAlarm(CpuState *s) {
    return sched_cancel_alarm(A0) == 0 ? 0 : 0x8002019fu;   /* UNKNOWN_ALMID */
}

/* ---- Kernel_Library ---- */

static uint32_t h_Memset(CpuState *s) {
    uint32_t dst = A0, n = A2; uint8_t v = (uint8_t)A1;
    for (uint32_t i = 0; i < n; i++) MEM_W8(dst + i, v);
    return dst;
}
static uint32_t h_Memcpy(CpuState *s) {
    uint32_t dst = A0, src = A1, n = A2;
    if (dst > src && dst < src + n) { for (uint32_t i = n; i-- > 0;) MEM_W8(dst + i, MEM_R8(src + i)); }
    else { for (uint32_t i = 0; i < n; i++) MEM_W8(dst + i, MEM_R8(src + i)); }
    return dst;
}

/* ---- Time / power ---- */

static uint64_t rtc_tick_now(void) {
    /* Microseconds since 0001-01-01, the PSP RTC epoch. FILETIME counts 100 ns since 1601. */
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return t / 10 + 50491123200000000ull;
}
static uint32_t h_RtcGetCurrentTick(CpuState *s) {
    uint64_t t = rtc_tick_now();
    if (A0) { MEM_W32(A0, (uint32_t)t); MEM_W32(A0 + 4, (uint32_t)(t >> 32)); }
    return 0;
}
static uint32_t h_RtcGetAccumulativeTime(CpuState *s) {
    uint64_t t = rtc_tick_now();
    s->r[3] = (uint32_t)(t >> 32);           /* u64 return: v0 low, v1 high */
    return (uint32_t)t;
}
static uint32_t h_RtcSetTick(CpuState *s) {
    /* sceRtcSetTick(ScePspDateTime *dt, const u64 *tick): convert tick -> date fields. */
    uint32_t dt = A0, tp = A1;
    uint64_t tick = MEM_R32(tp) | ((uint64_t)MEM_R32(tp + 4) << 32);
    if (tick < 50491123200000000ull) return 0x80010016;
    uint64_t ft = (tick - 50491123200000000ull) * 10;
    FILETIME f; SYSTEMTIME st;
    f.dwLowDateTime = (DWORD)ft; f.dwHighDateTime = (DWORD)(ft >> 32);
    if (!FileTimeToSystemTime(&f, &st)) return 0x80010016;
    MEM_W16(dt + 0, st.wYear);  MEM_W16(dt + 2, st.wMonth); MEM_W16(dt + 4, st.wDay);
    MEM_W16(dt + 6, st.wHour);  MEM_W16(dt + 8, st.wMinute); MEM_W16(dt + 10, st.wSecond);
    MEM_W32(dt + 12, (uint32_t)(tick % 1000000));
    return 0;
}

static uint32_t s_cpu_mhz = 222, s_bus_mhz = 111;
static uint32_t h_PowerSetClock(CpuState *s) { s_cpu_mhz = A1; s_bus_mhz = A2; return 0; }
static uint32_t h_PowerCpu(CpuState *s) { (void)s; return s_cpu_mhz; }
static uint32_t h_PowerBus(CpuState *s) { (void)s; return s_bus_mhz; }
static uint32_t h_PowerPll(CpuState *s) { (void)s; return s_cpu_mhz > s_bus_mhz * 2 ? s_cpu_mhz : s_bus_mhz * 2; }

/* ---- Misc ---- */

static uint32_t h_GeEdramGetSize(CpuState *s) { (void)s; return 0x00200000; }

static uint32_t h_OpenPSID(CpuState *s) {
    /* 16-byte console id; any stable value works. */
    static const uint8_t id[16] = { 0x10, 0x02, 0xA3, 0x44, 0x13, 0xF5, 0x93, 0xB0,
                                    0xCC, 0x6E, 0xD1, 0x32, 0x27, 0x85, 0x0F, 0x9B };
    for (int i = 0; i < 16; i++) MEM_W8(A0 + (uint32_t)i, id[i]);
    return 0;
}

/* ---- Networking: the port is offline. Init/term succeed, everything else reports
 * "not connected" so the game's online features stay disabled. ---- */

static uint32_t h_net_fail(CpuState *s) { (void)s; return 0x80410A05; }   /* generic net error */
static uint32_t h_ApctlGetState(CpuState *s) { if (A0) MEM_W32(A0, 0); return 0; }       /* disconnected */
static uint32_t h_AdhocctlGetState(CpuState *s) { if (A0) MEM_W32(A0, 0); return 0; }
static uint32_t h_handler_id(CpuState *s) { (void)s; return 0; }

/* sceUtilityNetconf*: report the dialog as finished and cancelled. */
static int s_netconf = 0;   /* 0 none, 1 init, 3 finished, 4 shutdown */
static uint32_t h_NetconfInitStart(CpuState *s) {
    (void)s; s_netconf = 3;
    if (A0) MEM_W32(A0 + 0x1C, 1);   /* base.result: cancelled */
    return 0;
}
static uint32_t h_NetconfGetStatus(CpuState *s) { (void)s; int st = s_netconf; if (st == 4) s_netconf = 0; return (uint32_t)st; }
static uint32_t h_NetconfShutdown(CpuState *s) { (void)s; s_netconf = 4; return 0; }

/* ---- UMD / display ---- */

static uint32_t h_WaitVblank(CpuState *s) { (void)s; sched_wait_vblank(); return 0; }

/* ---- sceUtility modules and message dialog ---- */

/* sceUtilityMsgDialog: no UI yet. The message is logged, the dialog reports itself visible for
 * one update, then finishes with "Yes/OK" pressed. Struct offsets (pspUtilityMsgDialogParams):
 * base 0x30 bytes (result at +0x1C), mode +0x34, errorValue +0x38, message[512] +0x3C,
 * options +0x23C, buttonPressed +0x240. */
static int s_msg_status = 0;           /* 0 none, 1 init, 2 visible, 3 finished, 4 shutdown */
static uint32_t s_msg_params = 0;
static uint32_t h_MsgDialogInitStart(CpuState *s) {
    if (s_msg_status != 0) return 0x80110001;     /* SCE_ERROR_UTILITY_INVALID_STATUS */
    s_msg_params = A0;
    uint32_t p = A0, mode = MEM_R32(p + 0x34);
    char msg[513]; int n = 0;
    if (mode == 0) snprintf(msg, sizeof(msg), "error 0x%08x", MEM_R32(p + 0x38));
    else { for (; n < 512; n++) { char c = (char)MEM_R8(p + 0x3C + (uint32_t)n); if (!c) break; msg[n] = c; } msg[n] = 0; }
    fprintf(stderr, "MSGDIALOG (options 0x%x): %s\n", MEM_R32(p + 0x23C), msg);
    s_msg_status = 1;
    return 0;
}
static uint32_t h_MsgDialogGetStatus(CpuState *s) {
    (void)s;
    int st = s_msg_status;
    if (st == 1) s_msg_status = 2;
    else if (st == 2) {
        s_msg_status = 3;
        MEM_W32(s_msg_params + 0x1C, 0);          /* result: OK */
        MEM_W32(s_msg_params + 0x240, 1);         /* buttonPressed: YES/OK */
    } else if (st == 4) s_msg_status = 0;
    return (uint32_t)st;
}
static uint32_t h_MsgDialogShutdown(CpuState *s) { (void)s; if (s_msg_status == 3) s_msg_status = 4; return 0; }
static uint32_t h_MsgDialogAbort(CpuState *s) { (void)s; if (s_msg_status) s_msg_status = 3; return 0; }

/* ---- Directory I/O on the host-backed fs/ store (flattened paths, see hle.c host_path) ---- */

void sr_host_path(const char *guest, char *out, int max);

static void guest_str(uint32_t a, char *out, int max) {
    int i = 0;
    for (; i < max - 1; i++) { char c = (char)MEM_R8(a + (uint32_t)i); if (!c) break; out[i] = c; }
    out[i] = 0;
}

typedef struct { int used; HANDLE h; WIN32_FIND_DATAA fd; int first, dot; char prefix[512]; } DirH;
static DirH s_dirs[8];

static uint32_t h_IoDopen(CpuState *s) {
    char g[256], hp[512];
    guest_str(A0, g, sizeof(g));
    sr_host_path(g, hp, sizeof(hp));
    for (int i = 0; i < 8; i++) {
        if (s_dirs[i].used) continue;
        DirH *d = &s_dirs[i];
        memset(d, 0, sizeof(*d));
        snprintf(d->prefix, sizeof(d->prefix), "%s_", strrchr(hp, '/') ? strrchr(hp, '/') + 1 : hp);
        char pat[600]; snprintf(pat, sizeof(pat), "%s_*", hp);
        d->h = FindFirstFileA(pat, &d->fd);
        d->used = 1; d->first = 1; d->dot = 0;
        if (getenv("SR_IOLOG")) fprintf(stderr, "Dopen(%s) -> %d\n", g, i);
        return 0x100u + (uint32_t)i;
    }
    return 0x80010018;
}

/* SceIoDirent: SceIoStat (0x58) + d_name[256] + d_private + dummy. */
static void write_dirent(uint32_t ent, const char *name, int is_dir, uint64_t size) {
    for (uint32_t i = 0; i < 0x58 + 256; i++) MEM_W8(ent + i, 0);
    MEM_W32(ent + 0, (is_dir ? 0x1000u : 0x2000u) | 0x1FFu);   /* mode */
    MEM_W32(ent + 4, is_dir ? 0x10u : 0x20u);                  /* attr */
    MEM_W32(ent + 8, (uint32_t)size); MEM_W32(ent + 12, (uint32_t)(size >> 32));
    for (int i = 0; name[i] && i < 255; i++) MEM_W8(ent + 0x58 + (uint32_t)i, (uint8_t)name[i]);
}

/* Returns 1 per entry, 0 at the end. Lists ".", "..", then the host files flattened under
 * this directory's prefix (first path component after it). */
static uint32_t h_IoDread(CpuState *s) {
    uint32_t id = A0 - 0x100u;
    if (id >= 8 || !s_dirs[id].used) return 0x80010009;   /* bad fd */
    DirH *d = &s_dirs[id];
    if (d->dot < 2) { write_dirent(A1, d->dot ? ".." : ".", 1, 0); d->dot++; return 1; }
    while (d->h != INVALID_HANDLE_VALUE) {
        if (!d->first && !FindNextFileA(d->h, &d->fd)) { FindClose(d->h); d->h = INVALID_HANDLE_VALUE; break; }
        d->first = 0;
        const char *rest = d->fd.cFileName + strlen(d->prefix);
        char name[256]; int n = 0;
        while (rest[n] && rest[n] != '_' && n < 255) { name[n] = rest[n]; n++; }
        name[n] = 0;
        if (!n) continue;
        int is_dir = rest[n] == '_';
        uint64_t sz = ((uint64_t)d->fd.nFileSizeHigh << 32) | d->fd.nFileSizeLow;
        write_dirent(A1, name, is_dir, is_dir ? 0 : sz);
        return 1;
    }
    return 0;
}
static uint32_t h_IoDclose(CpuState *s) {
    uint32_t id = A0 - 0x100u;
    if (id >= 8 || !s_dirs[id].used) return 0x80010009;
    if (s_dirs[id].h != INVALID_HANDLE_VALUE) FindClose(s_dirs[id].h);
    s_dirs[id].used = 0;
    return 0;
}
static uint32_t h_IoRemove(CpuState *s) {
    char g[256], hp[512];
    guest_str(A0, g, sizeof(g));
    sr_host_path(g, hp, sizeof(hp));
    return remove(hp) == 0 ? 0 : 0x80010002;
}
/* Directories are implicit in the flattened store, so mkdir/rmdir/chdir/chstat just succeed. */
static uint32_t h_IoIoctl(CpuState *s) {
    static int n = 0;
    if (n++ < 8) fprintf(stderr, "HLE: sceIoIoctl(fd=%d, cmd=0x%08x) -> 0 (not modelled)\n", (int)A0, A1);
    return 0;
}

/* ---- Kernel callbacks ----
 * A callback belongs to the thread that created it and only runs while that thread is inside a
 * ...CB call or sceKernelCheckCallback (hle.c sr_syscall delivers). Handler signature:
 * int cb(int notifyCount, int notifyArg, void *common); a non-zero return deletes it. */
uint32_t sr_call_guest(CpuState *s, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, int on_thread_stack);
typedef struct { int used; uint32_t uid, func, common, thread; int count; uint32_t arg; } KCallback;
static KCallback s_kcb[32];
static int s_kcb_pending = 0;

static KCallback *kcb_find(uint32_t uid) {
    for (int i = 0; i < 32; i++) if (s_kcb[i].used && s_kcb[i].uid == uid) return &s_kcb[i];
    return NULL;
}
static void kcb_notify(uint32_t uid, uint32_t arg) {
    KCallback *c = kcb_find(uid);
    if (!c) return;
    c->count++; c->arg = arg;
    s_kcb_pending = 1;
}
int sr_callbacks_pending(void) { return s_kcb_pending; }
void sr_run_callbacks(CpuState *s) {
    uint32_t me = sched_current_uid();
    int left = 0;
    for (int i = 0; i < 32; i++) {
        KCallback *c = &s_kcb[i];
        if (!c->used || !c->count) continue;
        if (c->thread != me) { left = 1; continue; }
        int count = c->count; uint32_t arg = c->arg;
        c->count = 0;
        if (getenv("SR_CBLOG")) fprintf(stderr, "callback uid=0x%x fn=0x%08x count=%d arg=0x%x on thread 0x%x\n",
                                        c->uid, c->func, count, arg, me);
        uint32_t r = sr_call_guest(s, c->func, (uint32_t)count, arg, c->common, 1);
        if (r) c->used = 0;
    }
    s_kcb_pending = left;
}
/* sceKernelCreateCallback(name, func, common) */
static uint32_t h_CreateCallback(CpuState *s) {
    for (int i = 0; i < 32; i++) {
        if (s_kcb[i].used) continue;
        memset(&s_kcb[i], 0, sizeof(s_kcb[i]));
        s_kcb[i].used = 1; s_kcb[i].uid = sr_alloc_uid();
        s_kcb[i].func = A1; s_kcb[i].common = A2; s_kcb[i].thread = sched_current_uid();
        return s_kcb[i].uid;
    }
    return 0x80020190;
}
static uint32_t h_DeleteCallback(CpuState *s) {
    KCallback *c = kcb_find(A0);
    if (!c) return 0x800201a1;          /* UNKNOWN_CBID */
    c->used = 0;
    return 0;
}
static uint32_t h_NotifyCallback(CpuState *s) { kcb_notify(A0, A1); return 0; }

/* UMD: the drive-status callback gets PRESENT|INITED|READY once the disc is activated. The game
 * waits for READY (0x20) before streaming movies from the disc. */
#define UMD_STAT_READY_ALL 0x32u
static uint32_t s_umd_cb = 0;
static int s_umd_active = 0;
static uint32_t h_UmdRegisterUMDCallBack(CpuState *s) {
    if (!kcb_find(A0)) return 0x80010016;
    s_umd_cb = A0;
    if (s_umd_active) kcb_notify(s_umd_cb, UMD_STAT_READY_ALL);
    return 0;
}
static uint32_t h_UmdUnRegisterUMDCallBack(CpuState *s) { if (A0 == s_umd_cb) s_umd_cb = 0; return 0; }
static uint32_t h_UmdActivate(CpuState *s) {
    (void)s;
    s_umd_active = 1;
    if (s_umd_cb) kcb_notify(s_umd_cb, UMD_STAT_READY_ALL);
    return 0;
}

void sr_hle_init_ext(void) {
    sr_hle_register(0xe81caf8f, "sceKernelCreateCallback", h_CreateCallback);
    sr_hle_register(0xc11ba8c4, "sceKernelNotifyCallback", h_NotifyCallback);
    sr_hle_register(0xaee7404d, "sceUmdRegisterUMDCallBack", h_UmdRegisterUMDCallBack);
    sr_hle_register(0xc6183d47, "sceUmdActivate", h_UmdActivate);
    sr_hle_register(0x4a9e5e29, "sceUmdWaitDriveStatCB", h_zero);
    sr_hle_register(0xbd2bde07, "sceUmdUnRegisterUMDCallBack", h_UmdUnRegisterUMDCallBack);
    sr_hle_register(0x36cdfade, "sceDisplayWaitVblank", h_WaitVblank);
    sr_hle_register(0x46f186c3, "sceDisplayWaitVblankStartCB", h_WaitVblank);

    sr_hle_register(0x2a2b3de0, "sceUtilityLoadModule", h_zero);
    sr_hle_register(0xe49bfe92, "sceUtilityUnloadModule", h_zero);
    sr_hle_register(0x2ad8e239, "sceUtilityMsgDialogInitStart", h_MsgDialogInitStart);
    sr_hle_register(0x9a1c91d7, "sceUtilityMsgDialogGetStatus", h_MsgDialogGetStatus);
    sr_hle_register(0x95fc253b, "sceUtilityMsgDialogUpdate", h_zero);
    sr_hle_register(0x67af3428, "sceUtilityMsgDialogShutdownStart", h_MsgDialogShutdown);
    sr_hle_register(0x4928bd96, "sceUtilityMsgDialogAbort", h_MsgDialogAbort);

    sr_hle_register(0xb29ddf9c, "sceIoDopen", h_IoDopen);
    sr_hle_register(0xe3eb004c, "sceIoDread", h_IoDread);
    sr_hle_register(0xeb092469, "sceIoDclose", h_IoDclose);
    sr_hle_register(0xf27a9c51, "sceIoRemove", h_IoRemove);
    sr_hle_register(0x06a70004, "sceIoMkdir", h_zero);
    sr_hle_register(0x1117c65f, "sceIoRmdir", h_zero);
    sr_hle_register(0x55f4717d, "sceIoChdir", h_zero);
    sr_hle_register(0xb8a740f4, "sceIoChstat", h_zero);
    sr_hle_register(0x63632449, "sceIoIoctl", h_IoIoctl);

    sr_hle_register(0x1b4217bc, "sceKernelSetCompiledSdkVersion603_605", h_SetCompiledSdkVersionAny);

    sr_hle_register(0xc07bb470, "sceKernelCreateFpl", h_CreateFpl);
    sr_hle_register(0xd979e9bf, "sceKernelAllocateFpl", h_AllocateFpl);
    sr_hle_register(0xed1410e0, "sceKernelDeleteFpl", h_DeleteFpl);

    sr_hle_register(0x19cff145, "sceKernelCreateLwMutex", h_CreateLwMutex);
    sr_hle_register(0x60107536, "sceKernelDeleteLwMutex", h_DeleteLwMutex);
    sr_hle_register(0x1fc64e09, "sceKernelLockLwMutexCB", h_LockLwMutex);
    sr_hle_register(0x15b6446b, "sceKernelUnlockLwMutex", h_UnlockLwMutex);

    sr_hle_register(0x17c1684e, "sceKernelReferThreadStatus", h_ReferThreadStatus);
    sr_hle_register(0x68da9e36, "sceKernelDelayThreadCB", h_DelayThreadCB);
    sr_hle_register(0x8ffdf9a2, "sceKernelCancelSema", h_CancelSema);
    sr_hle_register(0xedba5844, "sceKernelDeleteCallback", h_DeleteCallback);
    sr_hle_register(0x349d6d6c, "sceKernelCheckCallback", h_zero);
    sr_hle_register(0x6652b8ca, "sceKernelSetAlarm", h_SetAlarm);
    sr_hle_register(0x7e65b999, "sceKernelCancelAlarm", h_CancelAlarm);

    sr_hle_register(0xa089eca4, "sceKernelMemset", h_Memset);
    sr_hle_register(0x1839852a, "sceKernelMemcpy", h_Memcpy);

    sr_hle_register(0xb435dec5, "sceKernelDcacheWritebackInvalidateAll", h_zero);
    sr_hle_register(0xbfa98062, "sceKernelDcacheInvalidateRange", h_zero);
    sr_hle_register(0x34b9fa9e, "sceKernelDcacheWritebackInvalidateRange", h_zero);

    sr_hle_register(0x3f7ad767, "sceRtcGetCurrentTick", h_RtcGetCurrentTick);
    sr_hle_register(0x011f03c1, "sceRtcGetAccumulativeTime", h_RtcGetAccumulativeTime);
    sr_hle_register(0x7ed29e40, "sceRtcSetTick", h_RtcSetTick);

    sr_hle_register(0x469989ad, "scePowerSetClockFrequency630", h_PowerSetClock);
    sr_hle_register(0xfdb5bfe9, "scePowerGetCpuClockFrequencyInt", h_PowerCpu);
    sr_hle_register(0xbd681969, "scePowerGetBusClockFrequencyInt", h_PowerBus);
    sr_hle_register(0x34f9c463, "scePowerGetPllClockFrequencyInt", h_PowerPll);

    sr_hle_register(0x1f6752ad, "sceGeEdramGetSize", h_GeEdramGetSize);
    sr_hle_register(0xc69bebce, "sceOpenPSIDGetOpenPSID", h_OpenPSID);

    /* networking: offline */
    sr_hle_register(0x39af39a6, "sceNetInit", h_zero);
    sr_hle_register(0x281928a9, "sceNetTerm", h_zero);
    sr_hle_register(0x17943399, "sceNetInetInit", h_zero);
    sr_hle_register(0xa9ed66b9, "sceNetInetTerm", h_zero);
    sr_hle_register(0xf3370e61, "sceNetResolverInit", h_zero);
    sr_hle_register(0x6138194a, "sceNetResolverTerm", h_zero);
    sr_hle_register(0xe2f91f9b, "sceNetApctlInit", h_zero);
    sr_hle_register(0xb3edd0ec, "sceNetApctlTerm", h_zero);
    sr_hle_register(0x5deac81b, "sceNetApctlGetState", h_ApctlGetState);
    sr_hle_register(0x8abadd51, "sceNetApctlAddHandler", h_handler_id);
    sr_hle_register(0x5963991b, "sceNetApctlDelHandler", h_zero);
    sr_hle_register(0x24fe91a1, "sceNetApctlDisconnect", h_zero);
    sr_hle_register(0x20b317a0, "sceNetAdhocctlAddHandler", h_handler_id);
    sr_hle_register(0x6402490b, "sceNetAdhocctlDelHandler", h_zero);
    sr_hle_register(0x34401d65, "sceNetAdhocctlDisconnect", h_zero);
    sr_hle_register(0x75ecd386, "sceNetAdhocctlGetState", h_AdhocctlGetState);
    sr_hle_register(0xab1abe07, "sceHttpInit", h_zero);
    sr_hle_register(0xd1c8945e, "sceHttpEnd", h_zero);
    sr_hle_register(0xf49934f6, "sceHttpSetMallocFunction", h_zero);
    {
        static const struct { uint32_t nid; const char *name; } http_fail[] = {
            {0x0282a3bd, "sceHttpGetContentLength"}, {0x03d9526f, "sceHttpSetResolveRetry"},
            {0x1f0fc3e3, "sceHttpSetRecvTimeOut"},   {0x3eaba285, "sceHttpAddExtraHeader"},
            {0x47940436, "sceHttpSetResolveTimeOut"},{0x4cc7d78f, "sceHttpGetStatusCode"},
            {0x5152773b, "sceHttpDeleteConnection"}, {0x8acd1f73, "sceHttpSetConnectTimeOut"},
            {0x9988172d, "sceHttpSetSendTimeOut"},   {0x9b1f1f36, "sceHttpCreateTemplate"},
            {0xa5512e01, "sceHttpDeleteRequest"},    {0xb509b09e, "sceHttpCreateRequestWithURL"},
            {0xbb70706f, "sceHttpSendRequest"},      {0xcdf8ecb9, "sceHttpCreateConnectionWithURL"},
            {0xdb266ccf, "sceHttpGetAllHeader"},     {0xedeeb999, "sceHttpReadData"},
            {0xfcf8c055, "sceHttpDeleteTemplate"},   {0xad7bfdef, "sceParseHttpResponseHeader"},
        };
        for (size_t i = 0; i < sizeof(http_fail) / sizeof(http_fail[0]); i++)
            sr_hle_register(http_fail[i].nid, http_fail[i].name, h_net_fail);
    }
    sr_hle_register(0x4db1e739, "sceUtilityNetconfInitStart", h_NetconfInitStart);
    sr_hle_register(0x6332aa39, "sceUtilityNetconfGetStatus", h_NetconfGetStatus);
    sr_hle_register(0x91e70e35, "sceUtilityNetconfUpdate", h_zero);
    sr_hle_register(0xf88155f6, "sceUtilityNetconfShutdownStart", h_NetconfShutdown);
}
