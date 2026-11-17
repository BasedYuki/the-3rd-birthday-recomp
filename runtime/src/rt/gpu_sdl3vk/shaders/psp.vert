#version 450
/* PSP GE vertex stage, Phase 1. Vertices arrive ALREADY transformed and projected by the
 * (reference-correct) CPU T&L in ge.c: x/y are screen pixels snapped to the PSP's 28.4
 * subpixel grid, z is the 16-bit PSP depth (0..65535), rw is 1/clipw (1.0 in through
 * mode). All varyings are noperspective: the software rasterizer interpolates u*rw, v*rw
 * and rw AFFINELY in screen space and divides per pixel, which this reproduces exactly.
 * The render target is the full 512x272 PSP framebuffer stride. */
layout(location = 0) in vec4 in_pos;    /* x px, y px, z 0..65535, rw */
layout(location = 1) in vec2 in_uv;     /* u*rw, v*rw texel coords (raw u,v in through mode) */
layout(location = 2) in float in_fog;   /* per-vertex fog factor, 1 = no fog */
layout(location = 3) in vec4 in_color;  /* UNORM8 RGBA */
layout(location = 4) in vec4 in_prev;   /* skinned: clip position in the previous frame (w 0: none) */

/* The same geometry is often drawn twice with depth test EQUAL (a base pass, then a lit or
 * decal pass over it). Every pipeline must then compute bit-identical positions, which only
 * `invariant` guarantees: AMD's compiler otherwise optimises the vertex math differently per
 * pipeline, and in the frame-interpolation path (more math) the second pass failed the
 * depth test in patches: barricades flashing at 60 fps (issue #11). */
invariant gl_Position;

layout(location = 0) noperspective out vec2  v_uv;
layout(location = 1) noperspective out float v_rw;
layout(location = 2) noperspective out float v_fog;
layout(location = 3) noperspective out vec4  v_color;

/* Frame interpolation (ge_gpu.c fi_replay): R moves this draw's clip-space positions from the
 * frame being replayed to the in-between time. vs = viewport scale (x, y, z) and an enable
 * flag, vc = viewport centre minus the screen offset. Live rendering reads entry 0 (all zero).
 * vs.w = 2: skinned draw, each vertex goes to mix(previous clip position, this one, vc.w).
 * One entry per draw in an instance-rate buffer (first instance = draw), not push constants:
 * those are only 128 bytes on some GPUs (an AMD iGPU in issue #11), the fragment stage uses 80. */
layout(location = 5) in vec4 in_r0;
layout(location = 6) in vec4 in_r1;
layout(location = 7) in vec4 in_r2;
layout(location = 8) in vec4 in_r3;
layout(location = 9) in vec4 in_vs;
layout(location = 10) in vec4 in_vc;

void main() {
    vec4 p = in_pos;
    vec2 uv = in_uv;
    const mat4 R = mat4(in_r0, in_r1, in_r2, in_r3);
    if (in_vs.w != 0.0 && p.w > 0.0) {
        /* screen -> clip (undo the divide and the viewport), move, project again */
        float w = 1.0 / p.w;
        vec3 ndc = (p.xyz - in_vc.xyz) / in_vs.xyz;
        vec4 cc = vec4(ndc * w, w);
        vec4 c = (in_vs.w > 1.5 && in_prev.w != 0.0) ? mix(in_prev, cc, in_vc.w) : R * cc;
        if (c.w > 1e-3) {
            float rw = 1.0 / c.w;
            uv *= rw / p.w;                  /* the varying carries u*rw */
            p.xyz = c.xyz * rw * in_vs.xyz + in_vc.xyz;
            p.w = rw;
        }
    }
    gl_Position = vec4(p.x * (1.0 / 256.0) - 1.0,
                       p.y * (1.0 / 136.0) - 1.0,
                       clamp(p.z, 0.0, 65535.0) * (1.0 / 65535.0),
                       1.0);
    v_uv    = uv;
    v_rw    = p.w;
    v_fog   = in_fog;
    v_color = in_color;
}
