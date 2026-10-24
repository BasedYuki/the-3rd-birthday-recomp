/* SDL3 + Vulkan presentation backend, Phase 0 (see README.md for the full renderer plan).
 *
 * Scope of this phase: a real Vulkan swapchain fed by the (proven-correct) software GE.
 * Per frame: the 480x272 BGRA framebuffer is memcpy'd into a persistently-mapped host
 * buffer, copied to a device image, and vkCmdBlitImage'd (linear filter, aspect-correct
 * letterbox) onto the swapchain image. No pipelines/descriptors/shaders yet — those arrive
 * in Phase 1 with the GPU rasterizer. Input (keyboard + gamepads) comes from SDL3, mapped
 * to the same PSP sceCtrl bits gui.c uses.
 *
 * Deliberately single-frame-in-flight (fence after submit): presentation cost is trivial
 * and it keeps swapchain recreation and shutdown simple while the architecture settles.
 *
 * AI disclosure: this renderer is an original implementation (it does not reuse PPSSPP's
 * GPU) written with substantial assistance from an LLM (Anthropic Claude). See CREDITS.md.
 * GPLv2+: it consumes ge.c, whose GE semantics are derived from PPSSPP. */

#include "sdl3vk.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>                        /* GDI text for the toast */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define PSP_W 480
#define PSP_H 272

static SDL_Window      *s_win;
static SDL_Gamepad     *s_pad;
static VkInstance       s_inst;
static VkSurfaceKHR     s_surf;
static VkPhysicalDevice s_pdev;
static VkDevice         s_dev;
static uint32_t         s_qfam;
static VkQueue          s_queue;
static VkCommandPool    s_pool;
static VkCommandBuffer  s_cmd;
static VkFence          s_fence;
static VkSemaphore      s_sem_acq, s_sem_done;

static VkSwapchainKHR s_swap;
static VkFormat       s_swap_fmt;
static VkExtent2D     s_swap_ext;
static uint32_t       s_swap_n;
static VkImage        s_swap_img[8];

static VkBuffer        s_staging;
static VkDeviceMemory  s_staging_mem;
static void           *s_staging_map;
static VkImage         s_fbimg;
static VkDeviceMemory  s_fbimg_mem;
static VkImage         s_visimg;           /* visible part of a GE target at render scale */
static VkDeviceMemory  s_visimg_mem;
static int             s_scale = 0;        /* internal render scale (sdl3vk_render_scale) */

static uint32_t s_buttons;
static uint8_t  s_lx = 128, s_ly = 128;
static float    s_rx = 0.0f, s_ry = 0.0f;   /* right stick, -1..1 (twin-stick camera) */
static int      s_sens_steps = 0, s_sens_prev = 0;   /* camera sensitivity keys (- and =) */
static int      s_camreset = 0, s_camreset_prev = 0; /* camera reset press (R3 / O) */
static int      s_menu_ev = 0;                       /* SDL3VK_MENU_* events not yet taken */
static uint32_t s_key_taps = 0;                      /* PSP buttons of keys pressed since the last poll */
/* ---- controls: remappable bindings (controls.cfg, Esc menu -> Controls) ----------------
 * Each action has two keyboard keys and one gamepad button (or trigger). The gamepad sticks
 * stay analog: left stick = PSP stick, right stick = camera. */
enum { BA_CROSS, BA_CIRCLE, BA_SQUARE, BA_TRIANGLE, BA_L, BA_R, BA_START, BA_SELECT,
       BA_UP, BA_DOWN, BA_LEFT, BA_RIGHT, BA_MUP, BA_MDOWN, BA_MLEFT, BA_MRIGHT,
       BA_CUP, BA_CDOWN, BA_CLEFT, BA_CRIGHT, BA_CRESET, BA_COUNT };
enum { PADB_NONE = -1, PADB_LT = 100, PADB_RT = 101 };
static const struct { const char *id, *label; uint32_t bit; } k_acts[BA_COUNT] = {
    { "cross", "Cross", 0x4000 }, { "circle", "Circle", 0x2000 }, { "square", "Square", 0x8000 },
    { "triangle", "Triangle", 0x1000 }, { "l", "L", 0x0100 }, { "r", "R", 0x0200 },
    { "start", "Start", 0x0008 }, { "select", "Select", 0x0001 },
    { "dpad_up", "D-pad up", 0x0010 }, { "dpad_down", "D-pad down", 0x0040 },
    { "dpad_left", "D-pad left", 0x0080 }, { "dpad_right", "D-pad right", 0x0020 },
    { "move_up", "Move forward", 0 }, { "move_down", "Move back", 0 },
    { "move_left", "Move left", 0 }, { "move_right", "Move right", 0 },
    { "camera_up", "Camera up", 0 }, { "camera_down", "Camera down", 0 },
    { "camera_left", "Camera left", 0 }, { "camera_right", "Camera right", 0 },
    { "camera_reset", "Camera behind Aya", 0 },
};
/* Mouse buttons sit in the key slots too, as codes past the keyboard's (left, middle, right, 4, 5);
 * they act only while the mouse is captured, and the "Mouse buttons" option switches them all off. */
#define MOUSE_KEY(b) ((SDL_Scancode)(SDL_SCANCODE_COUNT + (b)))
static const char *const k_mouse_names[6] = { "", "Mouse Left", "Mouse Middle", "Mouse Right", "Mouse 4", "Mouse 5" };
static const char *key_name(SDL_Scancode sc) {   /* "," would split the "key, key" list: Comma */
    if (sc > SDL_SCANCODE_COUNT && sc <= MOUSE_KEY(5)) return k_mouse_names[sc - SDL_SCANCODE_COUNT];
    if (sc == SDL_SCANCODE_COMMA) return "Comma";
    if (sc == SDL_SCANCODE_KP_COMMA) return "Keypad Comma";
    return SDL_GetScancodeName(sc);
}
static SDL_Scancode key_from_name(const char *n) {
    for (int b = 1; b <= 5; b++) if (!SDL_strcasecmp(n, k_mouse_names[b])) return MOUSE_KEY(b);
    if (!SDL_strcasecmp(n, "Comma")) return SDL_SCANCODE_COMMA;
    if (!SDL_strcasecmp(n, "Keypad Comma")) return SDL_SCANCODE_KP_COMMA;
    return SDL_GetScancodeFromName(n);
}
typedef struct { SDL_Scancode key[2]; int pad; } Bind;
static Bind s_bind[BA_COUNT];
static int  s_cap_act = -1, s_cap_slot = 0;          /* waiting for a key/button to bind */

static void bind_defaults(void) {
    static const struct { int a; SDL_Scancode k0, k1; int pad; } d[] = {
        { BA_CROSS, SDL_SCANCODE_X, SDL_SCANCODE_SPACE, SDL_GAMEPAD_BUTTON_SOUTH },
        { BA_CIRCLE, SDL_SCANCODE_Z, SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_EAST },
        { BA_SQUARE, SDL_SCANCODE_C, SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_WEST },
        { BA_TRIANGLE, SDL_SCANCODE_V, SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_NORTH },
        { BA_L, SDL_SCANCODE_Q, MOUSE_KEY(SDL_BUTTON_RIGHT), PADB_LT },     /* right mouse button: aim */
        { BA_R, SDL_SCANCODE_E, MOUSE_KEY(SDL_BUTTON_LEFT), PADB_RT },      /* left mouse button: fire */
        { BA_START, SDL_SCANCODE_RETURN, SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_START },
        { BA_SELECT, SDL_SCANCODE_TAB, SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_BACK },   /* not Shift: held while walking */
        { BA_UP, SDL_SCANCODE_UP, SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_DPAD_UP },
        { BA_DOWN, SDL_SCANCODE_DOWN, SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN },
        { BA_LEFT, SDL_SCANCODE_LEFT, SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT },
        { BA_RIGHT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_DPAD_RIGHT },
        { BA_MUP, SDL_SCANCODE_W, SDL_SCANCODE_UNKNOWN, PADB_NONE },
        { BA_MDOWN, SDL_SCANCODE_S, SDL_SCANCODE_UNKNOWN, PADB_NONE },
        { BA_MLEFT, SDL_SCANCODE_A, SDL_SCANCODE_UNKNOWN, PADB_NONE },
        { BA_MRIGHT, SDL_SCANCODE_D, SDL_SCANCODE_UNKNOWN, PADB_NONE },
        { BA_CUP, SDL_SCANCODE_I, SDL_SCANCODE_UNKNOWN, PADB_NONE },
        { BA_CDOWN, SDL_SCANCODE_K, SDL_SCANCODE_UNKNOWN, PADB_NONE },
        { BA_CLEFT, SDL_SCANCODE_J, SDL_SCANCODE_UNKNOWN, PADB_NONE },
        { BA_CRIGHT, SDL_SCANCODE_L, SDL_SCANCODE_UNKNOWN, PADB_NONE },
        { BA_CRESET, SDL_SCANCODE_O, SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_RIGHT_STICK },
    };
    for (unsigned i = 0; i < sizeof(d) / sizeof(d[0]); i++) {
        s_bind[d[i].a].key[0] = d[i].k0; s_bind[d[i].a].key[1] = d[i].k1; s_bind[d[i].a].pad = d[i].pad;
    }
}
static int s_bump_too = 1;   /* bumpers also give L / R (as before remapping existed) */
/* Mouse camera: a click into the window captures the mouse (Esc or leaving the window releases
 * it); its movement turns the twin-stick camera, the left button is R (fire), the right L (aim).
 * Keyboard smoothing eases WASD direction changes instead of jumping between the 8 directions. */
static int   s_mouse_on = 1, s_mouse_btn = 1, s_mouse_inv = 0, s_kbd_smooth = 1;
/* Controller stick deadzones (controls.cfg, Controls page): radial, and the stick's travel past
 * it is rescaled to the full range, so movement starts smoothly from zero. Worn or noisy sticks
 * (drift) need more, precise ones can use less. */
#define DZ_DEFAULT 0.20f
static float s_dz_l = DZ_DEFAULT, s_dz_r = DZ_DEFAULT;
static void stick_deadzone(float *x, float *y, float dz) {
    const float mag = sqrtf(*x * *x + *y * *y);
    if (mag <= dz) { *x = *y = 0.0f; return; }
    const float sc = (mag >= 1.0f ? 1.0f : (mag - dz) / (1.0f - dz)) / mag;
    *x *= sc; *y *= sc;
}
static float s_mouse_sens = 1.0f;
static int   s_mouse_cap = 0;                   /* captured (relative mode) */
static float s_mdx = 0.0f, s_mdy = 0.0f;        /* motion since the camera last read it */
static void mouse_capture(int on) {
    if (!s_win || on == s_mouse_cap) return;
    s_mouse_cap = on;
    SDL_SetWindowRelativeMouseMode(s_win, on ? true : false);
    s_mdx = s_mdy = 0.0f;
}
static const char *pad_name(int b) {
    if (b == PADB_LT) return "lefttrigger";
    if (b == PADB_RT) return "righttrigger";
    if (b < 0) return "";
    const char *n = SDL_GetGamepadStringForButton((SDL_GamepadButton)b);
    return n ? n : "";
}
static int pad_from_name(const char *n) {
    if (!*n) return PADB_NONE;
    if (!SDL_strcasecmp(n, "lefttrigger")) return PADB_LT;
    if (!SDL_strcasecmp(n, "righttrigger")) return PADB_RT;
    const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(n);
    return b == SDL_GAMEPAD_BUTTON_INVALID ? PADB_NONE : (int)b;
}
static void trim(char *t) {
    char *a = t; while (*a == ' ' || *a == '\t') a++;
    memmove(t, a, strlen(a) + 1);
    size_t n = strlen(t);
    while (n && (t[n - 1] == ' ' || t[n - 1] == '\t' || t[n - 1] == '\r' || t[n - 1] == '\n')) t[--n] = 0;
}
static const char *ctl_path(void) {   /* SR_CONTROLS_CFG: test runs keep the player's file */
    const char *e = getenv("SR_CONTROLS_CFG");
    return e && *e ? e : "controls.cfg";
}
static void bind_save(void) {
    FILE *f = fopen(ctl_path(), "w");
    if (!f) return;
    fprintf(f, "# action = key, key | gamepad button   (keys may be Mouse Left / Middle / Right / 4 / 5;\n"
               "# the Esc menu's Controls page and the launcher edit this file)\nbinds_version = 2\n");
    for (int a = 0; a < BA_COUNT; a++) {
        const char *k0 = s_bind[a].key[0] ? key_name(s_bind[a].key[0]) : "";
        const char *k1 = s_bind[a].key[1] ? key_name(s_bind[a].key[1]) : "";
        fprintf(f, "%s = %s, %s | %s\n", k_acts[a].id, k0, k1, pad_name(s_bind[a].pad));
    }
    fprintf(f, "bumpers_are_l_r = %d\n", s_bump_too);
    fprintf(f, "mouse_camera = %d\nmouse_sensitivity = %.2f\nmouse_invert_y = %d\nmouse_buttons_fire_aim = %d\n"
               "keyboard_smoothing = %d\n", s_mouse_on, s_mouse_sens, s_mouse_inv, s_mouse_btn, s_kbd_smooth);
    fprintf(f, "left_stick_deadzone = %.2f\nright_stick_deadzone = %.2f\n", s_dz_l, s_dz_r);
    fclose(f);
}
static void bind_load(void) {
    bind_defaults();
    FILE *f = fopen(ctl_path(), "r");
    if (!f) { bind_save(); return; }
    char line[256]; int version = 1;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = 0; char *id = line; trim(id);
        if (!SDL_strcasecmp(id, "binds_version")) { version = atoi(eq + 1); continue; }
        if (!SDL_strcasecmp(id, "bumpers_are_l_r")) { s_bump_too = atoi(eq + 1) != 0; continue; }
        if (!SDL_strcasecmp(id, "mouse_camera")) { s_mouse_on = atoi(eq + 1) != 0; continue; }
        if (!SDL_strcasecmp(id, "mouse_sensitivity")) { s_mouse_sens = (float)atof(eq + 1); continue; }
        if (!SDL_strcasecmp(id, "mouse_invert_y")) { s_mouse_inv = atoi(eq + 1) != 0; continue; }
        if (!SDL_strcasecmp(id, "mouse_buttons_fire_aim")) { s_mouse_btn = atoi(eq + 1) != 0; continue; }
        if (!SDL_strcasecmp(id, "keyboard_smoothing")) { s_kbd_smooth = atoi(eq + 1) != 0; continue; }
        if (!SDL_strcasecmp(id, "left_stick_deadzone") || !SDL_strcasecmp(id, "right_stick_deadzone")) {
            float v = (float)atof(eq + 1);
            v = v < 0.0f ? 0.0f : v > 0.6f ? 0.6f : v;
            if (id[0] == 'l' || id[0] == 'L') s_dz_l = v; else s_dz_r = v;
            continue;
        }
        int a = -1; for (int i = 0; i < BA_COUNT; i++) if (!SDL_strcasecmp(id, k_acts[i].id)) a = i;
        if (a < 0) continue;
        char *keys = eq + 1, *bar = strchr(keys, '|'), *pad = (char *)"";
        if (bar) { *bar = 0; pad = bar + 1; }
        char *c = strchr(keys, ','), *k1 = (char *)"";
        if (c) { *c = 0; k1 = c + 1; }
        trim(keys); trim(k1); trim(pad);
        s_bind[a].key[0] = *keys ? key_from_name(keys) : SDL_SCANCODE_UNKNOWN;
        s_bind[a].key[1] = *k1 ? key_from_name(k1) : SDL_SCANCODE_UNKNOWN;
        s_bind[a].pad = pad_from_name(pad);
    }
    fclose(f);
    if (version < 2) {   /* older file: the mouse buttons were fixed to fire / aim, keep that */
        if (!s_bind[BA_R].key[1]) s_bind[BA_R].key[1] = MOUSE_KEY(SDL_BUTTON_LEFT);
        if (!s_bind[BA_L].key[1]) s_bind[BA_L].key[1] = MOUSE_KEY(SDL_BUTTON_RIGHT);
        bind_save();
    }
}
/* for the Controls page (gui.c) */
int  sdl3vk_bind_count(void) { return BA_COUNT; }
const char *sdl3vk_bind_label(int a) { return a >= 0 && a < BA_COUNT ? k_acts[a].label : ""; }
void sdl3vk_bind_text(int a, int slot, char *out, int cap) {
    out[0] = 0;
    if (a < 0 || a >= BA_COUNT) return;
    if (slot < 2) { if (s_bind[a].key[slot]) SDL_strlcpy(out, key_name(s_bind[a].key[slot]), (size_t)cap); }
    else {   /* Xbox-style names on screen; controls.cfg keeps SDL's names */
        static const char *const nice[][2] = {
            { "a", "A" }, { "b", "B" }, { "x", "X" }, { "y", "Y" }, { "back", "View" }, { "guide", "Guide" },
            { "start", "Menu" }, { "leftstick", "LS click" }, { "rightstick", "RS click" },
            { "leftshoulder", "LB" }, { "rightshoulder", "RB" }, { "lefttrigger", "LT" }, { "righttrigger", "RT" },
            { "dpup", "D-pad up" }, { "dpdown", "D-pad down" }, { "dpleft", "D-pad left" }, { "dpright", "D-pad right" },
        };
        const char *n = pad_name(s_bind[a].pad);
        for (unsigned i = 0; i < sizeof(nice) / sizeof(nice[0]); i++) if (!strcmp(n, nice[i][0])) n = nice[i][1];
        SDL_strlcpy(out, n, (size_t)cap);
    }
}
void sdl3vk_bind_capture(int a, int slot) { s_cap_act = a; s_cap_slot = slot; }
int  sdl3vk_bind_capturing(void) { return s_cap_act >= 0; }
void sdl3vk_bind_clear(int a, int slot) {
    if (a < 0 || a >= BA_COUNT) return;
    if (slot < 2) s_bind[a].key[slot] = SDL_SCANCODE_UNKNOWN; else s_bind[a].pad = PADB_NONE;
    bind_save();
}
void sdl3vk_bind_reset(void) {
    bind_defaults(); s_bump_too = 1;
    s_mouse_on = 1; s_mouse_sens = 1.0f; s_mouse_inv = 0; s_mouse_btn = 1; s_kbd_smooth = 1;
    s_dz_l = s_dz_r = DZ_DEFAULT;
    bind_save();
}
/* mouse / keyboard options under the bindings on the Controls page */
int  sdl3vk_input_opt_count(void) { return 7; }
void sdl3vk_input_opt_text(int i, char *out, int cap) {
    switch (i) {
    case 0: snprintf(out, (size_t)cap, "Mouse camera (click the window)\t%s", s_mouse_on ? "On" : "Off"); break;
    case 1: snprintf(out, (size_t)cap, "Mouse sensitivity\t%.2f", s_mouse_sens); break;
    case 2: snprintf(out, (size_t)cap, "Mouse invert up/down\t%s", s_mouse_inv ? "On" : "Off"); break;
    case 3: snprintf(out, (size_t)cap, "Mouse buttons (as bound above)\t%s", s_mouse_btn ? "On" : "Off"); break;
    case 4: snprintf(out, (size_t)cap, "Left stick deadzone\t%d%%", (int)lrintf(s_dz_l * 100.0f)); break;
    case 5: snprintf(out, (size_t)cap, "Right stick deadzone\t%d%%", (int)lrintf(s_dz_r * 100.0f)); break;
    default: snprintf(out, (size_t)cap, "Smooth WASD turning\t%s", s_kbd_smooth ? "On" : "Off"); break;
    }
}
void sdl3vk_input_opt_change(int i, int dir) {
    switch (i) {
    case 0: s_mouse_on = !s_mouse_on; if (!s_mouse_on) mouse_capture(0); break;
    case 1: s_mouse_sens *= dir > 0 ? 1.15f : 1.0f / 1.15f;
            if (s_mouse_sens < 0.1f) s_mouse_sens = 0.1f;
            if (s_mouse_sens > 10.0f) s_mouse_sens = 10.0f; break;
    case 2: s_mouse_inv = !s_mouse_inv; break;
    case 3: s_mouse_btn = !s_mouse_btn; break;
    case 4: case 5: {                                   /* 0..60% in steps of 2 (Enter = up) */
        float *dz = i == 4 ? &s_dz_l : &s_dz_r;
        *dz += dir > 0 ? 0.02f : -0.02f;
        *dz = *dz < 0.0f ? 0.0f : *dz > 0.6f ? 0.6f : *dz;
        *dz = roundf(*dz * 50.0f) / 50.0f;
        break;
    }
    default: s_kbd_smooth = !s_kbd_smooth; break;
    }
    bind_save();
}
/* a captured key / button moves away from whatever action had it */
static void bind_assign_key(SDL_Scancode sc) {
    for (int a = 0; a < BA_COUNT; a++)
        for (int k = 0; k < 2; k++) if (s_bind[a].key[k] == sc) s_bind[a].key[k] = SDL_SCANCODE_UNKNOWN;
    s_bind[s_cap_act].key[s_cap_slot] = sc;
    s_cap_act = -1; bind_save();
}
static void bind_assign_pad(int b) {
    for (int a = 0; a < BA_COUNT; a++) if (s_bind[a].pad == b) s_bind[a].pad = PADB_NONE;
    s_bind[s_cap_act].pad = b;
    if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER || b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) s_bump_too = 0;
    s_cap_act = -1; bind_save();
}
static int key_down(const bool *k, SDL_Scancode sc) {
    if (!sc) return 0;
    if (sc < SDL_SCANCODE_COUNT) return k[sc];
    return s_mouse_cap && s_mouse_btn && (SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_MASK(sc - SDL_SCANCODE_COUNT));
}
static int bind_key_down(const bool *k, int a) {
    return key_down(k, s_bind[a].key[0]) || key_down(k, s_bind[a].key[1]);
}
static int bind_pad_down(SDL_Gamepad *pad, int a) {
    const int b = s_bind[a].pad;
    if (!pad || b == PADB_NONE) return 0;
    if (b == PADB_LT) return SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8192;
    if (b == PADB_RT) return SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8192;
    return SDL_GetGamepadButton(pad, (SDL_GamepadButton)b);
}
static int      s_ws_dirty = 0;                      /* widescreen setting changed */
static int      s_shot_req = 0;                      /* F12: save the next frame */
static SDL_Joystick *s_vjoy = NULL;                  /* SR_VPAD test pad */
int sdl3vk_dual_src = 0;                             /* device has dualSrcBlend enabled */
/* The SDL joystick API belongs to the main thread: ctrl_server.c queues, the event pump applies. */
static volatile LONG s_vq_w, s_vq_r; static int s_vq[64][3];
static void vq_push(int kind, int a, int b) {
    LONG w = s_vq_w; if (w - s_vq_r >= 64) return;
    s_vq[w & 63][0] = kind; s_vq[w & 63][1] = a; s_vq[w & 63][2] = b;
    InterlockedExchange(&s_vq_w, w + 1);
}
void sdl3vk_vpad_button(int button, int down) { vq_push(0, button, down); }
void sdl3vk_vpad_axis(int axis, int value) { vq_push(1, axis, value); }
void sdl3vk_vmouse_button(int button, int down) { vq_push(2, button, down); }   /* tests: a mouse click event */
static void vq_apply(void) {
    while (s_vq_r != s_vq_w) {
        const int *q = s_vq[s_vq_r & 63];
        if (q[0] == 2) {
            SDL_Event e; SDL_zero(e);
            e.type = q[2] ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.windowID = s_win ? SDL_GetWindowID(s_win) : 0; e.button.button = (Uint8)q[1]; e.button.down = q[2] != 0;
            SDL_PushEvent(&e);
        } else if (s_vjoy) {
            const bool ok = q[0] == 0 ? SDL_SetJoystickVirtualButton(s_vjoy, q[1], q[2] != 0)
                                      : SDL_SetJoystickVirtualAxis(s_vjoy, q[1], (Sint16)q[2]);
            if (getenv("SR_KEYLOG")) fprintf(stderr, "vpad: %s %d = %d -> %s (pad=%p)%s\n", q[0] ? "axis" : "button", q[1], q[2],
                                             ok ? "ok" : SDL_GetError(), (void *)s_pad, s_pad ? "" : " NO GAMEPAD OPEN");
        }
        InterlockedIncrement(&s_vq_r);
    }
}
static int      s_pad_present;

#define VK_TRY(expr) do { VkResult vr_ = (expr); if (vr_ != VK_SUCCESS) { \
    fprintf(stderr, "sdl3vk: %s failed: %d\n", #expr, (int)vr_); return 0; } } while (0)

static uint32_t find_mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(s_pdev, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

/* ---- swapchain ---------------------------------------------------------------------- */

static void destroy_swapchain(void) {
    if (s_swap) { vkDestroySwapchainKHR(s_dev, s_swap, NULL); s_swap = VK_NULL_HANDLE; }
}

static int create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_pdev, s_surf, &caps));

    /* Prefer BGRA8 UNORM (matches the framebuffer byte order); fall back to whatever the
     * surface offers first. */
    VkSurfaceFormatKHR fmts[32]; uint32_t nf = 32;
    VK_TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(s_pdev, s_surf, &nf, fmts));
    VkSurfaceFormatKHR pick = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) { pick = fmts[i]; break; }
    s_swap_fmt = pick.format;

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) {   /* surface lets us choose: use the window pixel size */
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(s_win, &w, &h);
        ext.width  = (uint32_t)w;  ext.height = (uint32_t)h;
    }
    if (ext.width  < caps.minImageExtent.width)  ext.width  = caps.minImageExtent.width;
    if (ext.width  > caps.maxImageExtent.width)  ext.width  = caps.maxImageExtent.width;
    if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
    if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    if (ext.width == 0 || ext.height == 0) return 0;   /* minimized: keep old swapchain */
    s_swap_ext = ext;

    uint32_t n = caps.minImageCount + 1;
    if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;

    VkSwapchainKHR old = s_swap;
    VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sci.surface          = s_surf;
    sci.minImageCount    = n;
    sci.imageFormat      = pick.format;
    sci.imageColorSpace  = pick.colorSpace;
    sci.imageExtent      = ext;
    sci.imageArrayLayers = 1;
    sci.imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform     = caps.currentTransform;
    sci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    /* Game speed is paced by the scheduler's vblank clock (sched.c vblank_pace), not by
     * presentation. MAILBOX (no tearing, never blocks) avoids beating against the monitor
     * refresh; FIFO is the mandated fallback. */
    sci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
    {
        VkPresentModeKHR pm[8]; uint32_t npm = 8;
        if (vkGetPhysicalDeviceSurfacePresentModesKHR(s_pdev, s_surf, &npm, pm) >= VK_SUCCESS)
            for (uint32_t i = 0; i < npm; i++)
                if (pm[i] == VK_PRESENT_MODE_MAILBOX_KHR) { sci.presentMode = pm[i]; break; }
    }
    sci.clipped          = VK_TRUE;
    sci.oldSwapchain     = old;
    VK_TRY(vkCreateSwapchainKHR(s_dev, &sci, NULL, &s_swap));
    if (old) vkDestroySwapchainKHR(s_dev, old, NULL);

    s_swap_n = 8;
    VK_TRY(vkGetSwapchainImagesKHR(s_dev, s_swap, &s_swap_n, s_swap_img));
    return 1;
}

/* ---- toast: a short status message in the window corner ("Game saved") -------------------
 * The text is drawn with GDI into a small bitmap (the system UI font, antialiased), then
 * copied onto the swapchain image after the frame blit. It is sized from the window height
 * and re-rendered only when the message or the size changes. */
#define TOAST_MAX_W 1400
#define TOAST_MAX_H 160
/* A text label drawn with GDI into a host-visible buffer, copied onto the swapchain image. */
typedef struct {
    char msg[96], drawn[96];
    int w, h, px;
    uint32_t panel;                          /* background colour (BGRX), 0 = none (dark) */
    VkBuffer buf; VkDeviceMemory mem; void *map;
} Label;
static Label  s_toast, s_fps;
static Uint64 s_toast_until = 0;             /* SDL_GetTicks() deadline */
static int    s_show_fps = -1;               /* graphics.cfg show_fps (Esc menu) */
static Uint64 s_fps_t0 = 0; static int s_fps_shown = 0, s_fps_game = 0, s_fps_v = 0, s_fps_gv = 0;

void sdl3vk_toast(const char *msg, int ms) {
    SDL_strlcpy(s_toast.msg, msg ? msg : "", sizeof(s_toast.msg));
    s_toast_until = SDL_GetTicks() + (Uint64)(ms > 0 ? ms : 2500);
}

/* Render a label's bitmap for a font of `px` pixels. Returns 0 if unavailable. */
static int label_render(Label *L, int px, int accent) {
    if (!strcmp(L->drawn, L->msg) && L->px == px && L->w) return 1;
    if (!L->buf) {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = (VkDeviceSize)TOAST_MAX_W * TOAST_MAX_H * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateBuffer(s_dev, &bci, NULL, &L->buf) != VK_SUCCESS) return 0;
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(s_dev, L->buf, &mr);
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = find_mem_type(mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(s_dev, &mai, NULL, &L->mem) != VK_SUCCESS) return 0;
        vkBindBufferMemory(s_dev, L->buf, L->mem, 0);
        vkMapMemory(s_dev, L->mem, 0, VK_WHOLE_SIZE, 0, &L->map);
    }
    HDC dc = CreateCompatibleDC(NULL);
    HFONT font = CreateFontA(-px, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Segoe UI");
    HGDIOBJ oldf = SelectObject(dc, font);
    SIZE ts; GetTextExtentPoint32A(dc, L->msg, (int)strlen(L->msg), &ts);
    int pad = px * 2 / 3, bar = accent ? (px / 5 > 2 ? px / 5 : 2) : 0;
    int w = ts.cx + pad * 2 + bar, h = ts.cy + pad;
    if (w > TOAST_MAX_W) w = TOAST_MAX_W;
    if (h > TOAST_MAX_H) h = TOAST_MAX_H;
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;       /* top-down */
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    int ok = bm && bits;
    if (ok) {
        HGDIOBJ oldb = SelectObject(dc, bm);
        uint32_t *p = (uint32_t *)bits;
        for (int i = 0; i < w * h; i++) p[i] = 0x00141414u;              /* dark panel (BGRX) */
        for (int y = 0; y < h; y++) for (int x = 0; x < bar; x++) p[y * w + x] = 0x00E08A2Eu;  /* orange edge */
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(240, 240, 240));
        TextOutA(dc, bar + pad, pad / 2, L->msg, (int)strlen(L->msg));
        GdiFlush();
        uint32_t *dst = (uint32_t *)L->map;
        for (int i = 0; i < w * h; i++) dst[i] = p[i] | 0xFF000000u;   /* BGRA, opaque */
        SelectObject(dc, oldb);
        DeleteObject(bm);
    }
    SelectObject(dc, oldf);
    DeleteObject(font);
    DeleteDC(dc);
    if (!ok) return 0;
    L->w = w; L->h = h; L->px = px;
    SDL_strlcpy(L->drawn, L->msg, sizeof(L->drawn));
    return 1;
}

static void label_copy(const Label *L, VkImage dst, int x, int y) {
    VkBufferImageCopy c = {0};
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.bufferRowLength = (uint32_t)L->w;
    c.imageOffset.x = x; c.imageOffset.y = y;
    c.imageExtent.width = (uint32_t)L->w; c.imageExtent.height = (uint32_t)L->h; c.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_cmd, L->buf, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

/* FPS counter: frames shown per second and game frames per second (sceDisplaySetFrameBuf). */
void sdl3vk_fps_game_frame(void) { s_fps_game++; }
int  sdl3vk_show_fps(void) {
    if (s_show_fps < 0) { const char *e = getenv("SR_SHOWFPS"); s_show_fps = e ? atoi(e) : sdl3vk_cfg_get("show_fps", 0); }
    return s_show_fps;
}
void sdl3vk_set_show_fps(int on) { s_show_fps = on; sdl3vk_cfg_set("show_fps", on); }
static int s_glow = -1;
int  sdl3vk_glow(void) {
    if (s_glow < 0) { const char *e = getenv("SR_GLOW"); s_glow = e ? atoi(e) : sdl3vk_cfg_get("glow", 1); }
    return s_glow;
}
void sdl3vk_set_glow(int on) { s_glow = on; sdl3vk_cfg_set("glow", on); }

/* Record the toast and the FPS counter onto a swapchain image in TRANSFER_DST layout. */
static void toast_record(VkImage dst) {
    int dh = (int)s_swap_ext.height, dw = (int)s_swap_ext.width;
    const Uint64 now = SDL_GetTicks();
    s_fps_shown++;
    if (!s_fps_t0) s_fps_t0 = now;
    if (now - s_fps_t0 >= 1000) {
        s_fps_v = (int)((s_fps_shown * 1000 + 500) / (now - s_fps_t0));
        s_fps_gv = (int)((s_fps_game * 1000 + 500) / (now - s_fps_t0));
        s_fps_shown = s_fps_game = 0; s_fps_t0 = now;
    }
    if (sdl3vk_show_fps() && s_fps_v) {
        snprintf(s_fps.msg, sizeof(s_fps.msg), "%d FPS   (game %d)", s_fps_v, s_fps_gv);
        int px = dh / 40; if (px < 12) px = 12; if (px > 40) px = 40;
        if (label_render(&s_fps, px, 0) && s_fps.w + px < dw && s_fps.h + px < dh)
            label_copy(&s_fps, dst, px / 2, px / 2);
    }
    if (!s_toast.msg[0] || now >= s_toast_until) return;
    int px = dh / 28; if (px < 14) px = 14; if (px > 64) px = 64;
    if (!label_render(&s_toast, px, 1)) return;
    int margin = px;
    if (s_toast.w + margin > dw || s_toast.h + margin > dh) return;
    label_copy(&s_toast, dst, dw - s_toast.w - margin, dh - s_toast.h - margin);
}

/* ---- settings menu panel ----------------------------------------------------------------
 * Same technique as the toast: GDI text into a bitmap, copied onto the swapchain image after
 * the frame blit, centred. Line 0 is the title; the selected line gets an orange marker. */
#define MENU_MAX_LINES 20
#define MENU_MAX_W 1600
#define MENU_MAX_H 1400
static char     s_menu_lines[MENU_MAX_LINES][96];
static int      s_menu_n = 0, s_menu_sel = 0, s_menu_dirty = 1;
static int      s_menu_w = 0, s_menu_h = 0, s_menu_px = 0;
static VkBuffer s_menu_buf; static VkDeviceMemory s_menu_mem; static void *s_menu_map;

int sdl3vk_menu_events(void) { int e = s_menu_ev; s_menu_ev = 0; return e; }

void sdl3vk_menu_show(const char *const *lines, int n, int sel) {
    if (n > MENU_MAX_LINES) n = MENU_MAX_LINES;
    int changed = n != s_menu_n || sel != s_menu_sel;
    for (int i = 0; i < n; i++) {
        if (strncmp(s_menu_lines[i], lines[i], sizeof(s_menu_lines[i]) - 1)) changed = 1;
        SDL_strlcpy(s_menu_lines[i], lines[i], sizeof(s_menu_lines[i]));
    }
    s_menu_n = n; s_menu_sel = sel;
    if (changed) s_menu_dirty = 1;
}

static int menu_render(int px) {
    if (!s_menu_dirty && s_menu_px == px && s_menu_w) return 1;
    if (!s_menu_buf) {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = (VkDeviceSize)MENU_MAX_W * MENU_MAX_H * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateBuffer(s_dev, &bci, NULL, &s_menu_buf) != VK_SUCCESS) return 0;
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(s_dev, s_menu_buf, &mr);
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = find_mem_type(mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(s_dev, &mai, NULL, &s_menu_mem) != VK_SUCCESS) return 0;
        vkBindBufferMemory(s_dev, s_menu_buf, s_menu_mem, 0);
        vkMapMemory(s_dev, s_menu_mem, 0, VK_WHOLE_SIZE, 0, &s_menu_map);
    }
    HDC dc = CreateCompatibleDC(NULL);
    HFONT font = CreateFontA(-px, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Segoe UI");
    HFONT bold = CreateFontA(-px * 6 / 5, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Segoe UI");
    HGDIOBJ oldf = SelectObject(dc, font);
    const int pad = px, line_h = px * 8 / 5, bar = px / 4 > 3 ? px / 4 : 3;
    int w = 0;
    for (int i = 0; i < s_menu_n; i++) {
        SIZE ts;
        SelectObject(dc, i == 0 ? bold : font);
        const int tabs[3] = { px * 11, px * 18, px * 25 };   /* tab columns (Controls page) */
        const DWORD ext = GetTabbedTextExtentA(dc, s_menu_lines[i], (int)strlen(s_menu_lines[i]), 3, tabs);
        ts.cx = LOWORD(ext);
        if (ts.cx > w) w = ts.cx;
    }
    w += pad * 3 + bar;
    int h = pad * 2 + line_h * s_menu_n + line_h / 2;
    if (w > MENU_MAX_W) w = MENU_MAX_W;
    if (h > MENU_MAX_H) h = MENU_MAX_H;
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    int ok = bm && bits;
    if (ok) {
        HGDIOBJ oldb = SelectObject(dc, bm);
        uint32_t *p = (uint32_t *)bits;
        for (int i = 0; i < w * h; i++) p[i] = 0x00161616u;                        /* panel */
        for (int y = 0; y < h; y++) for (int x = 0; x < bar; x++) p[y * w + x] = 0x00E08A2Eu;
        SetBkMode(dc, TRANSPARENT);
        for (int i = 0; i < s_menu_n; i++) {
            int y = pad + i * line_h + (i > 0 ? line_h / 2 : 0);
            if (i == s_menu_sel && i > 0)                                           /* selection */
                for (int yy = y - px / 5; yy < y + line_h - px / 5 && yy < h; yy++)
                    for (int x = bar; x < w; x++) p[yy * w + x] = 0x00402A18u;
            SelectObject(dc, i == 0 ? bold : font);
            SetTextColor(dc, i == 0 ? RGB(240, 150, 60) : i == s_menu_sel ? RGB(255, 255, 255) : RGB(190, 190, 190));
            const int tabs[3] = { px * 11, px * 18, px * 25 };
            TabbedTextOutA(dc, bar + pad, y, s_menu_lines[i], (int)strlen(s_menu_lines[i]), 3, tabs, bar + pad);
        }
        GdiFlush();
        uint32_t *dst = (uint32_t *)s_menu_map;
        for (int i = 0; i < w * h; i++) dst[i] = p[i] | 0xFF000000u;
        SelectObject(dc, oldb);
        DeleteObject(bm);
    }
    SelectObject(dc, oldf);
    DeleteObject(font); DeleteObject(bold);
    DeleteDC(dc);
    if (!ok) return 0;
    s_menu_w = w; s_menu_h = h; s_menu_px = px; s_menu_dirty = 0;
    return 1;
}

static void menu_record(VkImage dst) {
    if (s_menu_n <= 0) return;
    int dh = (int)s_swap_ext.height, dw = (int)s_swap_ext.width;
    int px = dh / 30; if (px < 13) px = 13; if (px > 48) px = 48;
    if (!menu_render(px)) return;
    if (s_menu_w > dw || s_menu_h > dh) return;
    VkBufferImageCopy c = {0};
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.bufferRowLength = (uint32_t)s_menu_w;
    c.imageOffset.x = (dw - s_menu_w) / 2;
    c.imageOffset.y = (dh - s_menu_h) / 2;
    c.imageExtent.width = (uint32_t)s_menu_w; c.imageExtent.height = (uint32_t)s_menu_h; c.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_cmd, s_menu_buf, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

/* Widescreen (graphics.cfg widescreen=1, Esc menu "Aspect ratio"): how much wider than the
 * PSP's 480:272 the window is. ge.c squeezes the 3D (wider field of view) and the HUD by this
 * and the presenter stretches the picture back over the whole window. 1 = original. */
float sdl3vk_widescreen_factor(void) {
    static int on = -1;
    if (on < 0 || s_ws_dirty) {
        const char *e = getenv("SR_WIDESCREEN");   /* tests: override graphics.cfg */
        on = e ? atoi(e) : sdl3vk_cfg_get("widescreen", 0);
        s_ws_dirty = 0;
    }
    if (!on || !s_swap_ext.height) return 1.0f;
    float f = ((float)s_swap_ext.width / (float)s_swap_ext.height) / ((float)PSP_W / (float)PSP_H);
    return f < 1.0f ? 1.0f : f > 3.0f ? 3.0f : f;
}
void sdl3vk_set_widescreen(int on) { sdl3vk_cfg_set("widescreen", on); s_ws_dirty = 1; }

int  sdl3vk_fullscreen(void) { return s_win && (SDL_GetWindowFlags(s_win) & SDL_WINDOW_FULLSCREEN) != 0; }
void sdl3vk_set_fullscreen(int on) { if (s_win) SDL_SetWindowFullscreen(s_win, on ? true : false); }
void sdl3vk_request_screenshot(void) { s_shot_req = 1; }
/* Test runs: save the next frame as <SR_SHOTDIR>/<name>.png (no toast). */
static char s_shot_name[64], s_shot_path[300];
/* ctrl_server.c: save the next frame at a full path */
void sdl3vk_request_screenshot_path(const char *path) {
    snprintf(s_shot_path, sizeof(s_shot_path), "%s", path);
    s_shot_req = 1;
}
void sdl3vk_request_screenshot_named(const char *name) {
    snprintf(s_shot_name, sizeof(s_shot_name), "%s", name);
    s_shot_req = 1;
}

/* ---- screenshots (F12) -------------------------------------------------------------------
 * The visible game image (at the internal resolution, without the menu or toasts) is copied to
 * a host buffer during the present and saved as PNG through GDI+ into screenshots/. */
typedef struct { UINT32 GdiplusVersion; void *DebugEventCallback; BOOL SuppressBackgroundThread; BOOL SuppressExternalCodecs; } GpStartupIn;
int __stdcall GdiplusStartup(ULONG_PTR *token, const GpStartupIn *input, void *output);
int __stdcall GdipCreateBitmapFromScan0(INT w, INT h, INT stride, INT format, BYTE *scan0, void **bitmap);
int __stdcall GdipSaveImageToFile(void *image, const WCHAR *filename, const CLSID *encoder, const void *params);
int __stdcall GdipDisposeImage(void *image);
int __stdcall GdipCreateBitmapFromFile(const WCHAR *filename, void **bitmap);
int __stdcall GdipGetImageWidth(void *image, UINT *w);
int __stdcall GdipGetImageHeight(void *image, UINT *h);
typedef struct { INT X, Y, Width, Height; } GpRect;
typedef struct { UINT Width, Height; INT Stride; INT PixelFormat; void *Scan0; UINT_PTR Reserved; } GpBitmapData;
int __stdcall GdipBitmapLockBits(void *bitmap, const GpRect *rect, UINT flags, INT format, GpBitmapData *data);
int __stdcall GdipBitmapUnlockBits(void *bitmap, GpBitmapData *data);
static int gdip_ready(void) {
    static ULONG_PTR token = 0;
    if (!token) { GpStartupIn in = { 1, NULL, FALSE, FALSE }; if (GdiplusStartup(&token, &in, NULL) != 0) token = 0; }
    return token != 0;
}
/* PNG files for texture packs (ge_gpu.c): RGBA8 with R in the low byte; GDI+ keeps BGRA. */
static const CLSID k_png_clsid = { 0x557CF406, 0x1A04, 0x11D3, { 0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E } };
int sdl3vk_png_save(const char *path, const uint32_t *rgba, int w, int h) {
    if (!gdip_ready() || w <= 0 || h <= 0) return 0;
    uint32_t *px = malloc((size_t)w * (size_t)h * 4);
    if (!px) return 0;
    for (int i = 0; i < w * h; i++) {
        const uint32_t c = rgba[i];
        px[i] = (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
    }
    WCHAR name[MAX_PATH]; _snwprintf(name, MAX_PATH, L"%hs", path); name[MAX_PATH - 1] = 0;
    void *bmp = NULL;
    int ok = GdipCreateBitmapFromScan0(w, h, w * 4, 0x0026200A /* 32bppARGB */, (BYTE *)px, &bmp) == 0 && bmp &&
             GdipSaveImageToFile(bmp, name, &k_png_clsid, NULL) == 0;
    if (bmp) GdipDisposeImage(bmp);
    free(px);
    return ok;
}
uint32_t *sdl3vk_png_load(const char *path, int *w, int *h) {
    if (!gdip_ready()) return NULL;
    WCHAR name[MAX_PATH]; _snwprintf(name, MAX_PATH, L"%hs", path); name[MAX_PATH - 1] = 0;
    void *bmp = NULL;
    if (GdipCreateBitmapFromFile(name, &bmp) != 0 || !bmp) return NULL;
    UINT iw = 0, ih = 0; GdipGetImageWidth(bmp, &iw); GdipGetImageHeight(bmp, &ih);
    uint32_t *out = NULL;
    GpRect r = { 0, 0, (INT)iw, (INT)ih };
    GpBitmapData bd;
    if (iw && ih && iw <= 4096 && ih <= 4096 && GdipBitmapLockBits(bmp, &r, 1 /* read */, 0x0026200A, &bd) == 0) {
        out = malloc((size_t)iw * ih * 4);
        if (out)
            for (UINT y = 0; y < ih; y++) {
                const uint32_t *row = (const uint32_t *)((const uint8_t *)bd.Scan0 + (ptrdiff_t)y * bd.Stride);
                for (UINT x = 0; x < iw; x++) {
                    const uint32_t c = row[x];
                    out[y * iw + x] = (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
                }
            }
        GdipBitmapUnlockBits(bmp, &bd);
    }
    GdipDisposeImage(bmp);
    if (out) { *w = (int)iw; *h = (int)ih; }
    return out;
}

static VkBuffer s_shot_buf; static VkDeviceMemory s_shot_mem; static void *s_shot_map;
static VkDeviceSize s_shot_cap = 0;
static int s_shot_w = 0, s_shot_h = 0, s_shot_pending = 0;

/* Record a copy of `src` (TRANSFER_SRC, w x h BGRA) into the screenshot buffer. */
static void shot_record(VkImage src, int w, int h) {
    VkDeviceSize need = (VkDeviceSize)w * h * 4;
    if (need > s_shot_cap) {
        if (s_shot_buf) { vkDestroyBuffer(s_dev, s_shot_buf, NULL); vkFreeMemory(s_dev, s_shot_mem, NULL); s_shot_buf = VK_NULL_HANDLE; }
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = need; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(s_dev, &bci, NULL, &s_shot_buf) != VK_SUCCESS) return;
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(s_dev, s_shot_buf, &mr);
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = find_mem_type(mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(s_dev, &mai, NULL, &s_shot_mem) != VK_SUCCESS) return;
        vkBindBufferMemory(s_dev, s_shot_buf, s_shot_mem, 0);
        vkMapMemory(s_dev, s_shot_mem, 0, VK_WHOLE_SIZE, 0, &s_shot_map);
        s_shot_cap = need;
    }
    VkBufferImageCopy c = {0};
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)w; c.imageExtent.height = (uint32_t)h; c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(s_cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_shot_buf, 1, &c);
    s_shot_w = w; s_shot_h = h; s_shot_pending = 1;
}

/* After the present's fence: write the PNG. */
static void shot_save(void) {
    if (!s_shot_pending) return;
    s_shot_pending = 0;
    static ULONG_PTR token = 0;
    if (!token) { GpStartupIn in = { 1, NULL, FALSE, FALSE }; if (GdiplusStartup(&token, &in, NULL) != 0) { token = 0; return; } }
    WCHAR name[400];
    const char *sd = getenv("SR_SHOTDIR");
    const int named = (sd && s_shot_name[0]) || s_shot_path[0];
    if (s_shot_path[0]) {
        _snwprintf(name, 400, L"%hs", s_shot_path);
        s_shot_path[0] = 0;
    } else if (named) {
        CreateDirectoryA(sd, NULL);
        _snwprintf(name, 400, L"%hs\\%hs.png", sd, s_shot_name);
        s_shot_name[0] = 0;
    } else {
        const char *dir = sd && *sd ? sd : "screenshots";   /* test runs keep the player's folder clean */
        CreateDirectoryA(dir, NULL);
        SYSTEMTIME st; GetLocalTime(&st);
        _snwprintf(name, 400, L"%hs\\3rd_Birthday_%04u-%02u-%02u_%02u-%02u-%02u-%03u.png", dir,
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    }
    name[399] = 0;
    /* the alpha byte of the frame is not image content: force opaque */
    uint32_t *px = (uint32_t *)s_shot_map;
    for (int i = 0; i < s_shot_w * s_shot_h; i++) px[i] |= 0xFF000000u;
    static const CLSID png = { 0x557CF406, 0x1A04, 0x11D3, { 0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E } };
    void *bmp = NULL;
    int ok = GdipCreateBitmapFromScan0(s_shot_w, s_shot_h, s_shot_w * 4, 0x0026200A /* 32bppARGB */,
                                       (BYTE *)s_shot_map, &bmp) == 0 && bmp &&
             GdipSaveImageToFile(bmp, name, &png, NULL) == 0;
    if (bmp) GdipDisposeImage(bmp);
    char msg[160];
    if (ok) snprintf(msg, sizeof(msg), "Screenshot saved (%dx%d)", s_shot_w, s_shot_h);
    else snprintf(msg, sizeof(msg), "Screenshot failed");
    fprintf(stderr, "screenshot: %s %ls\n", msg, name);
    if (!named) sdl3vk_toast(msg, 2000);
}

/* ---- render scale ------------------------------------------------------------------ */

/* Internal resolution multiplier for the GPU renderer (1 = PSP native 480x272, 4 = 1920x1088).
 * From SR_SCALE, else graphics.cfg in the working directory:
 *   render_scale=0   (0 = match the display: the smallest scale that covers its height)
 * A missing graphics.cfg is written with the default so it can be edited. */
/* graphics.cfg: "key=integer" lines. */
#define GCFG_MAX 16
static int gcfg_load(char keys[GCFG_MAX][40], int *vals) {
    int n = 0;
    FILE *f = fopen("graphics.cfg", "r");
    if (!f) return 0;
    char line[128];
    while (n < GCFG_MAX && fgets(line, sizeof(line), f)) {
        char k[40]; int v;
        if (sscanf(line, "%39[^=]=%d", k, &v) == 2) { SDL_strlcpy(keys[n], k, 40); vals[n++] = v; }
    }
    fclose(f);
    return n;
}

int sdl3vk_cfg_get(const char *key, int def) {
    char keys[GCFG_MAX][40]; int vals[GCFG_MAX];
    const int n = gcfg_load(keys, vals);
    for (int i = 0; i < n; i++) if (!strcmp(keys[i], key)) return vals[i];
    return def;
}

void sdl3vk_cfg_set(const char *key, int v) {
    char keys[GCFG_MAX][40]; int vals[GCFG_MAX];
    int n = gcfg_load(keys, vals), i;
    for (i = 0; i < n; i++) if (!strcmp(keys[i], key)) break;
    if (i == n) { if (n == GCFG_MAX) return; SDL_strlcpy(keys[n++], key, 40); }
    vals[i] = v;
    FILE *f = fopen("graphics.cfg", "w");
    if (!f) return;
    for (int k = 0; k < n; k++) fprintf(f, "%s=%d\n", keys[k], vals[k]);
    fclose(f);
}

int sdl3vk_render_scale_cfg(void) { return sdl3vk_cfg_get("render_scale", 0); }
void sdl3vk_set_render_scale_cfg(int v) { sdl3vk_cfg_set("render_scale", v); }

int sdl3vk_render_scale(void) {
    if (s_scale > 0) return s_scale;
    int want = 0;
    const char *e = getenv("SR_SCALE");
    if (e && e[0]) want = atoi(e);
    else {
        FILE *f = fopen("graphics.cfg", "r");
        if (f) {
            char line[128];
            while (fgets(line, sizeof(line), f)) { int v; if (sscanf(line, "render_scale=%d", &v) == 1) want = v; }
            fclose(f);
        } else if ((f = fopen("graphics.cfg", "w")) != NULL) {
            fprintf(f, "render_scale=0\n");
            fclose(f);
        }
    }
    if (want <= 0) {                                /* auto: cover the display height */
        want = 2;
        SDL_DisplayID d = s_win ? SDL_GetDisplayForWindow(s_win) : SDL_GetPrimaryDisplay();
        const SDL_DisplayMode *m = d ? SDL_GetCurrentDisplayMode(d) : NULL;
        if (m && m->h > 0) {
            int ph = (int)(m->h * (m->pixel_density > 0.0f ? m->pixel_density : 1.0f));
            want = (ph + PSP_H - 1) / PSP_H;
        }
    }
    if (want < 1) want = 1;
    if (want > 8) want = 8;
    s_scale = want;
    return s_scale;
}

/* ---- init --------------------------------------------------------------------------- */

int sdl3vk_init(const char *title) {
    if (getenv("SR_VPAD")) SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");   /* tests drive an unfocused window */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "sdl3vk: SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }
    /* Window: the whole-number multiple of 480x272 nearest to 3/4 of the screen height
     * (1440x816 on a 1080p screen), at least 2x. F11 / Alt+Enter toggles fullscreen. */
    int wk = 2;
    {
        const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
        if (m && m->h > 0) { wk = (m->h * 3 / 4 + PSP_H / 2) / PSP_H; if (wk < 2) wk = 2; }
    }
    int ww = PSP_W * wk, wh = PSP_H * wk;
    { const char *e = getenv("SR_WINSIZE");   /* <w>x<h>: e.g. a 21:9 window for widescreen tests */
      int a, b; if (e && sscanf(e, "%dx%d", &a, &b) == 2 && a > 0 && b > 0) { ww = a; wh = b; } }
    s_win = SDL_CreateWindow(title ? title : "PSP Recomp",
                             ww, wh,
                             SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!s_win) {
        fprintf(stderr, "sdl3vk: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 0;
    }
    if (getenv("SR_OFFSCREEN")) {   /* test runs: keep the window off the desktop, never take focus */
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        SDL_SetWindowPosition(s_win, -4000, 200);
    }

    Uint32 next = 0;
    const char * const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&next);
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.pApplicationName = "psp_recomp";
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = next;
    ici.ppEnabledExtensionNames = sdl_ext;
    VK_TRY(vkCreateInstance(&ici, NULL, &s_inst));

    if (!SDL_Vulkan_CreateSurface(s_win, s_inst, NULL, &s_surf)) {
        fprintf(stderr, "sdl3vk: SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        return 0;
    }

    /* Physical device: first one with a graphics queue that can present to our surface. */
    VkPhysicalDevice devs[16]; uint32_t nd = 16;
    VK_TRY(vkEnumeratePhysicalDevices(s_inst, &nd, devs));
    /* Pick the best device that can present: a discrete GPU over an integrated one (laptops
     * list the integrated GPU first). SR_GPU_DEVICE=<name substring> picks one explicitly. */
    s_pdev = VK_NULL_HANDLE;
    {
        const char *want = getenv("SR_GPU_DEVICE");
        int best = -1;
        for (uint32_t d = 0; d < nd; d++) {
            VkQueueFamilyProperties qf[16]; uint32_t nq = 16;
            vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &nq, qf);
            int fam = -1;
            for (uint32_t q = 0; q < nq && fam < 0; q++) {
                VkBool32 can_present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(devs[d], q, s_surf, &can_present);
                if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && can_present) fam = (int)q;
            }
            if (fam < 0) continue;
            VkPhysicalDeviceProperties pp;
            vkGetPhysicalDeviceProperties(devs[d], &pp);
            int score = pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                      : pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
            if (want && want[0] && SDL_strcasestr(pp.deviceName, want)) score = 10;
            fprintf(stderr, "sdl3vk: device %u: %s (score %d)\n", d, pp.deviceName, score);
            if (score > best) { best = score; s_pdev = devs[d]; s_qfam = (uint32_t)fam; }
        }
    }
    if (!s_pdev) { fprintf(stderr, "sdl3vk: no usable Vulkan device\n"); return 0; }
    {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(s_pdev, &pp);
        fprintf(stderr, "sdl3vk: using %s\n", pp.deviceName);
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = s_qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char *dev_ext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    /* dual-source blending: the PSP's (1 - 2*src alpha) destination factor (ge_gpu.c) */
    VkPhysicalDeviceFeatures have, want = {0};
    vkGetPhysicalDeviceFeatures(s_pdev, &have);
    if (have.dualSrcBlend && !getenv("SR_NODUALSRC")) { want.dualSrcBlend = VK_TRUE; sdl3vk_dual_src = 1; }
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pEnabledFeatures = &want;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_ext;
    VK_TRY(vkCreateDevice(s_pdev, &dci, NULL, &s_dev));
    vkGetDeviceQueue(s_dev, s_qfam, 0, &s_queue);

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = s_qfam;
    VK_TRY(vkCreateCommandPool(s_dev, &cpi, NULL, &s_pool));
    VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = s_pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VK_TRY(vkAllocateCommandBuffers(s_dev, &cbi, &s_cmd));

    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VK_TRY(vkCreateFence(s_dev, &fci, NULL, &s_fence));
    VkSemaphoreCreateInfo sci2 = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VK_TRY(vkCreateSemaphore(s_dev, &sci2, NULL, &s_sem_acq));
    VK_TRY(vkCreateSemaphore(s_dev, &sci2, NULL, &s_sem_done));

    /* Persistently-mapped upload buffer + the device-local blit source image. */
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = PSP_W * PSP_H * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VK_TRY(vkCreateBuffer(s_dev, &bci, NULL, &s_staging));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(s_dev, s_staging, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem_type(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_TRY(vkAllocateMemory(s_dev, &mai, NULL, &s_staging_mem));
    VK_TRY(vkBindBufferMemory(s_dev, s_staging, s_staging_mem, 0));
    VK_TRY(vkMapMemory(s_dev, s_staging_mem, 0, VK_WHOLE_SIZE, 0, &s_staging_map));

    VkImageCreateInfo imi = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imi.imageType = VK_IMAGE_TYPE_2D;
    imi.format = VK_FORMAT_B8G8R8A8_UNORM;
    imi.extent.width = PSP_W; imi.extent.height = PSP_H; imi.extent.depth = 1;
    imi.mipLevels = 1; imi.arrayLayers = 1;
    imi.samples = VK_SAMPLE_COUNT_1_BIT;
    imi.tiling = VK_IMAGE_TILING_OPTIMAL;
    imi.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_TRY(vkCreateImage(s_dev, &imi, NULL, &s_fbimg));
    vkGetImageMemoryRequirements(s_dev, s_fbimg, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_TRY(vkAllocateMemory(s_dev, &mai, NULL, &s_fbimg_mem));
    VK_TRY(vkBindImageMemory(s_dev, s_fbimg, s_fbimg_mem, 0));

    /* GPU-rendered frames: the visible part of a target, at the internal render scale */
    {
        const int sc = sdl3vk_render_scale();
        imi.extent.width = (uint32_t)(PSP_W * sc); imi.extent.height = (uint32_t)(PSP_H * sc);
        VK_TRY(vkCreateImage(s_dev, &imi, NULL, &s_visimg));
        vkGetImageMemoryRequirements(s_dev, s_visimg, &mr);
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = find_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_TRY(vkAllocateMemory(s_dev, &mai, NULL, &s_visimg_mem));
        VK_TRY(vkBindImageMemory(s_dev, s_visimg, s_visimg_mem, 0));
    }

    if (!create_swapchain()) return 0;

    if (getenv("SR_VPAD")) {   /* tests: a virtual Xbox-style pad driven by ctrl_server.c */
        SDL_VirtualJoystickDesc d;
        SDL_INIT_INTERFACE(&d);
        d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        d.naxes = SDL_GAMEPAD_AXIS_COUNT;
        d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        d.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1u;   /* standard layout: index = SDL button */
        d.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
        d.name = "SR virtual pad";
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
        s_vjoy = id ? SDL_OpenJoystick(id) : NULL;
        fprintf(stderr, "sdl3vk: virtual pad %s\n", s_vjoy ? "attached" : SDL_GetError());
    }
    if (SDL_HasGamepad()) {
        int npads = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&npads);
        if (ids && npads > 0) s_pad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    fprintf(stderr, "sdl3vk: init ok (%ux%u swapchain, fmt %d, gamepad=%s)\n",
            s_swap_ext.width, s_swap_ext.height, (int)s_swap_fmt, s_pad ? "yes" : "no");
    return 1;
}

/* ---- input -------------------------------------------------------------------------- */

static void poll_input(int *quit) {
    SDL_Event ev;
    static int loaded = 0;
    if (!loaded) { bind_load(); loaded = 1; }
    vq_apply();                                   /* SR_VPAD test inputs */
    SDL_UpdateJoysticks();
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_EVENT_QUIT: *quit = 1; break;
        case SDL_EVENT_KEY_DOWN:
            if (s_cap_act >= 0) {                 /* Controls page: this key becomes the binding */
                if (!ev.key.repeat) {
                    if (ev.key.scancode == SDL_SCANCODE_ESCAPE) s_cap_act = -1;
                    else bind_assign_key(ev.key.scancode);
                }
                break;
            }
            if (!ev.key.repeat)
                for (int a = 0; a < BA_COUNT; a++)
                    if (k_acts[a].bit && (s_bind[a].key[0] == ev.key.scancode || s_bind[a].key[1] == ev.key.scancode))
                        s_key_taps |= k_acts[a].bit;
            if (getenv("SR_KEYLOG"))
                fprintf(stderr, "keys: down key=0x%x scancode=%d mod=0x%x repeat=%d\n",
                        (unsigned)ev.key.key, (int)ev.key.scancode, (unsigned)ev.key.mod, (int)ev.key.repeat);
            if (!ev.key.repeat && (ev.key.key == SDLK_F11 ||
                                   (ev.key.key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT)))) {
                SDL_SetWindowFullscreen(s_win, !(SDL_GetWindowFlags(s_win) & SDL_WINDOW_FULLSCREEN));
                break;
            }
            if (!ev.key.repeat && ev.key.key == SDLK_F12) s_shot_req = 1;
            /* settings menu: Esc opens/closes it; arrows, Enter/Space/X and Backspace drive it */
            switch (ev.key.key) {
            case SDLK_ESCAPE:    if (!ev.key.repeat) { mouse_capture(0); s_menu_ev |= SDL3VK_MENU_TOGGLE; fprintf(stderr, "input: Esc (settings menu)\n"); } break;
            case SDLK_UP:    case SDLK_KP_8: case SDLK_W: s_menu_ev |= SDL3VK_MENU_UP; break;
            case SDLK_DOWN:  case SDLK_KP_2: case SDLK_S: s_menu_ev |= SDL3VK_MENU_DOWN; break;
            case SDLK_LEFT:  case SDLK_KP_4: case SDLK_A: s_menu_ev |= SDL3VK_MENU_LEFT; break;
            case SDLK_RIGHT: case SDLK_KP_6: case SDLK_D: s_menu_ev |= SDL3VK_MENU_RIGHT; break;
            case SDLK_RETURN: case SDLK_SPACE: case SDLK_X:
                if (!ev.key.repeat && !(ev.key.mod & SDL_KMOD_ALT)) s_menu_ev |= SDL3VK_MENU_OK;
                break;
            case SDLK_BACKSPACE: case SDLK_Z: if (!ev.key.repeat) s_menu_ev |= SDL3VK_MENU_BACK; break;
            case SDLK_DELETE: if (!ev.key.repeat) s_menu_ev |= SDL3VK_MENU_CLEAR; break;
            default: break;
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (s_cap_act >= 0) {
                if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) s_cap_act = -1;
                else bind_assign_pad(ev.gbutton.button);
                break;
            }
            switch (ev.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_WEST:       s_menu_ev |= SDL3VK_MENU_CLEAR; break;
            case SDL_GAMEPAD_BUTTON_DPAD_UP:    s_menu_ev |= SDL3VK_MENU_UP; break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  s_menu_ev |= SDL3VK_MENU_DOWN; break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  s_menu_ev |= SDL3VK_MENU_LEFT; break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: s_menu_ev |= SDL3VK_MENU_RIGHT; break;
            case SDL_GAMEPAD_BUTTON_SOUTH:      s_menu_ev |= SDL3VK_MENU_OK; break;
            case SDL_GAMEPAD_BUTTON_EAST:       s_menu_ev |= SDL3VK_MENU_BACK; break;
            case SDL_GAMEPAD_BUTTON_GUIDE:      s_menu_ev |= SDL3VK_MENU_TOGGLE; fprintf(stderr, "input: pad Guide (settings menu)\n"); break;
            case SDL_GAMEPAD_BUTTON_START:      /* Back (View) + Start (Menu): settings menu */
            case SDL_GAMEPAD_BUTTON_BACK:
                if (s_pad && SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_START) &&
                    SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_BACK))
                    s_menu_ev |= SDL3VK_MENU_TOGGLE;
                break;
            default: break;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (s_cap_act >= 0) {                 /* Controls page: a mouse button for a key column */
                if (s_cap_slot < 2 && ev.button.button >= 1 && ev.button.button <= 5)
                    bind_assign_key(MOUSE_KEY(ev.button.button));
                break;
            }
            if (s_mouse_on && !s_mouse_cap && s_menu_n == 0) {
                mouse_capture(1);                 /* this click only captures */
                fprintf(stderr, "input: mouse captured (Esc releases)\n");
            } else if (s_mouse_cap && s_mouse_btn && ev.button.button >= 1 && ev.button.button <= 5) {
                const SDL_Scancode mk = MOUSE_KEY(ev.button.button);   /* a quick click still counts */
                for (int a = 0; a < BA_COUNT; a++)
                    if (k_acts[a].bit && (s_bind[a].key[0] == mk || s_bind[a].key[1] == mk)) s_key_taps |= k_acts[a].bit;
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (s_mouse_cap) { s_mdx += ev.motion.xrel; s_mdy += ev.motion.yrel; }
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            mouse_capture(0);
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (s_cap_act >= 0 && ev.gaxis.value > 16000) {
                if (ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) bind_assign_pad(PADB_LT);
                else if (ev.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) bind_assign_pad(PADB_RT);
            }
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!s_pad) s_pad = SDL_OpenGamepad(ev.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (s_pad && SDL_GetGamepadID(s_pad) == ev.gdevice.which) {
                SDL_CloseGamepad(s_pad); s_pad = NULL;
            }
            break;
        default: break;
        }
    }

    /* Keyboard: WASD walk (the PSP analog stick), the Z X C V row is the face buttons,
     * Q / E the shoulders, arrows the D-pad, I J K L the camera. Keys pressed and released
     * between two polls still count once (s_key_taps), so a quick tap is never lost. */
    uint32_t b = s_key_taps;
    s_key_taps = 0;
    const bool *k = SDL_GetKeyboardState(NULL);
    for (int a = 0; a < BA_COUNT; a++)
        if (k_acts[a].bit && (bind_key_down(k, a) || bind_pad_down(s_pad, a))) b |= k_acts[a].bit;
    if (s_cap_act >= 0) b = 0;                    /* binding a key: nothing reaches the game */
    if ((b & 0x0008u) && (SDL_GetModState() & SDL_KMOD_ALT)) b &= ~0x0008u;   /* Alt+Enter: fullscreen */

    uint8_t lx = 128, ly = 128;
    {
        #define BD(a) (bind_key_down(k, a) || bind_pad_down(s_pad, a))
        int kx = (BD(BA_MRIGHT) ? 1 : 0) - (BD(BA_MLEFT) ? 1 : 0);
        int ky = (BD(BA_MDOWN) ? 1 : 0) - (BD(BA_MUP) ? 1 : 0);
        float tx = (float)kx, ty = (float)ky;
        if (kx && ky) { tx *= 0.7071f; ty *= 0.7071f; }   /* diagonals at full length, not more */
        static float sx = 0.0f, sy = 0.0f; static uint64_t last = 0;
        const uint64_t now = SDL_GetTicksNS();
        const float dt = last ? (float)(now - last) * 1e-9f : 0.0f;
        last = now;
        if (s_kbd_smooth && dt > 0.0f && dt < 0.25f) {
            /* the direction swings round over ~60 ms; starting and stopping stay immediate */
            const float k = 1.0f - expf(-dt / 0.06f);
            if (!kx && !ky) { sx = sy = 0.0f; }
            else if (sx == 0.0f && sy == 0.0f) { sx = tx; sy = ty; }
            else {
                sx += (tx - sx) * k; sy += (ty - sy) * k;
                const float len = sqrtf(sx * sx + sy * sy);  /* keep full deflection while turning */
                if (len > 1e-3f) { sx /= len; sy /= len; }
            }
        } else { sx = tx; sy = ty; }
        if (sx != 0.0f || sy != 0.0f) {
            lx = (uint8_t)(128.0f + sx * 127.0f + (sx < 0 ? -0.5f : 0.5f));
            ly = (uint8_t)(128.0f + sy * 127.0f + (sy < 0 ? -0.5f : 0.5f));
            if (lx < 1) lx = 1;
            if (ly < 1) ly = 1;
        }
    }
    /* Right stick for the camera: I/J/K/L on the keyboard, the gamepad's right stick below. */
    float rx = 0.0f, ry = 0.0f;
    if (BD(BA_CLEFT))  rx -= 1.0f;
    if (BD(BA_CRIGHT)) rx += 1.0f;
    if (BD(BA_CUP))    ry -= 1.0f;
    if (BD(BA_CDOWN))  ry += 1.0f;
    { int sk = k[SDL_SCANCODE_EQUALS] ? 1 : k[SDL_SCANCODE_MINUS] ? -1 : 0;
      if (sk && sk != s_sens_prev) s_sens_steps += sk;
      s_sens_prev = sk; }
    {   /* camera reset: right stick click (R3) or O */
        int cr = BD(BA_CRESET);
        #undef BD
        if (cr && !s_camreset_prev) s_camreset = 1;
        s_camreset_prev = cr;
    }
    s_pad_present = s_pad != NULL;
    if (s_pad) {
        if (s_bump_too && s_cap_act < 0) {        /* bumpers give L / R too, unless bound to something */
            if (SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))  b |= 0x0100;
            if (SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) b |= 0x0200;
        }
        /* both sticks: radial deadzone (left_stick_deadzone / right_stick_deadzone), rescaled */
        float px = SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        float py = SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        stick_deadzone(&px, &py, s_dz_l);
        if (px != 0.0f || py != 0.0f) {                  /* the pad's stick wins over the keys */
            const int ix = (int)lrintf(128.0f + px * 127.0f), iy = (int)lrintf(128.0f + py * 127.0f);
            lx = (uint8_t)(ix < 0 ? 0 : ix > 255 ? 255 : ix);
            ly = (uint8_t)(iy < 0 ? 0 : iy > 255 ? 255 : iy);
        }
        float gx = SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
        float gy = SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f;
        stick_deadzone(&gx, &gy, s_dz_r);
        rx += gx; ry += gy;
    }
    s_rx = rx < -1.0f ? -1.0f : rx > 1.0f ? 1.0f : rx;
    s_ry = ry < -1.0f ? -1.0f : ry > 1.0f ? 1.0f : ry;
    /* button changes go to the log (capped): "the menu opened while walking" reports */
    static int nlog = 0;
    if (b != s_buttons && nlog < 4000) { nlog++; fprintf(stderr, "input: buttons 0x%04x\n", (unsigned)b); }
    if (getenv("SR_KEYLOG") && (b != s_buttons || lx != s_lx || ly != s_ly))
        fprintf(stderr, "keys: buttons 0x%04x stick %u,%u\n", (unsigned)b, (unsigned)lx, (unsigned)ly);
    s_buttons = b;
    s_lx = lx; s_ly = ly;
}

int sdl3vk_get_vk(Sdl3VkInfo *out) {
    if (!s_dev || !out) return 0;
    out->instance = (void *)s_inst;
    out->physical = (void *)s_pdev;
    out->device   = (void *)s_dev;
    out->queue    = (void *)s_queue;
    out->queue_family = s_qfam;
    return 1;
}

uint32_t sdl3vk_buttons(void) { return s_buttons; }
void sdl3vk_analog(uint8_t *lx, uint8_t *ly) { if (lx) *lx = s_lx; if (ly) *ly = s_ly; }
int  sdl3vk_pad_present(void) { return s_pad_present; }
void sdl3vk_rstick(float *rx, float *ry) { if (rx) *rx = s_rx; if (ry) *ry = s_ry; }
/* Mouse camera: motion since the last call, already scaled to radians (yaw, pitch). */
void sdl3vk_mouse_look(float *yaw, float *pitch) {
    const float rad_per_px = 0.0022f * s_mouse_sens;
    *yaw = s_mdx * rad_per_px;
    *pitch = s_mdy * rad_per_px * (s_mouse_inv ? -1.0f : 1.0f);
    s_mdx = s_mdy = 0.0f;
}
void sdl3vk_mouse_test(float dx, float dy) { s_mouse_cap = 1; s_mdx += dx; s_mdy += dy; }   /* ctrl "mouse" */
int  sdl3vk_sens_steps(void) { int n = s_sens_steps; s_sens_steps = 0; return n; }
int  sdl3vk_cam_reset(void) { int n = s_camreset; s_camreset = 0; return n; }

/* ---- present ------------------------------------------------------------------------ */

static void barrier(VkCommandBuffer cmd, VkImage img,
                    VkImageLayout from, VkImageLayout to,
                    VkAccessFlags src_acc, VkAccessFlags dst_acc,
                    VkPipelineStageFlags src_st, VkPipelineStageFlags dst_st) {
    VkImageMemoryBarrier mb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    mb.srcAccessMask = src_acc;
    mb.dstAccessMask = dst_acc;
    mb.oldLayout = from;
    mb.newLayout = to;
    mb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.image = img;
    mb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    mb.subresourceRange.levelCount = 1;
    mb.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, src_st, dst_st, 0, 0, NULL, 0, NULL, 1, &mb);
}

/* Common present: blit `src` (already TRANSFER_SRC_OPTIMAL; srcw x srch source region)
 * onto the swapchain with aspect-correct letterboxing. upload!=NULL additionally
 * records the staging->s_fbimg copy first (the CPU framebuffer path). */
static int present_common(VkImage src, int srcw, int srch, int do_upload) {
    int quit = 0;
    poll_input(&quit);
    if (quit) return 0;

    for (int attempt = 0; attempt < 2; attempt++) {
        uint32_t idx = 0;
        VkResult ar = vkAcquireNextImageKHR(s_dev, s_swap, UINT64_MAX, s_sem_acq,
                                            VK_NULL_HANDLE, &idx);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) {
            vkDeviceWaitIdle(s_dev);
            if (!create_swapchain()) return 1;   /* minimized: drop the frame, stay alive */
            continue;
        }
        if (ar != VK_SUCCESS) return 1;

        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(s_cmd, 0);
        vkBeginCommandBuffer(s_cmd, &bi);

        if (do_upload) {
            /* staging buffer -> fb image */
            barrier(s_cmd, s_fbimg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy bic = {0};
            bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            bic.imageSubresource.layerCount = 1;
            bic.imageExtent.width = PSP_W; bic.imageExtent.height = PSP_H; bic.imageExtent.depth = 1;
            vkCmdCopyBufferToImage(s_cmd, s_staging, s_fbimg,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
            barrier(s_cmd, s_fbimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        } else if (src != s_fbimg) {
            /* A GE target is 512 wide. Scaling its 480x272 sub-rect with a linear blit makes
             * the last screen column blend in the off-screen texels at x=480 (blit filtering
             * clamps at the image edge, not the region's), so copy the visible part 1:1 into
             * the visible-size image first and scale from that. */
            barrier(s_cmd, s_visimg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkImageBlit cp = {0};
            cp.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            cp.srcSubresource.layerCount = 1;
            cp.srcOffsets[1].x = srcw; cp.srcOffsets[1].y = srch; cp.srcOffsets[1].z = 1;
            cp.dstSubresource = cp.srcSubresource;
            cp.dstOffsets[1].x = srcw; cp.dstOffsets[1].y = srch; cp.dstOffsets[1].z = 1;
            vkCmdBlitImage(s_cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           s_visimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp, VK_FILTER_NEAREST);
            barrier(s_cmd, s_visimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            src = s_visimg;
        }

        if (s_shot_req) { shot_record(src, srcw, srch); s_shot_req = 0; }

        /* fb image -> swapchain, aspect-correct letterbox blit */
        barrier(s_cmd, s_swap_img[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        {
            VkClearColorValue black = {{0, 0, 0, 1}};
            VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(s_cmd, s_swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &black, 1, &rng);
        }
        int dw = (int)s_swap_ext.width, dh = (int)s_swap_ext.height;
        /* widescreen: a GE frame is drawn squeezed; movies and CPU-written frames are not */
        const float wsf = do_upload ? 1.0f : sdl3vk_widescreen_factor();
        int vw = dw, vh = (int)(dw * PSP_H / (PSP_W * wsf));
        if (vh > dh) { vh = dh; vw = (int)(dh * PSP_W * wsf / PSP_H); }
        int x0 = (dw - vw) / 2, y0 = (dh - vh) / 2;
        VkImageBlit blt = {0};
        blt.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blt.srcSubresource.layerCount = 1;
        blt.srcOffsets[1].x = srcw; blt.srcOffsets[1].y = srch; blt.srcOffsets[1].z = 1;
        blt.dstSubresource = blt.srcSubresource;
        blt.dstOffsets[0].x = x0;      blt.dstOffsets[0].y = y0;
        blt.dstOffsets[1].x = x0 + vw; blt.dstOffsets[1].y = y0 + vh; blt.dstOffsets[1].z = 1;
        blt.dstOffsets[0].z = 0;
        vkCmdBlitImage(s_cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       s_swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blt, VK_FILTER_LINEAR);
        barrier(s_cmd, s_swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        menu_record(s_swap_img[idx]);
        toast_record(s_swap_img[idx]);
        barrier(s_cmd, s_swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        vkEndCommandBuffer(s_cmd);

        VkPipelineStageFlags wait_st = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &s_sem_acq;
        si.pWaitDstStageMask = &wait_st;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &s_cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &s_sem_done;
        vkQueueSubmit(s_queue, 1, &si, s_fence);

        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &s_sem_done;
        pi.swapchainCount = 1;
        pi.pSwapchains = &s_swap;
        pi.pImageIndices = &idx;
        VkResult pr = vkQueuePresentKHR(s_queue, &pi);

        vkWaitForFences(s_dev, 1, &s_fence, VK_TRUE, UINT64_MAX);
        vkResetFences(s_dev, 1, &s_fence);
        shot_save();
        /* SR_PACELOG=1: every 5 s, how many frames reached the screen and how evenly */
        {
            static int on = -1;
            if (on < 0) on = getenv("SR_PACELOG") ? 1 : 0;
            if (on) {
                static Uint64 prev = 0, t0 = 0; static unsigned n = 0, slow = 0; static double maxgap = 0;
                const Uint64 now = SDL_GetTicksNS();
                if (prev) {
                    const double g = (double)(now - prev) / 1e6;
                    n++; if (g > 25.0) slow++; if (g > maxgap) maxgap = g;
                } else t0 = now;
                prev = now;
                if (now - t0 >= 5000000000ull) {
                    fprintf(stderr, "SCREEN 5s: %u frames, gaps >25ms %u, max %.0f ms\n", n, slow, maxgap);
                    n = slow = 0; maxgap = 0; t0 = now;
                }
            }
        }

        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
            vkDeviceWaitIdle(s_dev);
            create_swapchain();
        }
        break;
    }
    return 1;
}

int sdl3vk_present_rgba(const uint32_t *px) {
    memcpy(s_staging_map, px, PSP_W * PSP_H * 4);
    return present_common(s_fbimg, PSP_W, PSP_H, 1);
}

int sdl3vk_present_image(void *vk_image) {
    /* GE target (512x272 times the render scale): only the visible 480x272 region is shown */
    const int sc = sdl3vk_render_scale();
    return present_common((VkImage)vk_image, PSP_W * sc, PSP_H * sc, 0);
}

void sdl3vk_shutdown(void) {
    if (s_dev) vkDeviceWaitIdle(s_dev);
    destroy_swapchain();
    if (s_fbimg)       vkDestroyImage(s_dev, s_fbimg, NULL);
    if (s_fbimg_mem)   vkFreeMemory(s_dev, s_fbimg_mem, NULL);
    if (s_staging)     vkDestroyBuffer(s_dev, s_staging, NULL);
    if (s_staging_mem) vkFreeMemory(s_dev, s_staging_mem, NULL);
    if (s_sem_acq)     vkDestroySemaphore(s_dev, s_sem_acq, NULL);
    if (s_sem_done)    vkDestroySemaphore(s_dev, s_sem_done, NULL);
    if (s_fence)       vkDestroyFence(s_dev, s_fence, NULL);
    if (s_pool)        vkDestroyCommandPool(s_dev, s_pool, NULL);
    if (s_dev)         vkDestroyDevice(s_dev, NULL);
    if (s_surf)        vkDestroySurfaceKHR(s_inst, s_surf, NULL);
    if (s_inst)        vkDestroyInstance(s_inst, NULL);
    if (s_pad)         SDL_CloseGamepad(s_pad);
    if (s_win)         SDL_DestroyWindow(s_win);
    SDL_Quit();
}
