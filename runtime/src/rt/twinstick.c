/* Twin-stick camera: the PC right stick orbits the gameplay camera.
 *
 * The game has several camera objects with a common layout (from a base pointer B: eye at
 * B+0x10, target at B+0x20, up at B+0x30, then pitch/yaw/distance and fov). Every frame the
 * camera controller writes a fresh eye into them from its own state, so writing angles directly
 * is undone within the frame. Instead the right stick accumulates an orbit offset here, and two
 * hooks in the game's own camera code (versions/<game>.toml [hooks]) rotate a freshly written
 * eye around its target by that offset:
 *   - camera_derive (pitch/yaw/distance from eye and target) for the logic camera behind
 *     camera_ptr, whose derived angles the game uses;
 *   - camera_copy (dst = a0, src = a1), which fills the rendered view camera (viewcam_ptr) from
 *     another camera every frame: its source is rotated just before the copy.
 * The game's own camera logic, lock-on and aim run unchanged underneath (PLAN.md: free orbit).
 * Camera cuts (a new view-camera source, or a target/eye jump) reset the offset; the game's
 * aim camera is one of them. L aims along Aya's facing, so pressing it with the camera turned
 * first turns her to the camera's direction through the game's own movement input.
 *
 * SR_TWINSTICK=0 disables it; SR_TWINSTICK_SPEED scales the turn rate (default 1).
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"
#include "game_addrs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

float g_script_rx = 0.0f, g_script_ry = 0.0f;   /* SR_PADSCRIPT right-stick override */

static float rdf(uint32_t a) { uint32_t w = MEM_R32(a); float f; memcpy(&f, &w, 4); return f; }
static void wrf(uint32_t a, float f) { uint32_t w; memcpy(&w, &f, 4); MEM_W32(a, w); }

/* Radians per controller read at full deflection; the game reads the pad at 30 Hz in
 * gameplay, so 0.07 is ~2 rad/s (about 120 degrees a second). */
#define YAW_RATE   0.07f
#define PITCH_RATE 0.04f
/* Limits on the final elevation of the eye above the target (radians). */
#define ELEV_MIN (-0.35f)   /* a little below the target */
#define ELEV_MAX (1.2f)     /* high overhead */

static int s_on = -1;
static float s_speed = 1.0f;
static float s_yaw_off = 0.0f, s_pitch_off = 0.0f;   /* accumulated right-stick orbit */
static unsigned s_ticks = 0, s_hook_tick = 0;        /* pad reads, and the last one with a camera hook */

/* The eye we last wrote into each camera object, so a second pass in the same frame (the
 * game calls these routines more than once) doesn't rotate it again. */
#define MAX_CAMS 8
static struct { uint32_t base; float eye[3]; } s_last[MAX_CAMS];

static void init_once(void) {
    if (s_on >= 0) return;
    const char *e = getenv("SR_TWINSTICK");
    s_on = !(e && e[0] == '0');
    const char *sp = getenv("SR_TWINSTICK_SPEED");
    if (sp) s_speed = (float)atof(sp);
}

/* Once per controller read: integrate the right stick into the orbit offset. */
void sr_twinstick_tick(void) {
    init_once();
    if (!s_on) return;
    /* Only while the gameplay camera is live (its hooks ran within the last few frames):
     * a stick push during a movie or menu must not bank an offset for later. */
    if (++s_ticks - s_hook_tick > 4) return;
    float rx = 0.0f, ry = 0.0f;
    if (gui_on()) gui_rstick(&rx, &ry);
    if (g_script_rx != 0.0f || g_script_ry != 0.0f) { rx = g_script_rx; ry = g_script_ry; }
    if (fabsf(rx) >= 0.01f) {
        s_yaw_off -= rx * YAW_RATE * s_speed;
        const float pi = 3.14159265f;
        while (s_yaw_off > pi) s_yaw_off -= 2.0f * pi;
        while (s_yaw_off < -pi) s_yaw_off += 2.0f * pi;
    }
    if (fabsf(ry) >= 0.01f) {
        s_pitch_off += ry * PITCH_RATE * s_speed;          /* stick up = look up (eye lower) */
        if (s_pitch_off > ELEV_MAX - ELEV_MIN) s_pitch_off = ELEV_MAX - ELEV_MIN;
        if (s_pitch_off < ELEV_MIN - ELEV_MAX) s_pitch_off = ELEV_MIN - ELEV_MAX;
    }
}

/* Aim along the camera: L aims wherever Aya faces, and turning the camera doesn't turn her.
 * When L is pressed with the camera turned (and the left stick idle), hold L back for a few
 * samples and feed "left stick forward" instead: the game's own camera-relative movement turns
 * Aya to face where the camera looks, then L goes through and she aims that way. Runs on every
 * pad sample (60 Hz), before the game sees it. SR_TWINSTICK_AIMTURN=0 disables it. */
#define AIM_TURN_SAMPLES 10
void sr_twinstick_filter(uint32_t *btn, uint8_t *lx, uint8_t *ly) {
    static int on = -1, phase = 0, prev_l = 0;
    init_once();
    if (on < 0) { const char *e = getenv("SR_TWINSTICK_AIMTURN"); on = s_on && !(e && e[0] == '0'); }
    if (!on) return;
    const int l = (*btn & 0x0100u) != 0;
    const int stick_idle = abs((int)*lx - 128) < 40 && abs((int)*ly - 128) < 40;
    if (l && !prev_l && stick_idle && fabsf(s_yaw_off) > 0.2f && s_ticks - s_hook_tick <= 4) {
        phase = AIM_TURN_SAMPLES;
        fprintf(stderr, "twinstick: aim along the camera (turning Aya %.2f rad first)\n", s_yaw_off);
    }
    prev_l = l;
    if (!l) phase = 0;
    if (phase > 0) {
        phase--;
        *btn &= ~0x0100u;          /* L waits */
        *lx = 128; *ly = 0;        /* stick forward: face the camera's direction */
    }
}

/* Rotate the eye of the camera object at base (eye +0x10, target +0x20) around its target by
 * the current offset, unless the eye is still the one we wrote last time. */
static void rotate_camera(uint32_t base, const char *why) {
    if (s_yaw_off == 0.0f && s_pitch_off == 0.0f) return;
    if (base < 0x08800000u || base >= 0x0A000000u - 0x40u) return;
    int slot = -1, freeslot = -1;
    for (int i = 0; i < MAX_CAMS; i++) {
        if (s_last[i].base == base) { slot = i; break; }
        if (!s_last[i].base && freeslot < 0) freeslot = i;
    }
    const uint32_t eye = base + 0x10u, tgt = base + 0x20u;
    float e[3] = { rdf(eye), rdf(eye + 4), rdf(eye + 8) };
    if (slot >= 0 && memcmp(e, s_last[slot].eye, sizeof(e)) == 0) return;   /* already done */
    if (slot < 0) {
        slot = freeslot >= 0 ? freeslot : 0;
        s_last[slot].base = base;
        fprintf(stderr, "twinstick: rotating camera object 0x%08x (%s)\n", base, why);
    }
    const float t[3] = { rdf(tgt), rdf(tgt + 4), rdf(tgt + 8) };
    const float v[3] = { e[0] - t[0], e[1] - t[1], e[2] - t[2] };
    const float horiz = sqrtf(v[0] * v[0] + v[2] * v[2]);
    const float dist = sqrtf(horiz * horiz + v[1] * v[1]);
    if (!(dist > 1.0f && dist < 100000.0f)) return;
    const float yaw = atan2f(v[0], v[2]) + s_yaw_off;
    float elev = atan2f(v[1], horiz) + s_pitch_off;
    if (elev < ELEV_MIN) elev = ELEV_MIN;
    if (elev > ELEV_MAX) elev = ELEV_MAX;
    const float ch = cosf(elev) * dist;
    e[0] = t[0] + sinf(yaw) * ch;
    e[1] = t[1] + sinf(elev) * dist;
    e[2] = t[2] + cosf(yaw) * ch;
    wrf(eye, e[0]); wrf(eye + 4, e[1]); wrf(eye + 8, e[2]);
    memcpy(s_last[slot].eye, e, sizeof(e));
}

/* Camera-cut detection, run on the view camera's source before it is rotated. A cut (new shot,
 * cutscene camera, respawn) shows up as a different source object or as a jump of the game's
 * own (unrotated) target or eye that no follow motion produces in one frame; the offset is
 * then dropped so the new shot starts exactly as the game frames it. */
static uint32_t s_cut_src = 0;
static float s_cut_tgt[3], s_cut_eye[3];

static void reset_offset(const char *why) {
    if (s_yaw_off != 0.0f || s_pitch_off != 0.0f)
        fprintf(stderr, "twinstick: camera cut (%s), offset reset\n", why);
    s_yaw_off = s_pitch_off = 0.0f;
    memset(s_last, 0, sizeof(s_last));
}

static void check_cut(uint32_t src) {
    if (src < 0x08800000u || src >= 0x0A000000u - 0x40u) return;
    float e[3] = { rdf(src + 0x10), rdf(src + 0x14), rdf(src + 0x18) };
    const float t[3] = { rdf(src + 0x20), rdf(src + 0x24), rdf(src + 0x28) };
    /* Our own rotated eye from earlier this frame is not the game's: compare against the
     * last fresh one instead, which is what the game would have left there. */
    for (int i = 0; i < MAX_CAMS; i++)
        if (s_last[i].base == src && memcmp(e, s_last[i].eye, sizeof(e)) == 0) { memcpy(e, s_cut_eye, sizeof(e)); break; }
    if (!s_cut_src) reset_offset("first camera");
    else {
        const float dt = sqrtf((t[0] - s_cut_tgt[0]) * (t[0] - s_cut_tgt[0]) + (t[1] - s_cut_tgt[1]) * (t[1] - s_cut_tgt[1]) +
                               (t[2] - s_cut_tgt[2]) * (t[2] - s_cut_tgt[2]));
        const float de = sqrtf((e[0] - s_cut_eye[0]) * (e[0] - s_cut_eye[0]) + (e[1] - s_cut_eye[1]) * (e[1] - s_cut_eye[1]) +
                               (e[2] - s_cut_eye[2]) * (e[2] - s_cut_eye[2]));
        const float dist = sqrtf((e[0] - t[0]) * (e[0] - t[0]) + (e[1] - t[1]) * (e[1] - t[1]) + (e[2] - t[2]) * (e[2] - t[2]));
        if (src != s_cut_src) {
            static int n = 0;
            if (n++ < 30) fprintf(stderr, "twinstick: view camera source 0x%08x -> 0x%08x\n", s_cut_src, src);
            reset_offset("new camera source");
        }
        else if (dt > fmaxf(0.3f * dist, 60.0f)) reset_offset("target jump");
        else if (de > fmaxf(0.5f * dist, 100.0f)) reset_offset("eye jump");
    }
    s_cut_src = src;
    memcpy(s_cut_tgt, t, sizeof(t));
    memcpy(s_cut_eye, e, sizeof(e));
}

void sr_func_hook(CpuState *s, uint32_t addr) {
    init_once();
    if (!s_on) return;
    s_hook_tick = s_ticks;
    switch (addr) {
    case HOOK_CAMERA_DERIVE:   /* a0 = camera object base (logic camera) */
        if (s->r[4] == MEM_R32(ADDR_CAMERA_PTR) + ADDR_CAMERA_EYE_OFF - 0x10u)
            rotate_camera(s->r[4], "logic camera");
        break;
    case HOOK_CAMERA_COPY:     /* copy(dst = a0, src = a1): the view camera's source */
        if (s->r[4] == MEM_R32(ADDR_VIEWCAM_PTR) && s->r[5] != s->r[4]) {
            check_cut(s->r[5]);
            rotate_camera(s->r[5], "view camera source");
        }
        break;
    default: break;
    }
}
