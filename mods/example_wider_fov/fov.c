/* Example code mod: 15% wider field of view for the cameras the game sets up through
 * 0x08B435B8 (fov argument in $f12). Returning 0 lets the original function run with the
 * changed argument; returning 1 would skip it entirely (a replacement). */
#include "recomp.h"

int mod_hook(CpuState *s, uint32_t addr) {
    if (addr == MODHOOK_CAM_SETUP) s->f[12] *= 1.15f;
    return 0;
}
