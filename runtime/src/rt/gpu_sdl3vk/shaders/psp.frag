#version 450
/* PSP GE fragment stage uber-shader, Phase 1. Mirrors ge.c shade() + zrange_ok():
 * texfunc 0-7 (modulate/decal/blend/replace/add) with the RGBA and color-double bits,
 * programmable alpha test, fog mix, and the minz/maxz depth-range DISCARD (transform
 * mode only -- it is a kill test on the PSP, not a clamp). Clear-mode draws bypass
 * texturing/alpha-test/fog like put_px_rgba does. Blending and write masks are VK
 * fixed-function state chosen by ge_gpu.c. Colors are computed in 0..255 space to track
 * the integer formulas in shade(). */
layout(location = 0) noperspective in vec2  v_uv;
layout(location = 1) noperspective in float v_rw;
layout(location = 2) noperspective in float v_fog;
layout(location = 3) noperspective in vec4  v_color;

layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) uniform sampler2D u_tex;

/* flags bits (pc.cfg.x >> 8): 1 textured, 2 texfunc-RGBA, 4 color-double, 8 fog,
 * 16 persp (apply minz/maxz), 32 clear mode, 64 nearest-filter (+0.5 texel shift),
 * 128 premultiply rgb by 2*alpha (PSP src blend factor DOUBLE_SRC_ALPHA with VK factor
 * ONE), 256 premultiply rgb by (1 - 2*alpha) (ONE_MINUS_DOUBLE_SRC_ALPHA). */
layout(push_constant) uniform PC {
    ivec4 cfg;      /* x = texfunc | flags<<8, y = alpha test (func|ref<<8|mask<<16),
                       z = minz, w = maxz */
    vec4  texenv;   /* rgb = GE_TEXENVCOLOR / 255 */
    vec4  fogcol;   /* rgb = GE_FOGCOLOR / 255 */
    vec4  texsize;  /* xy = tex dimensions, zw = texel offset (render-target sub-rect) */
    ivec4 depal;    /* flag 512: x = target fmt | clut fmt<<4 | linear<<8,
                       y = clut shift | mask<<8 | start<<16, z = palette row */
} pc;

/* Palettes for depal draws, one decoded palette per row (entry i at x = i). */
layout(set = 1, binding = 0) uniform sampler2D u_clut;

/* One texel of a render target read through the palette: rebuild the raw 16/32-bit value
 * the PSP would hold in VRAM (8-bit channels truncated to the target format, like the
 * PSP's own framebuffer writes), then index the palette exactly like ge.c clut_lookup. */
vec4 depal_texel(ivec2 p) {
    p = clamp(p, ivec2(0), textureSize(u_tex, 0) - 1);
    uvec4 c = uvec4(texelFetch(u_tex, p, 0) * 255.0 + 0.5);
    int sf = pc.depal.x & 3;
    uint raw;
    if (sf == 3)      raw = c.r | (c.g << 8) | (c.b << 16) | (c.a << 24);
    else if (sf == 2) raw = (c.r >> 4) | ((c.g >> 4) << 4) | ((c.b >> 4) << 8) | ((c.a >> 4) << 12);
    else if (sf == 1) raw = (c.r >> 3) | ((c.g >> 3) << 5) | ((c.b >> 3) << 10) | ((c.a >> 7) << 15);
    else              raw = (c.r >> 3) | ((c.g >> 2) << 5) | ((c.b >> 3) << 11);
    uint shift = uint(pc.depal.y) & 31u, mask = (uint(pc.depal.y) >> 8) & 255u;
    uint start = (uint(pc.depal.y) >> 16) & 0x1FFu;
    bool c32 = ((pc.depal.x >> 4) & 3) == 3;
    uint idx = ((raw >> shift) & mask) | (start & (c32 ? 0xFFu : 0x1FFu));
    idx &= c32 ? 511u : 1023u;
    return texelFetch(u_clut, ivec2(int(idx), pc.depal.z), 0);
}

/* uv in texel units. Linear filtering happens after the palette lookup, as on the PSP. */
vec4 depal_sample(vec2 uv) {
    if (((pc.depal.x >> 8) & 1) == 0) return depal_texel(ivec2(floor(uv)));
    vec2 q = uv - 0.5;
    ivec2 i = ivec2(floor(q));
    vec2 f = q - floor(q);
    vec4 a = mix(depal_texel(i), depal_texel(i + ivec2(1, 0)), f.x);
    vec4 b = mix(depal_texel(i + ivec2(0, 1)), depal_texel(i + ivec2(1, 1)), f.x);
    return mix(a, b, f.y);
}

void main() {
    int flags = pc.cfg.x >> 8;

    /* PSP depth-range test: discard transform-mode pixels outside [minz,maxz]. */
    if ((flags & 16) != 0) {
        int z16 = int(gl_FragCoord.z * 65535.0 + 0.5);
        if (z16 < pc.cfg.z || z16 > pc.cfg.w) discard;
    }

    vec4 v = v_color * 255.0;
    vec4 col = v;

    if ((flags & 32) == 0) {
        if ((flags & 1) != 0) {
            /* Recover texel coords (u*rw interpolated affinely, divided per pixel; in
             * through mode rw==1). UVs are evaluated at pixel centres like the PSP, so the
             * sampler's own rules match it: nearest takes floor(u), linear blends around
             * u - 0.5. (A +0.5 shift here used to push every nearest-filtered triangle one
             * texel right and down: a garbage column at x=479 and a black row at y=271.) */
            float rw = max(abs(v_rw), 1e-20);
            vec2 uv = v_uv / rw;
            uv += pc.texsize.zw;
            vec4 t = ((flags & 512) != 0 ? depal_sample(uv) : texture(u_tex, uv / pc.texsize.xy)) * 255.0;
            int  tf    = pc.cfg.x & 7;
            bool rgba  = (flags & 2) != 0;
            float dscl = ((flags & 4) != 0) ? 2.0 : 1.0;
            vec3 rgb; float al;
            if (tf == 1) {                       /* decal */
                if (rgba) rgb = mix(v.rgb, t.rgb, t.a * (1.0 / 255.0)) * dscl;
                else      rgb = t.rgb * dscl;
                al = v.a;
            } else if (tf == 2) {                /* blend against TEXENVCOLOR */
                rgb = mix(v.rgb, pc.texenv.rgb * 255.0, t.rgb * (1.0 / 255.0)) * dscl;
                al  = rgba ? v.a * t.a * (1.0 / 255.0) : v.a;
            } else if (tf == 3) {                /* replace */
                rgb = t.rgb * dscl;
                al  = rgba ? t.a : v.a;
            } else if (tf >= 4) {                /* add */
                rgb = (v.rgb + t.rgb) * dscl;
                al  = rgba ? v.a * t.a * (1.0 / 255.0) : v.a;
            } else {                             /* modulate */
                rgb = v.rgb * t.rgb * (1.0 / 255.0) * dscl;
                al  = rgba ? v.a * t.a * (1.0 / 255.0) : v.a;
            }
            col = vec4(min(rgb, vec3(255.0)), min(al, 255.0));
        }

        /* Alpha test (func order matches ge.c alpha_test). */
        int at = pc.cfg.y, afunc = at & 7;
        if (afunc != 1) {
            int amask = (at >> 16) & 0xFF;
            int aref  = ((at >> 8) & 0xFF) & amask;
            int av    = int(col.a + 0.5) & amask;
            bool pass;
            if      (afunc == 0) pass = false;
            else if (afunc == 2) pass = (av == aref);
            else if (afunc == 3) pass = (av != aref);
            else if (afunc == 4) pass = (av <  aref);
            else if (afunc == 5) pass = (av <= aref);
            else if (afunc == 6) pass = (av >  aref);
            else                 pass = (av >= aref);
            if (!pass) discard;
        }

        if ((flags & 8) != 0) {
            float f = clamp(v_fog, 0.0, 1.0);   /* 1 = no fog (PPSSPP convention) */
            col.rgb = mix(pc.fogcol.rgb * 255.0, col.rgb, f);
        }

        /* doubled-src-alpha blend factors folded into the source color */
        if ((flags & 128) != 0) col.rgb *= clamp(col.a * 2.0, 0.0, 255.0) * (1.0 / 255.0);
        if ((flags & 256) != 0) col.rgb *= clamp(255.0 - col.a * 2.0, 0.0, 255.0) * (1.0 / 255.0);
    }

    o_color = col * (1.0 / 255.0);
}
