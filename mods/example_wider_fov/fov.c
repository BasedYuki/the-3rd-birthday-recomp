/* Example code mod: changes the fov argument ($f12) of the camera setup function 0x08B435B8
 * by 15% and logs it (run.err: "example_wider_fov: camera fov 0.611 -> 0.702"). It shows the
 * mechanism; the gameplay camera sets its field of view again every frame elsewhere, so the
 * picture changes only for cameras that keep the value from this setup. Returning 0 lets the original function run with the
 * changed argument; returning 1 would skip it entirely (a replacement). */
#include "recomp.h"
#include <stdio.h>

int mod_hook(CpuState *s, uint32_t addr) {
    if (addr == MODHOOK_CAM_SETUP) {
        static int n = 0;
        if (n++ < 4) fprintf(stderr, "example_wider_fov: camera fov %.3f -> %.3f\n", s->f[12], s->f[12] * 1.15f);
        s->f[12] *= 1.15f;
    }
    return 0;
}
