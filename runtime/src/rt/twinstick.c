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
 *
 * Modern style (style=1, Esc menu "Camera style"): the camera holds an absolute direction set
 * by the mouse / right stick instead of an offset from the game's follow camera, so it doesn't
 * swing behind Aya when she runs; both cameras get that direction (W walks where you look). The
 * rendered view is also moved over Aya's shoulder: closer, eye and target shifted to the side
 * (only the view camera's source, so the game's logic camera keeps its own distance). A camera
 * cut or a special camera (aim, Overdive) takes the direction from the game's camera again.
 *
 * Free aim (free_aim=1, Esc menu "Free aim"; both styles). The game itself has none: pressing L
 * latches the aim direction (a unit vector in Aya's object) to her facing, the stick then
 * strafes, and her aim point, the aim camera and the crosshair all follow that latched vector.
 * While L is held the runtime writes its own direction there and sets her facing to match where
 * the game turns it into her root matrix (hook mat_from_yaw, versions/<game>.toml), so she, the
 * camera, the crosshair and the shots turn together. It starts where the camera looked when L
 * was pressed (no wait for Aya to turn first); the mouse / right stick move it left/right and
 * up/down while aiming. When L is released the game keeps her facing from the matrix.
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

/* Modern style framing, relative to the game's follow distance */
#define MODERN_DOLLY  0.70f   /* eye this much of the game's distance from the target */
#define MODERN_SIDE   0.22f   /* eye and target moved this much of the distance to the side */
#define MODERN_RAISE  0.06f   /* target raised a little: Aya's shoulder, not her waist */
/* The game's aim camera looks this much to the left of Aya's facing (radians, measured: the
 * same at every facing), so a turn that puts the crosshair on a direction sets her facing a
 * little to the right of it, as the game itself does. */
#define AIM_CAM_OFF   0.0594f
#define AIM_PITCH_MAX 0.9f      /* free aim up / down limit (radians) */
#define MODERN_AIM_GAIN 9.0f  /* mouse radians per pad sample -> aim stick deflection (x4 when steady) */

static int s_on = -1;
static int s_style = 0;                              /* 0 classic orbit, 1 modern (over the shoulder), 2 centred */
static int s_abs_valid = 0;                          /* modern: direction taken from the game yet */
static float s_abs_yaw = 0.0f, s_abs_elev = 0.0f;    /* modern: where the camera looks from */
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
static int s_free_aim = 1;                           /* free aim on */
static int s_aiming = 0;                             /* L reaches the game (last pad sample) */
static int s_aim_pending = 0;                        /* L just pressed: the turn is set on the next pose */
static float s_aim_look = 0.0f;                      /* where the view looked when L was pressed */
static int s_aim_active = 0;                         /* free aim sets Aya's facing */
static int s_aim_moved = 0;                          /* the mouse / right stick moved the aim (this L press) */
static int s_aim_stick = 0;                          /* this aim ignores free aim: feed the stick instead */
static float s_aim_yaw = 0.0f;                       /* the facing it sets (radians, look convention) */
static float s_aim_pitch = 0.0f;                     /* and the aim's elevation (radians, up > 0) */
static float s_view_look = 0.0f;                     /* the view camera's look direction (yaw) */
static int s_view_look_ok = 0;
static uint32_t s_aya = 0;                           /* Aya's object (hook player_pose) */

/* The eye we last wrote into each camera object, so a second pass in the same frame (the
 * game calls these routines more than once) doesn't rotate it again. */
#define MAX_CAMS 8
static struct { uint32_t base; float eye[3]; } s_last[MAX_CAMS];

static void cfg_save(void) {
    FILE *f = fopen(CFG_FILE, "w");
    if (!f) return;
    fprintf(f, "sensitivity=%.2f\ninvert_x=%d\ninvert_y=%d\nrecenter=%d\nrecenter_delay=%.2f\nrecenter_delay_still=%.2f\nstyle=%d\nfree_aim=%d\n",
            s_speed, s_inv_x, s_inv_y, s_recenter, s_recenter_delay, s_recenter_delay_still, s_style, s_free_aim);
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
            else if (sscanf(line, "style=%f", &v) == 1) s_style = v >= 2.0f ? 2 : v != 0.0f;
            else if (sscanf(line, "free_aim=%f", &v) == 1) s_free_aim = v != 0.0f;
        }
        fclose(f);
        cfg_save();                                  /* add keys missing from older files */
    } else {
        cfg_save();                                  /* write the defaults so they can be edited */
    }
    { const char *st = getenv("SR_CAMSTYLE"); if (st) { s_style = atoi(st); if (s_style < 0 || s_style > 2) s_style = 1; } }   /* tests */
    { const char *fa = getenv("SR_FREEAIM"); if (fa) s_free_aim = atoi(fa) != 0; }
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
    case SR_TS_MOVING:      return (float)s_moving;
    case SR_TS_STYLE:       return (float)s_style;
    case SR_TS_FREE_AIM:    return (float)s_free_aim;
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
    case SR_TS_STYLE:       s_style = v >= 2.0f ? 2 : v != 0.0f; s_abs_valid = 0; s_yaw_off = s_pitch_off = 0.0f; break;
    case SR_TS_FREE_AIM:    s_free_aim = v != 0.0f; break;
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
    if (s_suspended) {                               /* special camera */
        s_yaw_vel = s_pitch_vel = 0.0f; s_idle_s = 0.0f;
        if (s_free_aim && s_aiming && !s_aim_stick && gui_on()) {   /* free aim: mouse and right stick turn the aim */
            extern void gui_mouse_look(float *yaw, float *pitch);
            float myaw = 0.0f, mpitch = 0.0f, rx = 0.0f, ry = 0.0f;
            gui_mouse_look(&myaw, &mpitch);
            gui_rstick(&rx, &ry);
            if (g_script_rx != 0.0f) rx = g_script_rx;
            if (s_inv_x) { myaw = -myaw; rx = -rx; }
            rx *= fabsf(rx);                         /* squared response, like the camera */
            if (s_inv_y) { mpitch = -mpitch; ry = -ry; }
            ry *= fabsf(ry);
            if (myaw != 0.0f || mpitch != 0.0f || rx * rx + ry * ry > 0.0004f) s_aim_moved = 1;
            s_aim_yaw += -myaw - rx * YAW_RATE * s_speed * dt;
            s_aim_pitch += -mpitch - ry * PITCH_RATE * s_speed * dt;
            if (s_aim_pitch > AIM_PITCH_MAX) s_aim_pitch = AIM_PITCH_MAX;
            if (s_aim_pitch < -AIM_PITCH_MAX) s_aim_pitch = -AIM_PITCH_MAX;
            const float pi = 3.14159265f;
            while (s_aim_yaw > pi) s_aim_yaw -= 2.0f * pi;
            while (s_aim_yaw < -pi) s_aim_yaw += 2.0f * pi;
        }
        return;
    }
    float rx = 0.0f, ry = 0.0f;
    int m_mouse = 0;                       /* the mouse moved the camera this tick */
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
    {   /* mouse camera: motion turns the camera directly (1:1, no speed curve) */
        extern void gui_mouse_look(float *yaw, float *pitch);
        float myaw = 0.0f, mpitch = 0.0f;
        if (gui_on()) gui_mouse_look(&myaw, &mpitch);
        if (s_inv_x) myaw = -myaw;
        if (myaw != 0.0f || mpitch != 0.0f) {
            if (s_style) {
                s_abs_yaw -= myaw;
                s_abs_elev += mpitch;
            } else {
                s_yaw_off -= myaw;
                s_pitch_off += mpitch;
                if (s_pitch_off > ELEV_MAX - ELEV_MIN) s_pitch_off = ELEV_MAX - ELEV_MIN;
                if (s_pitch_off < ELEV_MIN - ELEV_MAX) s_pitch_off = ELEV_MIN - ELEV_MAX;
            }
            s_idle_s = 0.0f; m_mouse = 1;
        }
    }
    if (s_style) {   /* modern: the stick turns the absolute direction; no automatic recentre */
        s_abs_yaw += s_yaw_vel * step;
        s_abs_elev += s_pitch_vel * step;
        const float pi = 3.14159265f;
        while (s_abs_yaw > pi) s_abs_yaw -= 2.0f * pi;
        while (s_abs_yaw < -pi) s_abs_yaw += 2.0f * pi;
        if (s_abs_elev < ELEV_MIN) s_abs_elev = ELEV_MIN;
        if (s_abs_elev > ELEV_MAX) s_abs_elev = ELEV_MAX;
        if (gui_on() && gui_cam_reset()) {           /* R3 / O: behind Aya again */
            s_abs_valid = 0;
            fprintf(stderr, "twinstick: camera reset\n");
        }
        return;
    }
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
    if (m > 0.0f || m_mouse) s_idle_s = 0.0f;
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

/* End of a free aim (L released, or this aim went back to the stick). The game builds Aya's
 * matrix from her facing only while aiming, so this also runs from the pad filter. The latched
 * direction is levelled again: the follow camera tilts with it. */
static void aim_release(void) {
    if (!s_aim_active) return;
    s_aim_active = 0;
    fprintf(stderr, "twinstick: free aim released (Aya faces %.2f)\n", s_aim_yaw);
    if (!s_aya) return;
    for (uint32_t o = 0x13a0; o <= 0x13b0; o += 0x10) { wrf(s_aya + o, sinf(s_aim_yaw)); wrf(s_aya + o + 4, 0.0f); wrf(s_aya + o + 8, cosf(s_aim_yaw)); }
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
    {   /* SR_FACELOG=1: the stick and Aya's facing, twice a second (movement checks) */
        static int fl = -1; if (fl < 0) fl = getenv("SR_FACELOG") ? 1 : 0;
        if (fl && s_aya && (s_ticks % 30) == 0)
            fprintf(stderr, "face: stick %d,%d (angle %.2f) facing %.3f\n", *lx - 128, *ly - 128,
                    atan2f((float)(*lx - 128), (float)(128 - *ly)), rdf(s_aya + 0x4c4));
    }
    if (!on) return;
    const int l = (*btn & 0x0100u) != 0;
    const int stick_idle = abs((int)*lx - 128) < 40 && abs((int)*ly - 128) < 40;
    if (s_free_aim) {                  /* free aim turns Aya itself, from the first frame of aiming */
        if (l && !prev_l && s_view_look_ok && s_ticks - s_hook_tick <= 4) { s_aim_look = s_view_look; s_aim_pending = 1; }
        if (!l) { aim_release(); s_aim_pending = s_aim_moved = s_aim_stick = 0; }
    } else if (l && !prev_l && stick_idle && fabsf(s_yaw_off) > 0.2f && s_ticks - s_hook_tick <= 4) {
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
    /* Aim with the right stick: while L reaches the game, the PSP's single analog stick moves
     * the aim with the weapons whose aim the game moves itself (sniper scope, grenade / egg
     * launcher; with the others it strafes). Without free aim, or when free aim found that the
     * game doesn't follow it (aim_stick), the right stick and the mouse are fed to it. */
    if ((*btn & 0x0100u) && gui_on() && (!s_free_aim || s_aim_stick)) {
        float rx = 0.0f, ry = 0.0f;
        gui_rstick(&rx, &ry);
        if (s_inv_x) rx = -rx;
        if (s_inv_y) ry = -ry;
        if ((s_style || s_aim_stick) && rx * rx + ry * ry <= 0.0004f) {   /* the mouse moves the aim */
            /* Mouse motion becomes stick deflection that fades over ~0.1 s: the game reads the
             * pad once per frame (30 Hz) and keeps the latest sample, so a one-sample spike
             * could be missed; steady motion settles at 4x the per-sample amount. */
            static float ax_f = 0.0f, ay_f = 0.0f;
            extern void gui_mouse_look(float *yaw, float *pitch);
            float my = 0.0f, mp = 0.0f;
            gui_mouse_look(&my, &mp);
            if (s_inv_x) my = -my;
            ax_f = fmaxf(-1.0f, fminf(1.0f, ax_f * 0.75f + my * MODERN_AIM_GAIN));
            ay_f = fmaxf(-1.0f, fminf(1.0f, ay_f * 0.75f + mp * MODERN_AIM_GAIN));
            if (fabsf(ax_f) < 0.02f) ax_f = 0.0f;
            if (fabsf(ay_f) < 0.02f) ay_f = 0.0f;
            /* past the game's dead zone: any motion starts at 0.35 of a full push */
            rx = ax_f == 0.0f ? 0.0f : copysignf(0.35f + 0.65f * fabsf(ax_f), ax_f);
            ry = ay_f == 0.0f ? 0.0f : copysignf(0.35f + 0.65f * fabsf(ay_f), ay_f);
            static int nlog = 0;
            if ((rx != 0.0f || ry != 0.0f) && nlog < 20) { nlog++; fprintf(stderr, "twinstick: mouse aim %.2f %.2f\n", rx, ry); }
        }
        if (rx * rx + ry * ry > 0.0004f) {
            int ax = 128 + (int)lrintf(rx * 127.0f), ay = 128 + (int)lrintf(ry * 127.0f);
            *lx = (uint8_t)(ax < 0 ? 0 : ax > 255 ? 255 : ax);
            *ly = (uint8_t)(ay < 0 ? 0 : ay > 255 ? 255 : ay);
        }
    }
    /* What the game sees: L (aim, including the egg launcher) or Triangle (Overdive view)
     * switch to the game's own special cameras, which the orbit must leave alone. */
    s_btn_special = (*btn & (0x0100u | 0x1000u)) != 0;
    s_aiming = (*btn & 0x0100u) != 0;
}

/* Rotate the eye of the camera object at base (eye +0x10, target +0x20) around its target by
 * the current offset, unless the eye is still the one we wrote last time. */
/* Modern style: the view camera's target we shifted, in case the game doesn't rewrite it every
 * frame like the eye (then the shift is taken off before it is applied again). */
static uint32_t s_tgt_obj; static float s_tgt_written[3], s_tgt_orig[3];
/* Modern / centred: when a special camera (aim, Overdive) ends, the view eases from where that
 * camera looked to the follow view over BLEND_S instead of jumping (issue #12). */
#define BLEND_S 0.35
static int s_blend_on = 0; static double s_blend_t0;
static float s_bl_eye[3], s_bl_tgt[3], s_sp_eye[3], s_sp_tgt[3];
static int s_sp_ok = 0;
static float s_view_eye[3], s_view_tgt[3];           /* the last follow view we showed */
static int s_view_ok = 0;

/* Walls (issue #11 "seeing through walls"). The game keeps its follow camera out of walls by
 * sweeping the line from Aya to the camera (cam_collide, called from the follow camera at
 * 0x08AC1544 with a1 = start point, a2 = offset to the eye) and shortening the distance where it
 * hits. Our modern / centred camera looks along another line (its own direction, over the
 * shoulder), so the game's sweep missed the walls ours ran into. The sweep is given our line
 * instead; how far the game then lets its eye go (s_col_*) shortens our camera the same way. */
#define COLLIDE_RA 0x08AC1548u
static float s_col_start[3], s_col_len = 0.0f, s_col_nominal = 0.0f;
static unsigned s_col_tick = 0;

static void cam_collide(CpuState *s) {
    if (!s_style || !s_abs_valid || s_suspended || s->r[31] != COLLIDE_RA) return;
    const uint32_t pa = s->r[5], va = s->r[6];
    if (pa < 0x08800000u || pa >= 0x0A000000u - 16u || va < 0x08800000u || va >= 0x0A000000u - 16u) return;
    float p[3] = { rdf(pa), rdf(pa + 4), rdf(pa + 8) };
    const float v[3] = { rdf(va), rdf(va + 4), rdf(va + 8) };
    const float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(len > 1.0f && len < 100000.0f)) return;
    const float yaw = s_abs_yaw, elev = s_abs_elev;
    float want = len;
    if (s_style == 1) {                              /* the same framing as rotate_camera */
        const float side = MODERN_SIDE * len;
        want = len * MODERN_DOLLY;
        p[0] += cosf(yaw) * side; p[2] += -sinf(yaw) * side; p[1] += MODERN_RAISE * want;
    }
    const float ch = cosf(elev) * want;
    wrf(pa, p[0]); wrf(pa + 4, p[1]); wrf(pa + 8, p[2]);
    wrf(va, sinf(yaw) * ch); wrf(va + 4, sinf(elev) * want); wrf(va + 8, cosf(yaw) * ch);
    memcpy(s_col_start, p, sizeof(p));
    s_col_len = want; s_col_nominal = len;
    s_col_tick = s_ticks;
}

static void rotate_camera(uint32_t base, const char *why) {
    const int view = why[0] == 'v';                  /* the view camera's source (else the logic camera) */
    if (!s_style && s_yaw_off == 0.0f && s_pitch_off == 0.0f) return;
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
    float t[3] = { rdf(tgt), rdf(tgt + 4), rdf(tgt + 8) };
    if (s_style && view && base == s_tgt_obj && memcmp(t, s_tgt_written, sizeof(t)) == 0)
        memcpy(t, s_tgt_orig, sizeof(t));            /* our shifted target from last frame: undo */
    const float v[3] = { e[0] - t[0], e[1] - t[1], e[2] - t[2] };
    const float horiz = sqrtf(v[0] * v[0] + v[2] * v[2]);
    float dist = sqrtf(horiz * horiz + v[1] * v[1]);
    if (!(dist > 1.0f && dist < 100000.0f)) return;
    const float game_yaw = atan2f(v[0], v[2]), game_elev = atan2f(v[1], horiz);
    float yaw, elev;
    if (s_style && !view && !s_abs_valid) return;    /* modern: the view camera sets the direction first */
    if (s_style) {                                   /* modern: an absolute direction */
        if (!s_abs_valid) {
            s_abs_yaw = game_yaw; s_abs_elev = game_elev; s_abs_valid = 1;
            fprintf(stderr, "twinstick: modern camera direction from the game (yaw %.2f, elevation %.2f)\n", game_yaw, game_elev);
        }
        yaw = s_abs_yaw;
        /* the logic camera has its own tilt (steeper than the view's): it gets the same change */
        elev = view ? s_abs_elev : game_elev + s_pitch_off;
        if (view) {                                  /* what the aim turn and the menu look at */
            float d = s_abs_yaw - game_yaw;
            while (d > 3.14159265f) d -= 6.2831853f;
            while (d < -3.14159265f) d += 6.2831853f;
            s_yaw_off = d; s_pitch_off = s_abs_elev - game_elev;
        }
    } else {
        yaw = game_yaw + s_yaw_off; elev = game_elev + s_pitch_off;
    }
    if (elev < ELEV_MIN) elev = ELEV_MIN;
    if (elev > ELEV_MAX) elev = ELEV_MAX;
    /* how far the game let its eye go along our line (cam_collide): a wall shortens ours too */
    float wall = 1.0f;
    if (s_style && view && s_col_len > 1.0f && s_ticks - s_col_tick <= 4) {
        const float dx = e[0] - s_col_start[0], dy = e[1] - s_col_start[1], dz = e[2] - s_col_start[2];
        wall = sqrtf(dx * dx + dy * dy + dz * dz) / s_col_len;
        if (wall > 1.0f) wall = 1.0f;
        if (wall < 0.05f) wall = 0.05f;
        dist = s_col_nominal;                        /* the game's eye is on our line now: its own distance */
        { static int n = 0; static float last = 1.0f; if (getenv("SR_WALLLOG") && fabsf(wall - last) > 0.05f && n++ < 200) { last = wall; fprintf(stderr, "WALL %.2f\n", wall); } }
    }
    float tw[3] = { t[0], t[1], t[2] };              /* the target written back */
    if (s_style && view) memcpy(s_tgt_orig, t, sizeof(t));
    if (s_style == 1 && view) {                      /* over the shoulder: closer, to the side */
        const float side = MODERN_SIDE * dist * wall;   /* pulled in by a wall: back towards the middle */
        dist *= MODERN_DOLLY;
        const float rx = cosf(yaw), rz = -sinf(yaw);   /* horizontal, perpendicular to the view */
        tw[0] += rx * side; tw[2] += rz * side; tw[1] += MODERN_RAISE * dist;
    }
    dist *= wall;
    const float ch = cosf(elev) * dist;
    e[0] = tw[0] + sinf(yaw) * ch;
    e[1] = tw[1] + sinf(elev) * dist;
    e[2] = tw[2] + cosf(yaw) * ch;
    if (s_style && view && s_blend_on) {             /* back from aiming / Overdive: ease, don't snap */
        float k = (float)((now_s() - s_blend_t0) / BLEND_S);
        if (k >= 1.0f || k < 0.0f) s_blend_on = 0;
        else {
            k = k * k * (3.0f - 2.0f * k);
            for (int i = 0; i < 3; i++) {
                e[i] = s_bl_eye[i] + (e[i] - s_bl_eye[i]) * k;
                tw[i] = s_bl_tgt[i] + (tw[i] - s_bl_tgt[i]) * k;
            }
        }
    }
    wrf(eye, e[0]); wrf(eye + 4, e[1]); wrf(eye + 8, e[2]);
    if (s_style && view) {
        wrf(tgt, tw[0]); wrf(tgt + 4, tw[1]); wrf(tgt + 8, tw[2]);
        s_tgt_obj = base; memcpy(s_tgt_written, tw, sizeof(tw));
        memcpy(s_view_eye, e, sizeof(e)); memcpy(s_view_tgt, tw, sizeof(tw)); s_view_ok = 1;
    }
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
    s_abs_valid = 0;                                 /* modern: take the new shot's direction */
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
        /* with cam_collide the game's eye follows our turns, so only its target tells a cut */
        else if (de > fmaxf(0.5f * dist, 100.0f) && s_ticks - s_col_tick > 4) reset_offset("eye jump");
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
        if (!special && s_style && s_sp_ok) {        /* the ease starts at the special camera's view */
            memcpy(s_bl_eye, s_sp_eye, sizeof(s_bl_eye)); memcpy(s_bl_tgt, s_sp_tgt, sizeof(s_bl_tgt));
            s_blend_on = 1; s_blend_t0 = now_s();
        }
        s_sp_ok = 0;
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

/* Free aim works where the game's aim camera follows the latched direction (rifles, handguns,
 * shotguns). The sniper scope and the launchers move their aim with the stick instead and ignore
 * it: when the view keeps looking elsewhere for half a second after the aim was moved, this aim
 * goes back to the stick (aim_stick; the mouse / right stick then drive the PSP stick). */
static void aim_check(uint32_t src) {
    static int miss = 0;
    if (!(s_free_aim && s_aiming && s_aim_active && s_aim_moved)) { miss = 0; return; }
    const float dx = rdf(src + 0x20) - rdf(src + 0x10), dy = rdf(src + 0x24) - rdf(src + 0x14), dz = rdf(src + 0x28) - rdf(src + 0x18);
    const float h = sqrtf(dx * dx + dz * dz);
    if (h < 1.0f) return;
    float d = atan2f(dx, dz) - (s_aim_yaw + AIM_CAM_OFF);
    while (d > 3.14159265f) d -= 6.2831853f;
    while (d < -3.14159265f) d += 6.2831853f;
    const int off = fabsf(d) > 0.2f || (fabsf(s_aim_pitch) < 0.3f && fabsf(atan2f(dy, h) - s_aim_pitch) > 0.25f);
    miss = off ? miss + 1 : 0;
    if (miss > 15) {
        miss = 0; aim_release(); s_aim_stick = 1;
        fprintf(stderr, "twinstick: this aim doesn't follow free aim (scope / launcher): the stick moves it\n");
    }
}

int sr_func_hook(CpuState *s, uint32_t addr) {
    /* code mods (mods/<name>/, compiled in by scripts/build.sh) come first and may replace */
    { extern int sr_mods_hook(CpuState *s, uint32_t addr); if (sr_mods_hook(s, addr)) return 1; }
    /* Widescreen: give the game's cameras the window's aspect ratio, so it renders and culls
     * a wider view itself (ge.c then only squeezes draws that still use the PSP's 480:272). */
    if (addr == HOOK_CAM_PROJECTION || addr == HOOK_CAM_SETUP) {
        extern float g_ge_ws;
        const float psp = 480.0f / 272.0f, want = psp * (g_ge_ws > 1.0f ? g_ge_ws : 1.0f);
        if (addr == HOOK_CAM_SETUP) {
            if (fabsf(s->f[13] - psp) < 0.01f) s->f[13] = want;
        } else {
            uint32_t w = MEM_R32(s->r[4] + 0x264u); float a; memcpy(&a, &w, 4);
            if (fabsf(a - psp) < 0.01f || a > psp) { memcpy(&w, &want, 4); MEM_W32(s->r[4] + 0x264u, w); }
        }
        if (getenv("SR_WSLOG")) fprintf(stderr, "widescreen: camera %s aspect -> %.4f\n",
                                        addr == HOOK_CAM_SETUP ? "setup" : "projection", want);
        return 0;
    }
    init_once();
    if (!s_on) return 0;
    switch (addr) {
    case HOOK_CAMERA_DERIVE:   /* a0 = camera object base (logic camera) */
        s_hook_tick = s_ticks;
        if (!s_suspended && s->r[4] == MEM_R32(ADDR_CAMERA_PTR) + ADDR_CAMERA_EYE_OFF - 0x10u)
            rotate_camera(s->r[4], "logic camera");
        break;
    case HOOK_CAMERA_COPY:     /* copy(dst = a0, src = a1): the view camera's source */
        s_hook_tick = s_ticks;
        if (s->r[4] == MEM_R32(ADDR_VIEWCAM_PTR) && s->r[5] != s->r[4]) {
            check_cut(s->r[5]);
            update_special(s->r[5]);
            if (!s_suspended) {
                rotate_camera(s->r[5], "view camera source");
                const uint32_t src = s->r[5];       /* where the picture looks: the free aim's start */
                const float dx = rdf(src + 0x20) - rdf(src + 0x10), dz = rdf(src + 0x28) - rdf(src + 0x18);
                if (dx * dx + dz * dz > 1.0f) { s_view_look = atan2f(dx, dz); s_view_look_ok = 1; }
            } else {
                const uint32_t src = s->r[5];       /* the special camera's view, for the ease back */
                for (int i = 0; i < 3; i++) { s_sp_eye[i] = rdf(src + 0x10 + 4 * i); s_sp_tgt[i] = rdf(src + 0x20 + 4 * i); }
                s_sp_ok = 1;
                /* Free aim, before the game first builds Aya's aim pose: it aims (and its aim
                 * camera looks) along the direction it latched from her old facing when it saw
                 * L, for about three frames, then the free aim takes over and the camera jumped
                 * (issue #15: a jolt when aiming starts). Latch our direction from the start. */
                if (s_free_aim && s_aiming && s_aim_pending && !s_aim_stick && s_aya &&
                    MEM_R32(s_aya + 0x434u) == 0xFFFFFFFFu) {
                    const float y = s_aim_look - AIM_CAM_OFF;
                    static int n = 0;
                    if (getenv("SR_AIMLOG") && n++ < 30)
                        fprintf(stderr, "twinstick: early aim latch %.2f (was %.2f) eye-target yaw %.2f\n", y,
                                atan2f(rdf(s_aya + 0x13a0), rdf(s_aya + 0x13a8)),
                                atan2f(rdf(src + 0x20) - rdf(src + 0x10), rdf(src + 0x28) - rdf(src + 0x18)));
                    for (uint32_t o = 0x13a0; o <= 0x13b0; o += 0x10) { wrf(s_aya + o, sinf(y)); wrf(s_aya + o + 4, 0.0f); wrf(s_aya + o + 8, cosf(y)); }
                    wrf(s_aya + 0x4c4, y);         /* her facing, and her root matrix it is read back from */
                    wrf(s_aya + 0x20, cosf(y)); wrf(s_aya + 0x24, 0.0f); wrf(s_aya + 0x28, -sinf(y));
                    wrf(s_aya + 0x40, sinf(y)); wrf(s_aya + 0x44, 0.0f); wrf(s_aya + 0x48, cosf(y));
                    /* the aim camera holds a view along the game's own (unturned) follow camera
                     * until the aim pose starts: show ours until then */
                    if (s_style && s_view_ok)
                        for (int i = 0; i < 3; i++) { wrf(src + 0x10 + 4 * i, s_view_eye[i]); wrf(src + 0x20 + 4 * i, s_view_tgt[i]); }
                }
                aim_check(src);
            }
        }
        break;
    case HOOK_CAM_COLLIDE:     /* a1 = start, a2 = offset to the eye (follow camera's wall sweep) */
        cam_collide(s);
        break;
    case HOOK_PLAYER_POSE:     /* a0 = Aya's object */
        s_aya = s->r[4];
        {   /* SR_FACEWATCH=1: report the code that writes her facing (debugging movement) */
            static int fw = -1; if (fw < 0) fw = getenv("SR_FACEWATCH") ? 1 : 0;
            if (fw == 1 && s_aya) { extern void sr_watch_set(uint32_t); sr_watch_set(s_aya + 0x4c6); fw = 2; }
            static int rw = -1; if (rw < 0) rw = getenv("SR_READWATCH") ? 1 : 0;   /* readers of her stick copy */
            if (rw == 1 && s_aya) { extern void sr_readwatch_set(uint32_t, uint32_t); extern int g_sr_dbg; sr_readwatch_set(s_aya + 0xe88, 0x10); g_sr_dbg = 1; rw = 2; }
        }
        break;
    case HOOK_MAT_FROM_YAW:    /* a0 = object, $f13 = its facing */
        if (s->r[4] != s_aya || !s_aya) break;
        if (!s_free_aim || !s_aiming || s_aim_stick) { aim_release(); break; }   /* the game keeps the facing from her matrix */
        {
            /* The game's lock-on (Aya + 0x434: the target, -1 = none) picks an enemy when L is
             * pressed near one; its aim point is then the enemy. Free aim leaves that alone until
             * the mouse / right stick moves the aim, then drops the lock and aims by hand. */
            const uint32_t lock = s_aya + 0x434u;
            if (!s_aim_moved && MEM_R32(lock) != 0xFFFFFFFFu) {
                s_aim_active = s_aim_pending = 0;   /* when the aim moves, start from the lock-on */
                break;
            }
            if (!s_aim_active) {
                if (s_aim_pending) s_aim_yaw = s_aim_look - AIM_CAM_OFF;   /* where the camera looked */
                else s_aim_yaw = s->f[13];                                   /* from the lock-on */
                s_aim_pitch = 0.0f; s_aim_active = 1;
                static int n = 0;
                if (n++ < 40) fprintf(stderr, "twinstick: free aim %s (Aya %.2f -> %.2f)\n",
                                      s_aim_pending ? "from the camera" : "takes over from the lock-on", s->f[13], s_aim_yaw);
            }
            s_aim_pending = 0;
            if (s_aim_moved) MEM_W32(lock, 0xFFFFFFFFu);
        }
        if (s_aim_active) {
            /* Absolute: the game reads her facing back from the matrix every frame, so an added
             * turn would add up. */
            s->f[13] = s_aim_yaw;
            /* The aim direction the game latched when L was pressed (Aya + 0x13a0, a copy at
             * +0x13b0; unit vector): her aim point, the aim camera and so the crosshair follow it. */
            const float cp = cosf(s_aim_pitch), dx = sinf(s_aim_yaw) * cp, dy = sinf(s_aim_pitch), dz = cosf(s_aim_yaw) * cp;
            for (uint32_t o = 0x13a0; o <= 0x13b0; o += 0x10) { wrf(s_aya + o, dx); wrf(s_aya + o + 4, dy); wrf(s_aya + o + 8, dz); }
        }
        break;
    default: break;
    }
    return 0;
}
