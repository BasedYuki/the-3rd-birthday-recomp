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

layout(location = 0) noperspective out vec2  v_uv;
layout(location = 1) noperspective out float v_rw;
layout(location = 2) noperspective out float v_fog;
layout(location = 3) noperspective out vec4  v_color;

/* Frame interpolation (ge_gpu.c fi_replay): R moves this draw's clip-space positions from the
 * frame being replayed to the in-between time. vs = viewport scale (x, y, z) and an enable
 * flag, vc = viewport centre minus the screen offset. Live rendering pushes vs.w = 0. */
layout(push_constant) uniform PCV {
    layout(offset = 80) mat4 R;
    vec4 vs;
    vec4 vc;
} pcv;

void main() {
    vec4 p = in_pos;
    vec2 uv = in_uv;
    if (pcv.vs.w != 0.0 && p.w > 0.0) {
        /* screen -> clip (undo the divide and the viewport), move, project again */
        float w = 1.0 / p.w;
        vec3 ndc = (p.xyz - pcv.vc.xyz) / pcv.vs.xyz;
        vec4 c = pcv.R * vec4(ndc * w, w);
        if (c.w > 1e-3) {
            float rw = 1.0 / c.w;
            uv *= rw / p.w;                  /* the varying carries u*rw */
            p.xyz = c.xyz * rw * pcv.vs.xyz + pcv.vc.xyz;
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
