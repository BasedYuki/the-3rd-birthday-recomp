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
 * SR_TWINSTICK=0 disables it. Sensitivity and inversion live in twinstick.cfg (see below);
 * the - and = keys adjust sensitivity in game.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"
#include "game_addrs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

float g_script_rx = 0.0f, g_script_ry = 0.0f;   /* SR_PADSCRIPT right-stick override */

static float rdf(uint32_t a) { uint32_t w = MEM_R32(a); float f; memcpy(&f, &w, 4); return f; }
static void wrf(uint32_t a, float f) { uint32_t w; memcpy(&w, &f, 4); MEM_W32(a, w); }

/* Turn rates at full deflection and sensitivity 1, in radians per second of real time (so
 * the feel doesn't depend on how often the game reads the pad): about 120 and 70 degrees
 * a second. Stick deflection goes through a squared response curve, so small pushes give
 * fine control and only a full push turns at the full rate. */
#define YAW_RATE   2.1f
#define PITCH_RATE 1.2f
#define SMOOTH_S   0.06f    /* time constant of the turn-speed smoothing (seconds) */
/* Limits on the final elevation of the eye above the target (radians). */
#define ELEV_MIN (-0.35f)   /* a little below the target */
#define ELEV_MAX (1.2f)     /* high overhead */

/* Settings, kept in twinstick.cfg next to the executable's working directory:
 *   sensitivity=1.0   (the - and = keys change it in game, 0.2 to 4)
 *   invert_x=0
 *   invert_y=0
 * SR_TWINSTICK_SPEED overrides the saved sensitivity for one run. */
/*   recenter=1        (ease the camera back behind Aya once the right stick is released)
 *   recenter_delay=0.8  (seconds without right-stick input before it starts while Aya walks)
 *   recenter_delay_still=2.0  (the same when the left stick is idle: Liberation dashes, fights)
 * R3 (right stick click) or O snaps the camera back behind Aya at once. */
#define CFG_FILE "twinstick.cfg"
#define RECENTER_TC 0.35f   /* time constant of the recentre ease (seconds) */
#define SNAP_TC     0.08f   /* the same for an R3 / O reset */
#define SENS_MIN 0.2f
#define SENS_MAX 4.0f

static int s_on = -1;
static float s_speed = 1.0f;
static int s_inv_x = 0, s_inv_y = 0;
static int s_recenter = 1;
static float s_recenter_delay = 0.8f, s_recenter_delay_still = 2.0f;
static int s_snap = 0;                               /* R3 / O reset in progress */
static float s_idle_s = 0.0f;                        /* time since the right stick was last used */
static int s_moving = 0;                             /* left stick pushed (last pad sample) */
static int s_btn_special = 0;                        /* L or Triangle held (aim / Overdive view) */
static int s_suspended = 0;                          /* a special camera is live: no orbit */
static float s_yaw_off = 0.0f, s_pitch_off = 0.0f;   /* accumulated right-stick orbit */
static float s_yaw_vel = 0.0f, s_pitch_vel = 0.0f;   /* smoothed turn speed (rad/s) */
static unsigned s_ticks = 0, s_hook_tick = 0;        /* pad reads, and the last one with a camera hook */

/* The eye we last wrote into each camera object, so a second pass in the same frame (the
 * game calls these routines more than once) doesn't rotate it again. */
#define MAX_CAMS 8
static struct { uint32_t base; float eye[3]; } s_last[MAX_CAMS];

static void cfg_save(void) {
    FILE *f = fopen(CFG_FILE, "w");
    if (!f) return;
    fprintf(f, "sensitivity=%.2f\ninvert_x=%d\ninvert_y=%d\nrecenter=%d\nrecenter_delay=%.2f\nrecenter_delay_still=%.2f\n",
            s_speed, s_inv_x, s_inv_y, s_recenter, s_recenter_delay, s_recenter_delay_still);
    fclose(f);
}

static void init_once(void) {
    if (s_on >= 0) return;
    const char *e = getenv("SR_TWINSTICK");
    s_on = !(e && e[0] == '0');
    FILE *f = fopen(CFG_FILE, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            float v;
            if (sscanf(line, "sensitivity=%f", &v) == 1) s_speed = v;
            else if (sscanf(line, "invert_x=%f", &v) == 1) s_inv_x = v != 0.0f;
            else if (sscanf(line, "invert_y=%f", &v) == 1) s_inv_y = v != 0.0f;
            else if (sscanf(line, "recenter_delay_still=%f", &v) == 1) s_recenter_delay_still = v;
            else if (sscanf(line, "recenter_delay=%f", &v) == 1) s_recenter_delay = v;
            else if (sscanf(line, "recenter=%f", &v) == 1) s_recenter = v != 0.0f;
        }
        fclose(f);
        cfg_save();                                  /* add keys missing from older files */
    } else {
        cfg_save();                                  /* write the defaults so they can be edited */
    }
    const char *sp = getenv("SR_TWINSTICK_SPEED");
    if (sp) s_speed = (float)atof(sp);
    if (!(s_speed >= SENS_MIN)) s_speed = SENS_MIN;
    if (s_speed > SENS_MAX) s_speed = SENS_MAX;
}

/* Settings for the in-game menu (gui.c). Changes are saved to twinstick.cfg at once. */
float sr_twinstick_setting(int id) {
    init_once();
    switch (id) {
    case SR_TS_SENSITIVITY: return s_speed;
    case SR_TS_INVERT_X:    return (float)s_inv_x;
    case SR_TS_INVERT_Y:    return (float)s_inv_y;
    case SR_TS_RECENTER:    return (float)s_recenter;
    case SR_TS_TURNING:     return (s_yaw_vel != 0.0f || s_pitch_vel != 0.0f) ? 1.0f : 0.0f;
    default:                return 0.0f;
    }
}
void sr_twinstick_set(int id, float v) {
    init_once();
    switch (id) {
    case SR_TS_SENSITIVITY: s_speed = v < SENS_MIN ? SENS_MIN : v > SENS_MAX ? SENS_MAX : v; break;
    case SR_TS_INVERT_X:    s_inv_x = v != 0.0f; break;
    case SR_TS_INVERT_Y:    s_inv_y = v != 0.0f; break;
    case SR_TS_RECENTER:    s_recenter = v != 0.0f; break;
    default: return;
    }
    cfg_save();
}

static double now_s(void) {
    struct timespec ts; timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Once per controller read: integrate the right stick into the orbit offset. */
void sr_twinstick_tick(void) {
    init_once();
    if (!s_on) return;
    static double last = 0.0;
    const double t = now_s();
    float dt = last > 0.0 ? (float)(t - last) : 0.0f;
    last = t;
    if (dt > 0.1f) dt = 0.1f;                        /* a hitch must not become a big jump */

    if (gui_on()) {
        const int steps = gui_sens_steps();
        if (steps) {
            s_speed *= powf(1.15f, (float)steps);
            if (s_speed < SENS_MIN) s_speed = SENS_MIN;
            if (s_speed > SENS_MAX) s_speed = SENS_MAX;
            cfg_save();
            fprintf(stderr, "twinstick: camera sensitivity %.2f\n", s_speed);
        }
    }
    /* Only while the gameplay camera is live (its hooks ran within the last few frames):
     * a stick push during a movie or menu must not bank an offset for later. */
    if (++s_ticks - s_hook_tick > 4) { s_yaw_vel = s_pitch_vel = 0.0f; return; }
    if (s_suspended) { s_yaw_vel = s_pitch_vel = 0.0f; s_idle_s = 0.0f; return; }   /* special camera */
    float rx = 0.0f, ry = 0.0f;
    if (gui_on()) gui_rstick(&rx, &ry);
    if (g_script_rx != 0.0f || g_script_ry != 0.0f) { rx = g_script_rx; ry = g_script_ry; }
    /* squared radial response: direction kept, magnitude m becomes m*m */
    const float m = sqrtf(rx * rx + ry * ry);
    if (m > 1.0f) { rx /= m; ry /= m; }
    else          { rx *= m; ry *= m; }
    if (s_inv_x) rx = -rx;
    if (s_inv_y) ry = -ry;
    /* scripted and turbo runs (real time meaningless there) step one pad read at 30 Hz */
    static int turbo = -1;
    if (turbo < 0) turbo = getenv("SR_NOVBPACE") ? 1 : 0;
    const float step = (turbo || g_script_rx != 0.0f || g_script_ry != 0.0f) ? 1.0f / 30.0f : dt;
    /* ease the turn speed toward the stick so 30 Hz pad reads don't step visibly */
    const float k = step > 0.0f ? 1.0f - expf(-step / SMOOTH_S) : 1.0f;
    s_yaw_vel   += (-rx * YAW_RATE * s_speed - s_yaw_vel) * k;
    s_pitch_vel += ( ry * PITCH_RATE * s_speed - s_pitch_vel) * k;   /* stick up = look up (eye lower) */
    if (fabsf(rx) < 1e-4f && fabsf(s_yaw_vel) < 0.01f) s_yaw_vel = 0.0f;
    if (fabsf(ry) < 1e-4f && fabsf(s_pitch_vel) < 0.01f) s_pitch_vel = 0.0f;
    if (s_yaw_vel != 0.0f) {
        s_yaw_off += s_yaw_vel * step;
        const float pi = 3.14159265f;
        while (s_yaw_off > pi) s_yaw_off -= 2.0f * pi;
        while (s_yaw_off < -pi) s_yaw_off += 2.0f * pi;
    }
    if (s_pitch_vel != 0.0f) {
        s_pitch_off += s_pitch_vel * step;
        if (s_pitch_off > ELEV_MAX - ELEV_MIN) s_pitch_off = ELEV_MAX - ELEV_MIN;
        if (s_pitch_off < ELEV_MIN - ELEV_MAX) s_pitch_off = ELEV_MIN - ELEV_MAX;
    }
    /* Recentre: once the right stick has been left alone, ease the offset back to zero so the
     * game's own follow camera (which keeps itself behind Aya) takes over again. Walking starts
     * it after recenter_delay; otherwise after recenter_delay_still, because Liberation mode
     * dashes and attacks move Aya without the left stick. R3 / O snaps back at once. */
    if (m > 0.0f) s_idle_s = 0.0f;
    else          s_idle_s += step;
    if (gui_on() && gui_cam_reset()) {
        s_snap = 1;
        fprintf(stderr, "twinstick: camera reset\n");
    }
    static int recentring = 0;
    const int due = s_snap || (s_recenter && s_idle_s >= (s_moving ? s_recenter_delay : s_recenter_delay_still));
    if (due && (s_yaw_off != 0.0f || s_pitch_off != 0.0f)) {
        if (!recentring) fprintf(stderr, "twinstick: recentring from yaw %.2f pitch %.2f\n", s_yaw_off, s_pitch_off);
        recentring = 1;
        const float kr = 1.0f - expf(-step / (s_snap ? SNAP_TC : RECENTER_TC));
        s_yaw_off   -= s_yaw_off * kr;
        s_pitch_off -= s_pitch_off * kr;
        if (fabsf(s_yaw_off) < 0.005f)   s_yaw_off = 0.0f;
        if (fabsf(s_pitch_off) < 0.005f) s_pitch_off = 0.0f;
        if (s_yaw_off == 0.0f && s_pitch_off == 0.0f) {
            fprintf(stderr, "twinstick: recentred\n");
            recentring = 0; s_snap = 0;
        }
    } else {
        recentring = 0;
        if (s_yaw_off == 0.0f && s_pitch_off == 0.0f) s_snap = 0;
    }
    if (m > 0.0f) s_snap = 0;                        /* the stick takes over again */
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
    s_moving = abs((int)*lx - 128) >= 40 || abs((int)*ly - 128) >= 40;   /* recentre input */
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
    /* Aim with the right stick: while L reaches the game (aiming, including the egg launcher),
     * the PSP's single analog stick moves the aim. Feed it the right stick when that is pushed,
     * so aiming works like a modern twin-stick shooter; the left stick still works otherwise. */
    if ((*btn & 0x0100u) && gui_on()) {
        float rx = 0.0f, ry = 0.0f;
        gui_rstick(&rx, &ry);
        if (s_inv_x) rx = -rx;
        if (s_inv_y) ry = -ry;
        if (rx * rx + ry * ry > 0.0004f) {
            int ax = 128 + (int)lrintf(rx * 127.0f), ay = 128 + (int)lrintf(ry * 127.0f);
            *lx = (uint8_t)(ax < 0 ? 0 : ax > 255 ? 255 : ax);
            *ly = (uint8_t)(ay < 0 ? 0 : ay > 255 ? 255 : ay);
        }
    }
    /* What the game sees: L (aim, including the egg launcher) or Triangle (Overdive view)
     * switch to the game's own special cameras, which the orbit must leave alone. */
    s_btn_special = (*btn & (0x0100u | 0x1000u)) != 0;
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

/* Special cameras. The orbit only belongs on the game's normal follow camera. Aim (L, also the
 * egg launcher), the Overdive view (Triangle), Overdive jumps and Liberation mode use the game's
 * own cameras; rotating those put the eye inside Aya or fought the game's framing. Besides the
 * buttons, a special camera shows as a field of view (camera object +0x50) different from the
 * follow camera's. The follow camera's fov is learned as the value in use the longest (a small
 * time-weighted table with slow decay), so a Liberation run of a few seconds doesn't become the
 * reference. While a special camera is live the offset is dropped and nothing is rotated, so the
 * normal camera comes back behind Aya. */
#define FOV_SLOTS 6
static struct { float fov, w; } s_fov[FOV_SLOTS];

static float fov_standard(void) {
    int best = -1;
    for (int i = 0; i < FOV_SLOTS; i++)
        if (s_fov[i].w > 3.0f && (best < 0 || s_fov[i].w > s_fov[best].w)) best = i;
    return best >= 0 ? s_fov[best].fov : 0.0f;
}

static void fov_observe(float fov) {
    for (int i = 0; i < FOV_SLOTS; i++) s_fov[i].w *= 0.9999f;   /* ~4 min memory at 30 Hz */
    int hit = -1, low = 0;
    for (int i = 0; i < FOV_SLOTS; i++) {
        if (s_fov[i].w > 0.0f && fabsf(s_fov[i].fov - fov) <= 0.005f * fabsf(fov)) { hit = i; break; }
        if (s_fov[i].w < s_fov[low].w) low = i;
    }
    if (hit < 0) { hit = low; s_fov[hit].fov = fov; s_fov[hit].w = 0.0f; }
    s_fov[hit].w += 1.0f / 30.0f;
}

/* Decide, once per frame on the view camera's source, whether a special camera is live. */
static void update_special(uint32_t src) {
    float fov = rdf(src + 0x50);
    const int fov_ok = fov > 0.05f && fov < 4.0f;
    if (fov_ok && !s_btn_special) fov_observe(fov);
    const float std = fov_standard();
    const int by_fov = fov_ok && std > 0.0f && fabsf(fov - std) > 0.02f * std;
    const int special = s_btn_special || by_fov;
    if (special != s_suspended) {
        static int n = 0;
        if (n++ < 400)
            fprintf(stderr, "twinstick: %s camera (fov %.3f, follow fov %.3f, %s)\n",
                    special ? "special" : "follow", fov, std,
                    s_btn_special ? "button held" : by_fov ? "fov differs" : "back to normal");
        s_suspended = special;
    }
    if (special && (s_yaw_off != 0.0f || s_pitch_off != 0.0f)) {
        reset_offset("special camera");
        s_yaw_vel = s_pitch_vel = 0.0f;
    }
}

void sr_func_hook(CpuState *s, uint32_t addr) {
    init_once();
    if (!s_on) return;
    s_hook_tick = s_ticks;
    switch (addr) {
    case HOOK_CAMERA_DERIVE:   /* a0 = camera object base (logic camera) */
        if (!s_suspended && s->r[4] == MEM_R32(ADDR_CAMERA_PTR) + ADDR_CAMERA_EYE_OFF - 0x10u)
            rotate_camera(s->r[4], "logic camera");
        break;
    case HOOK_CAMERA_COPY:     /* copy(dst = a0, src = a1): the view camera's source */
        if (s->r[4] == MEM_R32(ADDR_VIEWCAM_PTR) && s->r[5] != s->r[4]) {
            check_cut(s->r[5]);
            update_special(s->r[5]);
            if (!s_suspended) rotate_camera(s->r[5], "view camera source");
        }
        break;
    default: break;
    }
}
