/* Update notification: at startup a background thread asks GitHub for the project's latest
 * release (https://api.github.com/repos/<SR_UPDATE_REPO>/releases/latest) and, when its tag is
 * a newer version than this build's (SR_VERSION, from the VERSION file), shows a notice in the
 * game. At most one check per day (update_check.txt next to the executable remembers when);
 * graphics.cfg update_check=0 (or the Esc menu) turns it off. Nothing is downloaded or sent
 * beyond the request itself. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#ifndef SR_VERSION
#define SR_VERSION "0.0.0"
#endif
#ifndef SR_UPDATE_REPO
#define SR_UPDATE_REPO "BasedYuki/the-3rd-birthday-recomp"
#endif

void gui_toast(const char *msg);
int sdl3vk_cfg_get(const char *key, int def);

static char s_latest[32];            /* newer release tag found, or empty */

const char *sr_version(void) { return SR_VERSION; }
const char *sr_update_available(void) { return s_latest[0] ? s_latest : NULL; }

/* "v1.2.3" / "1.2.3" -> comparable number; parts beyond 3 ignored */
static long long ver_num(const char *v) {
    while (*v && (*v < '0' || *v > '9')) v++;
    long long n = 0;
    for (int part = 0; part < 3; part++) {   /* missing parts count as 0 */
        long p = 0;
        if (*v >= '0' && *v <= '9') { char *e; p = strtol(v, &e, 10); v = e; }
        n = n * 100000 + p;
        if (*v == '.') v++;
    }
    return n;
}

static int fetch_latest(char *tag, int cap) {
    int ok = 0;
    HINTERNET ses = WinHttpOpen(L"ThirdBirthdayPC/" SR_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return 0;
    WinHttpSetTimeouts(ses, 5000, 5000, 5000, 8000);
    HINTERNET con = WinHttpConnect(ses, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    wchar_t path[200];
    const char *repo = getenv("SR_UPDATE_REPO");   /* tests: another repository */
    _snwprintf(path, 200, L"/repos/%hs/releases/latest", repo && *repo ? repo : SR_UPDATE_REPO);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;
    if (req && WinHttpSendRequest(req, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, NULL)) {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &sz, NULL);
        if (status == 200) {
            static char body[65536]; DWORD got = 0, n = 0;
            while (got < sizeof(body) - 1 && WinHttpReadData(req, body + got, (DWORD)(sizeof(body) - 1 - got), &n) && n) got += n;
            body[got] = 0;
            const char *t = strstr(body, "\"tag_name\"");
            if (t && (t = strchr(t + 10, '"')) != NULL) {
                t++;
                int i = 0;
                while (t[i] && t[i] != '"' && i < cap - 1) { tag[i] = t[i]; i++; }
                tag[i] = 0;
                ok = i > 0;
            }
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

static DWORD WINAPI check_thread(void *arg) {
    (void)arg;
    char tag[32];
    if (!fetch_latest(tag, sizeof(tag))) return 0;
    FILE *f = getenv("SR_UPDATE_FORCE") ? NULL : fopen("update_check.txt", "w");   /* tests leave no file */
    if (f) { fprintf(f, "%lld %s\n", (long long)time(NULL), tag); fclose(f); }
    if (ver_num(tag) > ver_num(SR_VERSION)) {
        snprintf(s_latest, sizeof(s_latest), "%s", tag);
        Sleep(15000);   /* after the boot logos, so the notice is seen */
        char msg[96];
        snprintf(msg, sizeof(msg), "Update available: %s  (you have v%s)", tag, SR_VERSION);
        gui_toast(msg);
        fprintf(stderr, "update: %s available (this build %s)\n", tag, SR_VERSION);
    }
    return 0;
}

/* gui_init: start the check (once a day at most, unless turned off or a test run) */
void sr_update_check_start(void) {
    const int force = getenv("SR_UPDATE_FORCE") != NULL;   /* tests: check now, whatever the rest says */
    if (!force && (getenv("SR_CTRL") || getenv("SR_NOUPDATECHECK"))) return;   /* scripted test runs */
    if (!sdl3vk_cfg_get("update_check", 1)) return;
    FILE *f = force ? NULL : fopen("update_check.txt", "r");
    if (f) {
        long long last = 0; char tag[32] = "";
        int n = fscanf(f, "%lld %31s", &last, tag);
        fclose(f);
        if (n >= 1 && time(NULL) - last < 24 * 3600) {
            /* checked today: still remind about a newer release found then */
            if (n == 2 && ver_num(tag) > ver_num(SR_VERSION)) {
                snprintf(s_latest, sizeof(s_latest), "%s", tag);
                fprintf(stderr, "update: %s available (this build %s)\n", tag, SR_VERSION);
            }
            return;
        }
    }
    HANDLE t = CreateThread(NULL, 0, check_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}
