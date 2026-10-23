/* SR_CTRL=<port>: a localhost line-protocol control channel for test drivers (tools/drive.py),
 * the port's counterpart of PPSSPP's WebSocket debugger, so one script can drive both and
 * compare them. Off unless SR_CTRL is set.
 *
 *   btn <hexmask> <frames>     hold PSP buttons for that many vblanks (OR'd with other input)
 *   stick <x> <y> <frames>     hold the left stick (0..255, 128 = centre)
 *   shot <path>                save the next presented frame as PNG at <path>
 *   read <hexaddr> <len>       guest memory as hex
 *   vcount                     vblanks so far
 * Each command gets one reply line ("ok ..." / "err ..."). */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "recomp.h"

static volatile LONG s_btn_mask, s_btn_left;          /* frames left */
static volatile LONG s_stick_xy = -1, s_stick_left;
static volatile LONG64 s_vbl;                         /* vblank counter (sampled frames) */
volatile int g_drawlog_on = 0;                       /* "drawlog" (ge.c / gui.c) */
static volatile LONG s_presses;                       /* btn masks not yet seen by a modal dialog */

/* Modal dialogs (gui_dialog) read button presses here instead of from the game's pad. */
uint32_t ctrlsrv_take_presses(void) { return (uint32_t)InterlockedExchange(&s_presses, 0); }

/* Called once per controller sample (vblank). */
void ctrlsrv_apply(uint32_t *btn, uint8_t *lx, uint8_t *ly) {
    InterlockedIncrement64(&s_vbl);
    if (s_btn_left > 0) { *btn |= (uint32_t)s_btn_mask; InterlockedDecrement(&s_btn_left); }
    if (s_stick_left > 0 && s_stick_xy >= 0) {
        *lx = (uint8_t)((s_stick_xy >> 8) & 0xFF); *ly = (uint8_t)(s_stick_xy & 0xFF);
        InterlockedDecrement(&s_stick_left);
    }
}

static void reply(SOCKET c, const char *s) { send(c, s, (int)strlen(s), 0); }

static void handle(SOCKET c, char *line) {
    char cmd[16] = {0}, arg[300] = {0};
    unsigned long a = 0, b = 0, f = 0;
    char out[8300];
    if (sscanf(line, "%15s", cmd) != 1) { reply(c, "err empty\n"); return; }
    if (!strcmp(cmd, "btn") && sscanf(line, "%*s %lx %lu", &a, &f) == 2) {
        InterlockedExchange(&s_btn_mask, (LONG)a); InterlockedExchange(&s_btn_left, (LONG)f);
        InterlockedOr(&s_presses, (LONG)a);
        reply(c, "ok\n");
    } else if (!strcmp(cmd, "stick") && sscanf(line, "%*s %lu %lu %lu", &a, &b, &f) == 3) {
        InterlockedExchange(&s_stick_xy, (LONG)(((a & 0xFF) << 8) | (b & 0xFF)));
        InterlockedExchange(&s_stick_left, (LONG)f);
        reply(c, "ok\n");
    } else if (!strcmp(cmd, "shot") && sscanf(line, "%*s %299[^\r\n]", arg) == 1) {
        extern void sdl3vk_request_screenshot_path(const char *);
        sdl3vk_request_screenshot_path(arg);
        reply(c, "ok\n");
    } else if (!strcmp(cmd, "read") && sscanf(line, "%*s %lx %lu", &a, &b) == 2 && b <= 4096) {
        int n = snprintf(out, sizeof out, "ok ");
        for (unsigned long i = 0; i < b; i++) n += snprintf(out + n, sizeof out - n, "%02x", MEM_R8((uint32_t)a + (uint32_t)i));
        snprintf(out + n, sizeof out - n, "\n");
        reply(c, out);
    } else if (!strcmp(cmd, "mouse")) {   /* test: mouse camera motion "mouse <dx> <dy>" (pixels) */
        float dx = 0, dy = 0; sscanf(line, "%*s %f %f", &dx, &dy);
        extern void sdl3vk_mouse_test(float dx, float dy); sdl3vk_mouse_test(dx, dy); reply(c, "ok\n");
    } else if (!strcmp(cmd, "write") && sscanf(line, "%*s %lx %lx", &a, &b) == 2) {     /* test: one guest word */
        MEM_W32((uint32_t)a, (uint32_t)b); reply(c, "ok\n");
    } else if (!strcmp(cmd, "vmouse") && sscanf(line, "%*s %lu %lu", &a, &b) == 2) {   /* test: mouse button event */
        extern void sdl3vk_vmouse_button(int, int); sdl3vk_vmouse_button((int)a, (int)b); reply(c, "ok\n");
    } else if (!strcmp(cmd, "vbtn") && sscanf(line, "%*s %lu %lu", &a, &b) == 2) {   /* SR_VPAD: SDL gamepad button */
        extern void sdl3vk_vpad_button(int, int); sdl3vk_vpad_button((int)a, (int)b); reply(c, "ok\n");
    } else if (!strcmp(cmd, "vaxis") && sscanf(line, "%*s %lu %ld", &a, (long *)&b) == 2) {  /* SDL gamepad axis, -32768..32767 */
        extern void sdl3vk_vpad_axis(int, int); sdl3vk_vpad_axis((int)a, (int)(long)b); reply(c, "ok\n");
    } else if (!strcmp(cmd, "drawlog")) {   /* log every draw of the next whole frame (ge.c DRAW lines) */
        extern volatile int g_drawlog_on; g_drawlog_on = 2; reply(c, "ok\n");
    } else if (!strcmp(cmd, "vcount")) {
        snprintf(out, sizeof out, "ok %lld\n", (long long)s_vbl);
        reply(c, out);
    } else reply(c, "err bad command\n");
}

static DWORD WINAPI server(LPVOID p) {
    const int port = (int)(intptr_t)p;
    WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w)) return 0;
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET; sa.sin_port = htons((u_short)port); sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) || listen(ls, 1)) {
        fprintf(stderr, "ctrl: cannot listen on 127.0.0.1:%d\n", port); return 0;
    }
    fprintf(stderr, "ctrl: listening on 127.0.0.1:%d\n", port);
    for (;;) {
        SOCKET c = accept(ls, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
        char buf[1024]; int len = 0;
        for (;;) {
            int r = recv(c, buf + len, (int)sizeof(buf) - 1 - len, 0);
            if (r <= 0) break;
            len += r; buf[len] = 0;
            char *nl;
            while ((nl = strchr(buf, '\n'))) {
                *nl = 0; handle(c, buf);
                len -= (int)(nl + 1 - buf); memmove(buf, nl + 1, (size_t)len + 1);
            }
            if (len >= (int)sizeof(buf) - 1) len = 0;
        }
        closesocket(c);
    }
}

void ctrlsrv_start(void) {
    const char *e = getenv("SR_CTRL");
    if (!e || !atoi(e)) return;
    CreateThread(NULL, 0, server, (LPVOID)(intptr_t)atoi(e), 0, NULL);
}
