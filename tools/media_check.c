/* media_check: decode every movie (PSMF) and every ATRAC3/ATRAC3+ sound (RIFF WAVE) on the game
 * disc through the port's own decoders (h264_mf.c, the at3 bridge), and report errors. It checks
 * the media of chapters nobody has played to yet.
 *
 * Usage: media_check <iso> <offsets.txt>
 *   offsets.txt: lines "PSMF <byte offset>" / "RIFF <byte offset>" (found by scanning the image)
 * Build (MSYS2 UCRT64), see tools/media_check.sh.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "at3_bridge.h"

int  sr_h264_create(void);
void sr_h264_destroy(int id);
void sr_h264_feed(int id, const uint8_t *data, uint32_t len);
int  sr_h264_frame(int id, int eos, uint8_t *dst, int frameWidth, int pixelMode);

static FILE *s_iso;

static int read_at(uint64_t off, void *buf, size_t n) {
    if (_fseeki64(s_iso, (long long)off, SEEK_SET)) return 0;
    return fread(buf, 1, n, s_iso) == n;
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* ---- movies ---- */
static uint8_t s_frame[512 * 272 * 4];

static int check_psmf(uint64_t off, int *frames_out, int *expect_out) {
    uint8_t h[2048];
    if (!read_at(off, h, sizeof(h))) return -10;
    const uint32_t hdr = be32(h + 8), size = be32(h + 12);
    /* first/last presentation timestamps (PPSSPP: 6-byte fields, 90 kHz) */
    const uint64_t t0 = (uint64_t)be32(h + 0x56), t1 = (uint64_t)be32(h + 0x5C);
    *expect_out = t1 > t0 ? (int)((t1 - t0) / 3003) : -1;
    if (!size || size > (1024u << 20)) return -11;
    const int id = sr_h264_create();
    if (id < 0) return -12;
    const uint32_t chunk = 1u << 20;
    uint8_t *buf = (uint8_t *)malloc(chunk);
    int frames = 0, err = 0;
    for (uint32_t done = 0; done < size && !err;) {
        const uint32_t n = size - done < chunk ? size - done : chunk;
        if (!read_at(off + hdr + done, buf, n)) { err = -13; break; }
        sr_h264_feed(id, buf, n);
        done += n;
        for (;;) {
            const int r = sr_h264_frame(id, 0, s_frame, 512, 3);
            if (r < 0) { err = -1; break; }
            if (r == 0) break;
            frames++;
        }
    }
    for (int guard = 0; !err && guard < 100000; guard++) {   /* drain at end of stream */
        const int r = sr_h264_frame(id, 1, s_frame, 512, 3);
        if (r < 0) { err = -2; break; }
        if (r == 0) break;
        frames++;
    }
    free(buf);
    sr_h264_destroy(id);
    *frames_out = frames;
    return err;
}

/* ---- sounds ---- */
static int check_riff(uint64_t off, int *frames_out, int *codec_out, int *peak_out) {
    uint8_t h[4096];
    if (!read_at(off, h, sizeof(h))) return -10;
    const uint32_t riff_size = le32(h + 4);
    int fmt = -1, ch = 0, align = 0;
    uint32_t data_off = 0, data_size = 0;
    for (uint32_t p = 12; p + 8 <= sizeof(h);) {
        const uint32_t sz = le32(h + p + 4);
        if (!memcmp(h + p, "fmt ", 4)) { fmt = le16(h + p + 8); ch = le16(h + p + 10); align = le16(h + p + 20); }
        if (!memcmp(h + p, "data", 4)) { data_off = p + 8; data_size = sz; break; }
        p += 8 + sz + (sz & 1);
    }
    *codec_out = fmt;
    if (fmt < 0 || !data_off || !align || ch < 1 || ch > 2) return -20;
    if (data_size > riff_size || data_size > (64u << 20)) return -21;
    At3Dec *d;
    if (fmt == 0x270) {
        const int joint = ch == 2 && align == 0xC0;
        uint8_t extra[14] = { 0 };
        extra[0] = 1; extra[3] = (uint8_t)(ch << 3); extra[6] = (uint8_t)joint; extra[8] = (uint8_t)joint; extra[10] = 1;
        d = at3dec_create(AT3_CODEC_AT3, ch, align, extra, sizeof(extra));
    } else {
        d = at3dec_create(AT3_CODEC_AT3PLUS, ch, align, NULL, 0);
    }
    if (!d) return -22;
    uint8_t *data = (uint8_t *)malloc(data_size + 64);
    memset(data, 0, data_size + 64);
    int err = 0, frames = 0, peak = 0;
    if (!read_at(off + data_off, data, data_size)) err = -23;
    static int16_t out[2048 * 2 * 2];
    for (uint32_t p = 0; !err && p + (uint32_t)align <= data_size; p += (uint32_t)align) {
        int samples = 0;
        const int r = at3dec_decode(d, data + p, align, 2, out, &samples);
        if (r < 0) { err = -3; break; }
        frames++;
        for (int i = 0; i < samples * 2; i++) { const int v = abs(out[i]); if (v > peak) peak = v; }
    }
    free(data);
    at3dec_free(d);
    *frames_out = frames;
    *peak_out = peak;
    return err;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: media_check <iso> <offsets.txt>\n"); return 2; }
    s_iso = fopen(argv[1], "rb");
    FILE *lst = fopen(argv[2], "r");
    if (!s_iso || !lst) { fprintf(stderr, "cannot open inputs\n"); return 2; }
    char kind[8]; unsigned long long off;
    int nm = 0, bad_m = 0, short_m = 0, ns = 0, bad_s = 0, silent_s = 0;
    long long total_frames = 0;
    while (fscanf(lst, "%7s %llu", kind, &off) == 2) {
        if (!strcmp(kind, "PSMF")) {
            int frames = 0, expect = 0;
            const int r = check_psmf(off, &frames, &expect);
            nm++; total_frames += frames;
            const int shortish = expect > 0 && frames < expect - 2;
            if (r) bad_m++;
            if (shortish) short_m++;
            printf("movie %2d @%llu: %s, %d frames (expected ~%d)%s\n", nm, off, r ? "ERROR" : "ok", frames, expect,
                   shortish ? "  SHORT" : "");
            if (r) printf("   error %d\n", r);
        } else {
            int frames = 0, codec = 0, peak = 0;
            const int r = check_riff(off, &frames, &codec, &peak);
            ns++;
            if (r) { bad_s++; printf("sound %3d @%llu codec %#x: ERROR %d after %d frames\n", ns, off, codec, r, frames); }
            else if (peak == 0) { silent_s++; printf("sound %3d @%llu codec %#x: silent (%d frames)\n", ns, off, codec, frames); }
        }
        fflush(stdout);
    }
    printf("\nmovies: %d checked, %d with decode errors, %d shorter than their timestamps, %lld frames total\n",
           nm, bad_m, short_m, total_frames);
    printf("sounds: %d checked, %d with decode errors, %d decoded to silence\n", ns, bad_s, silent_s);
    return (bad_m || bad_s) ? 1 : 0;
}
