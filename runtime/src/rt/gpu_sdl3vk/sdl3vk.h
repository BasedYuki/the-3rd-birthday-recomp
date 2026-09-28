/* SDL3 + Vulkan presentation backend (Phase 0 of the GPU renderer plan, see README.md).
 *
 * C ABI consumed by gui.c when built with -DSR_SDL3VK. Phase 0 presents the software-GE
 * framebuffer through a Vulkan swapchain (SDL3 owns the window and all input devices);
 * later phases move GE rasterization itself onto the GPU behind this same boundary.
 *
 * This is the project's own Vulkan backend, written from scratch; it does not reuse
 * PPSSPP's GPU. (An earlier PPSSPP-GPU bridge under src/rt/gpu_vk was a frozen experiment
 * and has been removed.) */
#ifndef SR_SDL3VK_H
#define SR_SDL3VK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create the SDL3 window and the Vulkan device/swapchain. Returns 1 on success, 0 on any
 * failure (caller falls back to the GDI path). */
int  sdl3vk_init(const char *title);

/* Present one 480x272 frame. px points to 480*272 little-endian BGRA words (the same
 * packing gui.c already produces for StretchDIBits: (r<<16)|(g<<8)|b). Pumps the SDL
 * event loop and samples input. Returns 0 when the user closed the window / pressed ESC,
 * 1 otherwise. */
int  sdl3vk_present_rgba(const uint32_t *px);

/* Present directly from a VkImage owned by the GPU rasterizer (ge_gpu.c). The image
 * must be 512x272 RGBA8 in TRANSFER_SRC_OPTIMAL layout; the visible 480x272 region is
 * blitted with the same letterboxing as sdl3vk_present_rgba. Same return semantics. */
int  sdl3vk_present_image(void *vk_image);

/* Internal render scale for GE targets (SR_SCALE or graphics.cfg render_scale, 0 = auto from
 * the display height). Fixed after the first call. */
int  sdl3vk_render_scale(void);

/* Show a short status message in the window corner for ms milliseconds (0 = 2.5 s). */
void sdl3vk_toast(const char *msg, int ms);

/* Settings menu support. Navigation events since the last call (bit mask), from the keyboard
 * (Esc toggles, arrows, Enter/Space/X, Backspace/Z) and the gamepad (Back+Start or Guide
 * toggles, D-pad, A, B). */
enum { SDL3VK_MENU_UP = 1, SDL3VK_MENU_DOWN = 2, SDL3VK_MENU_LEFT = 4, SDL3VK_MENU_RIGHT = 8,
       SDL3VK_MENU_OK = 16, SDL3VK_MENU_BACK = 32, SDL3VK_MENU_TOGGLE = 64 };
int  sdl3vk_menu_events(void);
/* Draw a centred menu panel over the game (n lines, sel highlighted; line 0 is the title).
 * n = 0 hides it. */
void sdl3vk_menu_show(const char *const *lines, int n, int sel);
/* Configured render scale in graphics.cfg (0 = auto) and writing it (takes effect on restart). */
int  sdl3vk_render_scale_cfg(void);
void sdl3vk_set_render_scale_cfg(int v);
int  sdl3vk_fullscreen(void);
void sdl3vk_set_fullscreen(int on);
/* F12: the next presented frame is saved as a PNG in screenshots/ (1 once saved). */
void sdl3vk_request_screenshot(void);

/* Input state captured by the last present (PSP sceCtrl button mask / analog stick). */
uint32_t sdl3vk_buttons(void);
void     sdl3vk_analog(uint8_t *lx, uint8_t *ly);
void     sdl3vk_rstick(float *rx, float *ry);   /* right stick, -1..1 */
int      sdl3vk_sens_steps(void);   /* camera sensitivity key presses since last call (+ up, - down) */
int      sdl3vk_cam_reset(void);    /* 1 once per camera-reset press (R3 / O) */
int      sdl3vk_pad_present(void);

/* Vulkan objects shared with the Phase-1 GPU rasterizer (ge_gpu.c). Handles are typed
 * void* here so this header stays vulkan.h-free; they are the real VkInstance etc.
 * Returns 0 until sdl3vk_init() has succeeded. */
typedef struct Sdl3VkInfo {
    void    *instance, *physical, *device, *queue;
    uint32_t queue_family;
} Sdl3VkInfo;
int sdl3vk_get_vk(Sdl3VkInfo *out);

void sdl3vk_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
