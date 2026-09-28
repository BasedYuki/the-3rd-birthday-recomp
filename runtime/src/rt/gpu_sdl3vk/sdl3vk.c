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
static int      s_shot_req = 0;                      /* F12: save the next frame */
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
static char     s_toast_msg[96];
static Uint64   s_toast_until = 0;            /* SDL_GetTicks() deadline */
static char     s_toast_drawn[96];
static int      s_toast_w = 0, s_toast_h = 0, s_toast_px = 0;
static VkBuffer s_toast_buf; static VkDeviceMemory s_toast_mem; static void *s_toast_map;

void sdl3vk_toast(const char *msg, int ms) {
    SDL_strlcpy(s_toast_msg, msg ? msg : "", sizeof(s_toast_msg));
    s_toast_until = SDL_GetTicks() + (Uint64)(ms > 0 ? ms : 2500);
}

/* Render the toast bitmap for a font of `px` pixels. Returns 0 if unavailable. */
static int toast_render(int px) {
    if (!strcmp(s_toast_drawn, s_toast_msg) && s_toast_px == px && s_toast_w) return 1;
    if (!s_toast_buf) {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = (VkDeviceSize)TOAST_MAX_W * TOAST_MAX_H * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateBuffer(s_dev, &bci, NULL, &s_toast_buf) != VK_SUCCESS) return 0;
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(s_dev, s_toast_buf, &mr);
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = find_mem_type(mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(s_dev, &mai, NULL, &s_toast_mem) != VK_SUCCESS) return 0;
        vkBindBufferMemory(s_dev, s_toast_buf, s_toast_mem, 0);
        vkMapMemory(s_dev, s_toast_mem, 0, VK_WHOLE_SIZE, 0, &s_toast_map);
    }
    HDC dc = CreateCompatibleDC(NULL);
    HFONT font = CreateFontA(-px, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Segoe UI");
    HGDIOBJ oldf = SelectObject(dc, font);
    SIZE ts; GetTextExtentPoint32A(dc, s_toast_msg, (int)strlen(s_toast_msg), &ts);
    int pad = px * 2 / 3, bar = px / 5 > 2 ? px / 5 : 2;
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
        TextOutA(dc, bar + pad, pad / 2, s_toast_msg, (int)strlen(s_toast_msg));
        GdiFlush();
        uint32_t *dst = (uint32_t *)s_toast_map;
        for (int i = 0; i < w * h; i++) dst[i] = p[i] | 0xFF000000u;   /* BGRA, opaque */
        SelectObject(dc, oldb);
        DeleteObject(bm);
    }
    SelectObject(dc, oldf);
    DeleteObject(font);
    DeleteDC(dc);
    if (!ok) return 0;
    s_toast_w = w; s_toast_h = h; s_toast_px = px;
    SDL_strlcpy(s_toast_drawn, s_toast_msg, sizeof(s_toast_drawn));
    return 1;
}

/* Record the toast copy onto a swapchain image in TRANSFER_DST layout. */
static void toast_record(VkImage dst) {
    if (!s_toast_msg[0] || SDL_GetTicks() >= s_toast_until) return;
    int dh = (int)s_swap_ext.height, dw = (int)s_swap_ext.width;
    int px = dh / 28; if (px < 14) px = 14; if (px > 64) px = 64;
    if (!toast_render(px)) return;
    int margin = px;
    if (s_toast_w + margin > dw || s_toast_h + margin > dh) return;
    VkBufferImageCopy c = {0};
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.bufferRowLength = (uint32_t)s_toast_w;
    c.imageOffset.x = dw - s_toast_w - margin;
    c.imageOffset.y = dh - s_toast_h - margin;
    c.imageExtent.width = (uint32_t)s_toast_w; c.imageExtent.height = (uint32_t)s_toast_h; c.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_cmd, s_toast_buf, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

/* ---- settings menu panel ----------------------------------------------------------------
 * Same technique as the toast: GDI text into a bitmap, copied onto the swapchain image after
 * the frame blit, centred. Line 0 is the title; the selected line gets an orange marker. */
#define MENU_MAX_LINES 16
#define MENU_MAX_W 1600
#define MENU_MAX_H 1100
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
        GetTextExtentPoint32A(dc, s_menu_lines[i], (int)strlen(s_menu_lines[i]), &ts);
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
            TextOutA(dc, bar + pad, y, s_menu_lines[i], (int)strlen(s_menu_lines[i]));
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

int  sdl3vk_fullscreen(void) { return s_win && (SDL_GetWindowFlags(s_win) & SDL_WINDOW_FULLSCREEN) != 0; }
void sdl3vk_set_fullscreen(int on) { if (s_win) SDL_SetWindowFullscreen(s_win, on ? true : false); }
void sdl3vk_request_screenshot(void) { s_shot_req = 1; }

/* ---- screenshots (F12) -------------------------------------------------------------------
 * The visible game image (at the internal resolution, without the menu or toasts) is copied to
 * a host buffer during the present and saved as PNG through GDI+ into screenshots/. */
typedef struct { UINT32 GdiplusVersion; void *DebugEventCallback; BOOL SuppressBackgroundThread; BOOL SuppressExternalCodecs; } GpStartupIn;
int __stdcall GdiplusStartup(ULONG_PTR *token, const GpStartupIn *input, void *output);
int __stdcall GdipCreateBitmapFromScan0(INT w, INT h, INT stride, INT format, BYTE *scan0, void **bitmap);
int __stdcall GdipSaveImageToFile(void *image, const WCHAR *filename, const CLSID *encoder, const void *params);
int __stdcall GdipDisposeImage(void *image);

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
    CreateDirectoryA("screenshots", NULL);
    SYSTEMTIME st; GetLocalTime(&st);
    WCHAR name[128];
    _snwprintf(name, 128, L"screenshots\\3rd_Birthday_%04u-%02u-%02u_%02u-%02u-%02u.png",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    name[127] = 0;
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
    sdl3vk_toast(msg, 2000);
}

/* ---- render scale ------------------------------------------------------------------ */

/* Internal resolution multiplier for the GPU renderer (1 = PSP native 480x272, 4 = 1920x1088).
 * From SR_SCALE, else graphics.cfg in the working directory:
 *   render_scale=0   (0 = match the display: the smallest scale that covers its height)
 * A missing graphics.cfg is written with the default so it can be edited. */
int sdl3vk_render_scale_cfg(void) {
    int v = 0;
    FILE *f = fopen("graphics.cfg", "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) { int x; if (sscanf(line, "render_scale=%d", &x) == 1) v = x; }
        fclose(f);
    }
    return v;
}

void sdl3vk_set_render_scale_cfg(int v) {
    FILE *f = fopen("graphics.cfg", "w");
    if (!f) return;
    fprintf(f, "render_scale=%d\n", v);
    fclose(f);
}

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
    s_win = SDL_CreateWindow(title ? title : "PSP Recomp",
                             PSP_W * wk, PSP_H * wk,
                             SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!s_win) {
        fprintf(stderr, "sdl3vk: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 0;
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
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
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
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_EVENT_QUIT: *quit = 1; break;
        case SDL_EVENT_KEY_DOWN:
            if (!ev.key.repeat && (ev.key.key == SDLK_F11 ||
                                   (ev.key.key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT)))) {
                SDL_SetWindowFullscreen(s_win, !(SDL_GetWindowFlags(s_win) & SDL_WINDOW_FULLSCREEN));
                break;
            }
            if (!ev.key.repeat && ev.key.key == SDLK_F12) s_shot_req = 1;
            /* settings menu: Esc opens/closes it; arrows, Enter/Space/X and Backspace drive it */
            switch (ev.key.key) {
            case SDLK_ESCAPE:    if (!ev.key.repeat) s_menu_ev |= SDL3VK_MENU_TOGGLE; break;
            case SDLK_UP:    case SDLK_KP_8: case SDLK_W: s_menu_ev |= SDL3VK_MENU_UP; break;
            case SDLK_DOWN:  case SDLK_KP_2: case SDLK_S: s_menu_ev |= SDL3VK_MENU_DOWN; break;
            case SDLK_LEFT:  case SDLK_KP_4: case SDLK_A: s_menu_ev |= SDL3VK_MENU_LEFT; break;
            case SDLK_RIGHT: case SDLK_KP_6: case SDLK_D: s_menu_ev |= SDL3VK_MENU_RIGHT; break;
            case SDLK_RETURN: case SDLK_SPACE: case SDLK_X:
                if (!ev.key.repeat && !(ev.key.mod & SDL_KMOD_ALT)) s_menu_ev |= SDL3VK_MENU_OK;
                break;
            case SDLK_BACKSPACE: case SDLK_Z: if (!ev.key.repeat) s_menu_ev |= SDL3VK_MENU_BACK; break;
            default: break;
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            switch (ev.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP:    s_menu_ev |= SDL3VK_MENU_UP; break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  s_menu_ev |= SDL3VK_MENU_DOWN; break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  s_menu_ev |= SDL3VK_MENU_LEFT; break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: s_menu_ev |= SDL3VK_MENU_RIGHT; break;
            case SDL_GAMEPAD_BUTTON_SOUTH:      s_menu_ev |= SDL3VK_MENU_OK; break;
            case SDL_GAMEPAD_BUTTON_EAST:       s_menu_ev |= SDL3VK_MENU_BACK; break;
            case SDL_GAMEPAD_BUTTON_GUIDE:      s_menu_ev |= SDL3VK_MENU_TOGGLE; break;
            case SDL_GAMEPAD_BUTTON_START:      /* Back (View) + Start (Menu): settings menu */
            case SDL_GAMEPAD_BUTTON_BACK:
                if (s_pad && SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_START) &&
                    SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_BACK))
                    s_menu_ev |= SDL3VK_MENU_TOGGLE;
                break;
            default: break;
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

    uint32_t b = 0;
    const bool *k = SDL_GetKeyboardState(NULL);
    /* Keyboard: WASD walk (the PSP analog stick), the Z X C V row is the face buttons,
     * Q / E the shoulders, arrows the D-pad, I J K L the camera. */
    if (k[SDL_SCANCODE_RETURN] && !(SDL_GetModState() & SDL_KMOD_ALT)) b |= 0x0008;   /* START (Alt+Enter: fullscreen) */
    if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) b |= 0x0001; /* SELECT */
    if (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_SPACE]) b |= 0x4000;   /* CROSS   */
    if (k[SDL_SCANCODE_Z]) b |= 0x2000;                            /* CIRCLE  */
    if (k[SDL_SCANCODE_C]) b |= 0x8000;                            /* SQUARE  */
    if (k[SDL_SCANCODE_V]) b |= 0x1000;                            /* TRIANGLE*/
    if (k[SDL_SCANCODE_Q]) b |= 0x0100;                            /* L       */
    if (k[SDL_SCANCODE_E]) b |= 0x0200;                            /* R       */
    if (k[SDL_SCANCODE_UP])    b |= 0x0010;
    if (k[SDL_SCANCODE_DOWN])  b |= 0x0040;
    if (k[SDL_SCANCODE_LEFT])  b |= 0x0080;
    if (k[SDL_SCANCODE_RIGHT]) b |= 0x0020;

    uint8_t lx = 128, ly = 128;
    {
        int kx = (k[SDL_SCANCODE_D] ? 1 : 0) - (k[SDL_SCANCODE_A] ? 1 : 0);
        int ky = (k[SDL_SCANCODE_S] ? 1 : 0) - (k[SDL_SCANCODE_W] ? 1 : 0);
        if (kx || ky) {
            const int m = (kx && ky) ? 90 : 127;             /* keep diagonals at full length */
            lx = (uint8_t)(128 + kx * m);
            ly = (uint8_t)(128 + ky * m);
        }
    }
    /* Right stick for the camera: I/J/K/L on the keyboard, the gamepad's right stick below. */
    float rx = 0.0f, ry = 0.0f;
    if (k[SDL_SCANCODE_J]) rx -= 1.0f;
    if (k[SDL_SCANCODE_L]) rx += 1.0f;
    if (k[SDL_SCANCODE_I]) ry -= 1.0f;
    if (k[SDL_SCANCODE_K]) ry += 1.0f;
    { int sk = k[SDL_SCANCODE_EQUALS] ? 1 : k[SDL_SCANCODE_MINUS] ? -1 : 0;
      if (sk && sk != s_sens_prev) s_sens_steps += sk;
      s_sens_prev = sk; }
    {   /* camera reset: right stick click (R3) or O */
        int cr = k[SDL_SCANCODE_O] || (s_pad && SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK));
        if (cr && !s_camreset_prev) s_camreset = 1;
        s_camreset_prev = cr;
    }
    s_pad_present = s_pad != NULL;
    if (s_pad) {
        #define PB(sdlb, bit) do { if (SDL_GetGamepadButton(s_pad, sdlb)) b |= (bit); } while (0)
        PB(SDL_GAMEPAD_BUTTON_SOUTH, 0x4000);          /* CROSS    */
        PB(SDL_GAMEPAD_BUTTON_EAST,  0x2000);          /* CIRCLE   */
        PB(SDL_GAMEPAD_BUTTON_WEST,  0x8000);          /* SQUARE   */
        PB(SDL_GAMEPAD_BUTTON_NORTH, 0x1000);          /* TRIANGLE */
        PB(SDL_GAMEPAD_BUTTON_START, 0x0008);
        PB(SDL_GAMEPAD_BUTTON_BACK,  0x0001);
        PB(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  0x0100);
        PB(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200);
        PB(SDL_GAMEPAD_BUTTON_DPAD_UP,    0x0010);
        PB(SDL_GAMEPAD_BUTTON_DPAD_DOWN,  0x0040);
        PB(SDL_GAMEPAD_BUTTON_DPAD_LEFT,  0x0080);
        PB(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0020);
        #undef PB
        if (SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)  > 8192) b |= 0x0100;
        if (SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8192) b |= 0x0200;
        int ax = SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTX);
        int ay = SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTY);
        if (ax < -7849 || ax > 7849) lx = (uint8_t)((ax + 32768) * 255 / 65535);
        if (ay < -7849 || ay > 7849) ly = (uint8_t)((ay + 32768) * 255 / 65535);
        /* Right stick with a radial deadzone, rescaled so movement starts at 0 past it. */
        float gx = SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
        float gy = SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f;
        float mag = sqrtf(gx * gx + gy * gy);
        const float dz = 0.2f;
        if (mag > dz) {
            float sc = (mag > 1.0f ? 1.0f : (mag - dz) / (1.0f - dz)) / mag;
            rx += gx * sc; ry += gy * sc;
        }
    }
    s_rx = rx < -1.0f ? -1.0f : rx > 1.0f ? 1.0f : rx;
    s_ry = ry < -1.0f ? -1.0f : ry > 1.0f ? 1.0f : ry;
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
        int vw = dw, vh = dw * PSP_H / PSP_W;
        if (vh > dh) { vh = dh; vw = dh * PSP_W / PSP_H; }
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
