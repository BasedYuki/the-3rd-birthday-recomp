/* Linked into game.dll together with the recompiled game code (compiled with -DSR_GAME_DLL).
 * The runtime calls sr_game_attach with its SrApi table first; the generated code then reaches
 * the runtime's data through the macros in recomp.h and its functions through the forwarders
 * below. game.dll contains no runtime code, the runtime contains no game code. */
#include "recomp.h"

struct SrApi sr_api;

__declspec(dllexport) int sr_game_attach(const struct SrApi *api, int version) {
    if (version != SR_API_VERSION) return 0;
    sr_api = *api;
    return 1;
}

void     sr_oor(uint32_t a, uint32_t v, int store)  { sr_api.oor(a, v, store); }
uint32_t sr_lwl(uint32_t rtv, uint32_t addr)         { return sr_api.lwl(rtv, addr); }
uint32_t sr_lwr(uint32_t rtv, uint32_t addr)         { return sr_api.lwr(rtv, addr); }
void     sr_swl(uint32_t addr, uint32_t rtv)         { sr_api.swl(addr, rtv); }
void     sr_swr(uint32_t addr, uint32_t rtv)         { sr_api.swr(addr, rtv); }
void sr_vread(float *r, const CpuState *s, const uint8_t *idx, int n, uint32_t prefix) { sr_api.vread(r, s, idx, n, prefix); }
void sr_vwrite(CpuState *s, const uint8_t *idx, float *d, int n, uint32_t dprefix)    { sr_api.vwrite(s, idx, d, n, dprefix); }
float sr_vfpu_rcp(float x)   { return sr_api.vfpu_rcp(x); }
float sr_vfpu_rsqrt(float x) { return sr_api.vfpu_rsqrt(x); }
float sr_vfpu_sqrt(float x)  { return sr_api.vfpu_sqrt(x); }
float sr_vfpu_sin(float x)   { return sr_api.vfpu_sin(x); }
float sr_vfpu_cos(float x)   { return sr_api.vfpu_cos(x); }
float sr_vfpu_exp2(float x)  { return sr_api.vfpu_exp2(x); }
void  sr_vfpu_ext(CpuState *s, uint32_t w)                    { sr_api.vfpu_ext(s, w); }
void  sr_register(uint32_t addr, RecompFn fn)                 { sr_api.reg(addr, fn); }
void  dispatch(CpuState *s, uint32_t target)                  { sr_api.dispatch(s, target); }
void  sr_begin_dbg(const CpuState *s, uint32_t pc, uint32_t op) { sr_api.begin_dbg(s, pc, op); }
void  sr_end_dbg(const CpuState *s, uint32_t a, int size)     { sr_api.end_dbg(s, a, size); }
int   sr_func_hook(CpuState *s, uint32_t addr)                { return sr_api.func_hook(s, addr); }
void  sr_syscall(CpuState *s, uint32_t nid)                   { sr_api.syscall(s, nid); }
void  sr_yield(CpuState *s)                                   { sr_api.yield(s); }
void  sr_unimplemented(uint32_t pc, const char *reason)       { sr_api.unimplemented(pc, reason); }
void  gui_toast(const char *msg)                              { sr_api.toast(msg); }

/* code mods (mods_gen.c in the same DLL) define this; the weak default is for none */
__attribute__((weak)) int sr_mods_hook(CpuState *s, uint32_t addr) { (void)s; (void)addr; return 0; }
__declspec(dllexport) int sr_game_mods_hook(CpuState *s, uint32_t addr) { return sr_mods_hook(s, addr); }

uint8_t sr_patch_on[64];   /* cheats: switched by the runtime through the pointer below */
__declspec(dllexport) uint8_t *sr_game_patch_flags(void) { return sr_patch_on; }

void sr_register_all(void);
__declspec(dllexport) void sr_game_register_all(void) { sr_register_all(); }
