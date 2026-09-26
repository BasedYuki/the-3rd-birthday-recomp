/* C interface to the vendored ATRAC3 / ATRAC3+ decoders (third_party/at3_standalone, the
 * FFmpeg decoders as extracted by PPSSPP). Mirrors PPSSPP's Core/HW/Atrac3Standalone.cpp:
 * planar float output is converted to interleaved s16, mono sources are duplicated for
 * stereo output, and the ATRAC3+ context is opened on the first decode. */

#include <cstdint>
#include <cstring>

#include "at3_decoders.h"
#include "at3_bridge.h"

struct At3Dec {
    int codec;          /* AT3_CODEC_AT3PLUS or AT3_CODEC_AT3 */
    int channels;       /* encoded channels */
    int blockAlign;
    ATRAC3PContext *at3p;
    ATRAC3Context *at3;
    int failed;
    float bufL[4096], bufR[4096];
};

static inline int16_t clamp16(float f) {
    if (f >= 1.0f) return 32767;
    if (f <= -1.0f) return -32767;
    return (int16_t)(f * 32767);
}

extern "C" At3Dec *at3dec_create(int codec, int channels, int blockAlign,
                                 const uint8_t *extraData, int extraDataSize) {
    At3Dec *d = new At3Dec();
    d->codec = codec;
    d->channels = channels;
    d->blockAlign = blockAlign;
    if (codec == AT3_CODEC_AT3) {
        d->at3 = atrac3_alloc(channels, &d->blockAlign, extraData, extraDataSize);
        if (!d->at3) d->failed = 1;
    }
    return d;
}

extern "C" void at3dec_free(At3Dec *d) {
    if (!d) return;
    if (d->at3) atrac3_free(d->at3);
    if (d->at3p) atrac3p_free(d->at3p);
    delete d;
}

extern "C" void at3dec_flush(At3Dec *d) {
    if (!d) return;
    if (d->at3) atrac3_flush_buffers(d->at3);
    if (d->at3p) atrac3p_flush_buffers(d->at3p);
}

extern "C" int at3dec_decode(At3Dec *d, const uint8_t *in, int inBytes, int outChannels,
                             int16_t *out, int *outSamples) {
    *outSamples = 0;
    if (!d || d->failed) return -1;
    if (d->codec == AT3_CODEC_AT3PLUS && !d->at3p) {
        d->at3p = atrac3p_alloc(d->channels, &d->blockAlign);
        if (!d->at3p) { d->failed = 1; return -1; }
    }
    d->blockAlign = inBytes;
    float *bufs[2] = { d->bufL, d->bufR };
    int n = 0;
    int r = d->codec == AT3_CODEC_AT3PLUS ? atrac3p_decode_frame(d->at3p, bufs, &n, in, inBytes)
                                          : atrac3_decode_frame(d->at3, bufs, &n, in, inBytes);
    if (r < 0) return -1;
    if (n > 0 && out) {
        const float *l = d->bufL, *rr = d->channels == 2 ? d->bufR : d->bufL;
        if (outChannels == 2) {
            for (int i = 0; i < n; i++) { out[i * 2] = clamp16(l[i]); out[i * 2 + 1] = clamp16(rr[i]); }
        } else {
            for (int i = 0; i < n; i++) out[i] = clamp16(l[i]);
        }
    }
    *outSamples = n;
    return r;
}
