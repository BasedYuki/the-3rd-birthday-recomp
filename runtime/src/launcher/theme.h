/* Launcher look: a background composed once into a DIB - the game's own PSP background image
 * (PSP_GAME/PIC1.PNG, read from the player's disc image; nothing of the game ships with the
 * launcher), darkened panels for the settings, blood splatter and drips drawn in code - and
 * dark-red buttons. Included by launcher.c. */
#include <objidl.h>
#include <uxtheme.h>
#include <stdint.h>

/* GDI+ flat API (PNG from memory, window icon) */
typedef struct { UINT32 GdiplusVersion; void *DebugEventCallback; BOOL SuppressBackgroundThread; BOOL SuppressExternalCodecs; } ThGpStartup;
int __stdcall GdiplusStartup(ULONG_PTR *token, const ThGpStartup *input, void *output);
int __stdcall GdipCreateBitmapFromStream(IStream *stream, void **bitmap);
int __stdcall GdipGetImageWidth(void *image, UINT *w);
int __stdcall GdipGetImageHeight(void *image, UINT *h);
typedef struct { INT X, Y, Width, Height; } ThGpRect;
typedef struct { UINT Width, Height; INT Stride; INT PixelFormat; void *Scan0; UINT_PTR Reserved; } ThGpData;
int __stdcall GdipBitmapLockBits(void *bitmap, const ThGpRect *rect, UINT flags, INT format, ThGpData *data);
int __stdcall GdipBitmapUnlockBits(void *bitmap, ThGpData *data);
int __stdcall GdipCreateHICONFromBitmap(void *bitmap, HICON *icon);
int __stdcall GdipDisposeImage(void *image);
IStream *__stdcall SHCreateMemStream(const BYTE *p, UINT n);

#define TH_TEXT     RGB(236, 226, 226)
#define TH_DIM      RGB(190, 170, 170)
#define TH_TITLE    RGB(235, 70, 70)
#define TH_FIELD    RGB(22, 8, 10)

static HBITMAP s_bg; static uint32_t *s_bgpx; static int s_bgw, s_bgh;
static HBRUSH s_bgbrush, s_fieldbrush;
static ULONG_PTR s_gdip;

/* A file from the disc image (ISO9660), malloc'd, or NULL. */
static unsigned char *iso_file(const char *iso, const char *path, unsigned *size) {
    FILE *f = fopen(iso, "rb");
    if (!f) return NULL;
    unsigned char pvd[2048];
    unsigned char *out = NULL;
    if (_fseeki64(f, 16 * 2048, SEEK_SET) || fread(pvd, 1, 2048, f) != 2048 || memcmp(pvd + 1, "CD001", 5)) { fclose(f); return NULL; }
    unsigned lba = pvd[158] | pvd[159] << 8 | pvd[160] << 16 | (unsigned)pvd[161] << 24;
    unsigned len = pvd[166] | pvd[167] << 8 | pvd[168] << 16 | (unsigned)pvd[169] << 24;
    char part[64]; const char *p = path;
    while (*p) {
        int n = 0; while (*p && *p != '/' && n < 63) part[n++] = *p++;
        part[n] = 0; if (*p == '/') p++;
        if (len > 1 << 20) break;
        unsigned char *dir = malloc(len);
        if (!dir || _fseeki64(f, (long long)lba * 2048, SEEK_SET) || fread(dir, 1, len, f) != len) { free(dir); break; }
        int found = 0;
        for (unsigned o = 0; o < len;) {
            unsigned char L = dir[o];
            if (!L) { o = (o / 2048 + 1) * 2048; continue; }
            int nl = dir[o + 32]; char nm[64]; int k = 0;
            for (; k < nl && k < 63 && dir[o + 33 + k] != ';'; k++) nm[k] = (char)dir[o + 33 + k];
            nm[k] = 0;
            if (!_stricmp(nm, part)) {
                lba = dir[o + 2] | dir[o + 3] << 8 | dir[o + 4] << 16 | (unsigned)dir[o + 5] << 24;
                len = dir[o + 10] | dir[o + 11] << 8 | dir[o + 12] << 16 | (unsigned)dir[o + 13] << 24;
                found = 1; break;
            }
            o += L;
        }
        free(dir);
        if (!found) { fclose(f); return NULL; }
        if (!*p) {
            out = malloc(len);
            if (out && (_fseeki64(f, (long long)lba * 2048, SEEK_SET) || fread(out, 1, len, f) != len)) { free(out); out = NULL; }
            if (out) *size = len;
        }
    }
    fclose(f);
    return out;
}

/* Decode a PNG (bytes) to BGRA pixels; also returns the GDI+ bitmap if wanted. */
static uint32_t *png_decode(const unsigned char *png, unsigned n, int *w, int *h, void **keep) {
    if (!s_gdip) { ThGpStartup in = { 1, NULL, FALSE, FALSE }; if (GdiplusStartup(&s_gdip, &in, NULL)) return NULL; }
    IStream *st = SHCreateMemStream(png, n);
    if (!st) return NULL;
    void *bmp = NULL; uint32_t *px = NULL;
    if (!GdipCreateBitmapFromStream(st, &bmp) && bmp) {
        UINT iw = 0, ih = 0; GdipGetImageWidth(bmp, &iw); GdipGetImageHeight(bmp, &ih);
        ThGpRect r = { 0, 0, (INT)iw, (INT)ih }; ThGpData d;
        if (iw && ih && !GdipBitmapLockBits(bmp, &r, 1, 0x0026200A, &d)) {
            px = malloc((size_t)iw * ih * 4);
            if (px) for (UINT y = 0; y < ih; y++) memcpy(px + y * iw, (char *)d.Scan0 + (ptrdiff_t)y * d.Stride, iw * 4);
            GdipBitmapUnlockBits(bmp, &d);
            *w = (int)iw; *h = (int)ih;
        }
        if (keep) *keep = bmp; else GdipDisposeImage(bmp);
    }
    st->lpVtbl->Release(st);
    return px;
}

static inline void blend(uint32_t *p, int r, int g, int b, int a) {   /* a 0..255 */
    if (a <= 0) return;
    if (a > 255) a = 255;
    const uint32_t c = *p;
    const int R = (c >> 16) & 255, G = (c >> 8) & 255, B = c & 255;
    *p = 0xFF000000u | (uint32_t)((R + (r - R) * a / 255) << 16) | (uint32_t)((G + (g - G) * a / 255) << 8) | (uint32_t)(B + (b - B) * a / 255);
}
static void fill_rect(int x0, int y0, int x1, int y1, int r, int g, int b, int a) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > s_bgw) x1 = s_bgw; if (y1 > s_bgh) y1 = s_bgh;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) blend(&s_bgpx[y * s_bgw + x], r, g, b, a);
}
/* soft-edged disc */
static void blob(float cx, float cy, float rad, int r, int g, int b, int a) {
    const int x0 = (int)(cx - rad - 2), x1 = (int)(cx + rad + 2), y0 = (int)(cy - rad - 2), y1 = (int)(cy + rad + 2);
    for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < s_bgh; y++)
        for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < s_bgw; x++) {
            const float dx = x - cx, dy = y - cy, d = sqrtf(dx * dx + dy * dy);
            const float e = rad - d;                         /* 1 px soft edge */
            if (e <= -1.0f) continue;
            const float k = e >= 0.0f ? 1.0f : 1.0f + e;
            blend(&s_bgpx[y * s_bgw + x], r, g, b, (int)(a * k));
        }
}
static unsigned s_seed;
static float rnd(void) { s_seed = s_seed * 1103515245u + 12345u; return (float)((s_seed >> 8) & 0xFFFF) / 65535.0f; }

/* a splatter: a core, satellites thrown outward along a direction, small specks */
static void splatter(float cx, float cy, float size, float dir) {
    const int R = 120, G = 4, B = 10;
    blob(cx, cy, size, R - 30, 0, 4, 235);
    for (int i = 0; i < 7; i++) blob(cx + (rnd() - 0.5f) * size * 1.4f, cy + (rnd() - 0.5f) * size * 1.4f, size * (0.35f + rnd() * 0.4f), R, G, B, 220);
    for (int i = 0; i < 26; i++) {
        const float a = dir + (rnd() - 0.5f) * 1.6f, d = size * (1.2f + rnd() * 3.2f);
        const float r = size * 0.05f + rnd() * size * 0.16f;
        blob(cx + cosf(a) * d, cy + sinf(a) * d, r, R + 20, G, B, 210);
        if (rnd() < 0.35f) blob(cx + cosf(a) * d * 1.25f, cy + sinf(a) * d * 1.25f, r * 0.5f, R, G, B, 200);
    }
    for (int i = 0; i < 20; i++) {
        const float a = rnd() * 6.283f, d = size * (1.0f + rnd() * 4.5f);
        blob(cx + cosf(a) * d, cy + sinf(a) * d, 0.6f + rnd() * 1.6f, R, G, B, 190);
    }
}
/* drips running down from the top edge */
static void drips(float s) {
    for (int i = 0; i < 26; i++) {
        const float x = rnd() * s_bgw, w = (2.0f + rnd() * 5.0f) * s, len = (8.0f + rnd() * rnd() * 110.0f) * s;
        for (float y = 0; y < len; y += 1.0f) {
            const float taper = 1.0f - 0.45f * (y / len);
            blob(x, y, w * 0.5f * taper, 110, 0, 8, 230);
        }
        blob(x, len, w * 0.85f, 110, 0, 8, 235);           /* the drop at the end */
        blob(x - w * 0.2f, len - w * 0.2f, w * 0.25f, 200, 60, 60, 90);   /* highlight */
    }
    fill_rect(0, 0, s_bgw, (int)(5 * s), 95, 0, 6, 235);    /* the bleeding edge */
}

typedef struct { int x, y, w, h; const char *title; } Panel;

/* Compose the background for a client area of cw x ch pixels. */
static void theme_compose(int cw, int ch, const char *iso, const Panel *panels, int npanels, float s, int ox) {
    if (s_bg) { DeleteObject(s_bg); s_bg = NULL; }
    if (s_bgbrush) { DeleteObject(s_bgbrush); s_bgbrush = NULL; }
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = cw; bi.bmiHeader.biHeight = -ch;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    s_bg = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!s_bg) return;
    s_bgpx = bits; s_bgw = cw; s_bgh = ch;
    /* base: deep red-black gradient */
    for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) {
        const int v = 18 + 30 * (ch - y) / ch;
        s_bgpx[y * cw + x] = 0xFF000000u | (uint32_t)(v << 16) | (uint32_t)((v / 6) << 8) | (uint32_t)(v / 5);
    }
    /* the game's PSP background (from the player's disc), Aya on the left */
    unsigned n = 0; int iw = 0, ih = 0;
    unsigned char *png = iso && *iso ? iso_file(iso, "PSP_GAME/PIC1.PNG", &n) : NULL;
    uint32_t *img = png ? png_decode(png, n, &iw, &ih, NULL) : NULL;
    free(png);
    if (img && iw == 480 && ih == 272) {
        /* take out the small title and the copyright line (redrawn sharp below): the title rows are
           filled from the rows above and below; over Aya only the light text pixels are replaced */
        static unsigned char text[29][242];                    /* light pixels near Aya, grown by 2 for the outline */
        memset(text, 0, sizeof(text));
        for (int y = 116; y <= 144; y++) for (int x = 238; x < 300; x++) {
            const uint32_t q = img[y * iw + x];
            const int r = (q >> 16) & 255, g = (q >> 8) & 255, b = q & 255;
            if (g > 120 && b > 110 && r - b < 90)
                for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) {
                    const int yy = y + dy - 116, xx = x + dx - 238;
                    if (yy >= 0 && yy < 29 && xx >= 0 && xx < 242) text[yy][xx] = 1;
                }
        }
        for (int y = 116; y <= 144; y++) for (int x = 238; x < iw; x++) {
            uint32_t *p = &img[y * iw + x];
            if (x < 300 && !text[y - 116][x - 238]) continue;
            const uint32_t u = img[115 * iw + x], v = img[145 * iw + x]; const float t = (y - 115) / 30.0f;
            uint32_t o = 0xFF000000u;
            for (int sh = 0; sh < 24; sh += 8) o |= (uint32_t)(((u >> sh) & 255) * (1 - t) + ((v >> sh) & 255) * t) << sh;
            *p = o;
        }
        ih = 246;                                              /* the copyright line starts below */
    }
    if (img) {
        const float k = (float)ch / ih;
        const int dx0 = (int)(ox * 0.5f - 245.0f * k);      /* Aya (x 245 of 480) centred left of the panels */
        for (int y = 0; y < ch; y++) {
            const float sy = (y + 0.5f) / k - 0.5f;
            const int y0 = sy < 0 ? 0 : (int)sy, y1 = y0 + 1 < ih ? y0 + 1 : y0; const float fy = sy - y0;
            for (int x = 0; x < cw; x++) {
                const float sx = (x - dx0 + 0.5f) / k - 0.5f;
                if (sx < 0 || sx > iw - 1) continue;
                const int x0 = (int)sx, x1 = x0 + 1 < iw ? x0 + 1 : x0; const float fx = sx - x0;
                const uint32_t a = img[y0 * iw + x0], b = img[y0 * iw + x1], c = img[y1 * iw + x0], d = img[y1 * iw + x1];
                uint32_t o = 0xFF000000u;
                for (int sh = 0; sh < 24; sh += 8) {
                    const float t = ((a >> sh) & 255) * (1 - fx) * (1 - fy) + ((b >> sh) & 255) * fx * (1 - fy)
                                  + ((c >> sh) & 255) * (1 - fx) * fy + ((d >> sh) & 255) * fx * fy;
                    o |= (uint32_t)(t + 0.5f) << sh;
                }
                /* fade the image out towards the panels */
                const float edge = (float)(dx0 + iw * k);
                const float fade = x > edge - 120 * s ? (edge - x) / (120 * s) : 1.0f;
                uint32_t *p = &s_bgpx[y * cw + x];
                const uint32_t base = *p;
                *p = o; if (fade < 1.0f) blend(p, (base >> 16) & 255, (base >> 8) & 255, base & 255, (int)((1.0f - fade) * 255));
            }
        }
        free(img);
    }
    /* darker behind the settings, and a vignette */
    for (int x = 0; x < cw; x++) {
        const float t = (x - (ox - 60 * s)) / (90 * s);
        const int a = t <= 0 ? 0 : t >= 1 ? 170 : (int)(170 * t);
        if (a) fill_rect(x, 0, x + 1, ch, 8, 2, 4, a);
    }
    for (int y = 0; y < ch; y++) {
        const int a = y > ch - 140 * s ? (int)(150 * (y - (ch - 140 * s)) / (140 * s)) : 0;
        if (a) fill_rect(0, y, cw, y + 1, 0, 0, 0, a);
    }
    /* blood */
    s_seed = 0x3B1D;
    drips(s);
    splatter(ox - 40 * s, ch - 70 * s, 26 * s, -0.6f);
    splatter(cw - 30 * s, 20 * s, 18 * s, 2.4f);
    splatter(ox * 0.18f, 60 * s, 14 * s, 0.8f);
    HDC dc = CreateCompatibleDC(NULL);
    HGDIOBJ ob = SelectObject(dc, s_bg);
    SetBkMode(dc, TRANSPARENT);
    /* the title, PSP-cover style, and the art's credit line */
    if (iso && *iso) {
        HFONT f = CreateFontA(-(int)(30 * s), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Courier New");
        HGDIOBJ of = SelectObject(dc, f);
        static const char *part[] = { "[The ", "3", "rd Birthday]" };
        SIZE sz; int tw = 0;
        for (int i = 0; i < 3; i++) { GetTextExtentPoint32A(dc, part[i], (int)strlen(part[i]), &sz); tw += sz.cx; }
        int x = (ox - tw) / 2; const int y = ch - (int)(96 * s);
        for (int i = 0; i < 3; i++) {
            SetTextColor(dc, RGB(10, 0, 2));                                   /* dark outline */
            for (int o = 0; o < 9; o++) if (o != 4) TextOutA(dc, x + (o % 3 - 1) * 2, y + (o / 3 - 1) * 2, part[i], (int)strlen(part[i]));
            SetTextColor(dc, i == 1 ? RGB(255, 40, 50) : RGB(245, 240, 240));
            TextOutA(dc, x, y, part[i], (int)strlen(part[i]));
            GetTextExtentPoint32A(dc, part[i], (int)strlen(part[i]), &sz); x += sz.cx;
        }
        SelectObject(dc, of); DeleteObject(f);
        f = CreateFontA(-(int)(11 * s), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
        of = SelectObject(dc, f);
        static const char credit[] = "Art from your disc  \xB7  \xA9 2010, 2011 SQUARE ENIX  \xB7  Character design: Tetsuya Nomura";
        GetTextExtentPoint32A(dc, credit, (int)strlen(credit), &sz);
        SetTextColor(dc, RGB(150, 120, 120));
        TextOutA(dc, (ox - sz.cx) / 2, ch - (int)(26 * s), credit, (int)strlen(credit));
        SelectObject(dc, of); DeleteObject(f);
    }
    /* panels */
    for (int i = 0; i < npanels; i++) {
        const Panel *pn = &panels[i];
        const int x = (int)(pn->x * s), y = (int)(pn->y * s), w = (int)(pn->w * s), h = (int)(pn->h * s);
        fill_rect(x, y, x + w, y + h, 14, 4, 6, 205);
        fill_rect(x, y, x + w, y + (int)(2 * s), 170, 16, 24, 230);          /* red top edge */
        fill_rect(x, y + h - 1, x + w, y + h, 90, 10, 14, 180);
        if (pn->title) {
            HFONT f = CreateFontA(-(int)(15 * s), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
            HGDIOBJ of = SelectObject(dc, f);
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, TH_TITLE);
            TextOutA(dc, x + (int)(10 * s), y + (int)(5 * s), pn->title, (int)strlen(pn->title));
            SelectObject(dc, of); DeleteObject(f);
        }
    }
    GdiFlush();
    SelectObject(dc, ob); DeleteDC(dc);
    for (int i = 0; i < cw * ch; i++) s_bgpx[i] |= 0xFF000000u;
    s_bgbrush = CreatePatternBrush(s_bg);
    if (!s_fieldbrush) s_fieldbrush = CreateSolidBrush(TH_FIELD);
}

/* window icon from the disc's ICON0.PNG */
static HICON theme_icon(const char *iso) {
    unsigned n = 0; HICON ic = NULL; void *bmp = NULL; int w, h;
    unsigned char *png = iso && *iso ? iso_file(iso, "PSP_GAME/ICON0.PNG", &n) : NULL;
    if (!png) return NULL;
    uint32_t *px = png_decode(png, n, &w, &h, &bmp);
    free(px); free(png);
    if (bmp) { GdipCreateHICONFromBitmap(bmp, &ic); GdipDisposeImage(bmp); }
    return ic;
}

/* dark-red push button */
static void theme_draw_button(const DRAWITEMSTRUCT *d, HFONT font, int big) {
    const int down = (d->itemState & ODS_SELECTED) != 0, dis = (d->itemState & ODS_DISABLED) != 0;
    const int hot = (d->itemState & ODS_FOCUS) != 0;
    RECT r = d->rcItem;
    HBRUSH fill = CreateSolidBrush(dis ? RGB(40, 24, 26) : down ? RGB(150, 12, 22) : big ? RGB(120, 8, 18) : RGB(48, 12, 16));
    FillRect(d->hDC, &r, fill); DeleteObject(fill);
    HBRUSH edge = CreateSolidBrush(dis ? RGB(70, 50, 52) : hot || big ? RGB(220, 40, 50) : RGB(140, 24, 32));
    FrameRect(d->hDC, &r, edge); DeleteObject(edge);
    char t[128]; GetWindowTextA(d->hwndItem, t, sizeof(t));
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, dis ? RGB(130, 110, 110) : RGB(245, 235, 235));
    HGDIOBJ of = SelectObject(d->hDC, font);
    if (down) OffsetRect(&r, 1, 1);
    DrawTextA(d->hDC, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(d->hDC, of);
}
