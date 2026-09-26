/* C interface to the vendored ATRAC3 / ATRAC3+ decoders (see at3_bridge.cpp). */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { AT3_CODEC_AT3PLUS = 0x1000, AT3_CODEC_AT3 = 0x1001 };

typedef struct At3Dec At3Dec;

/* channels = encoded channels (1 or 2). extraData is only used by ATRAC3 (joint stereo etc). */
At3Dec *at3dec_create(int codec, int channels, int blockAlign, const uint8_t *extraData, int extraDataSize);
void at3dec_free(At3Dec *d);
void at3dec_flush(At3Dec *d);
/* Decode one frame of inBytes. Writes *outSamples samples per channel (interleaved when
 * outChannels == 2) to out, which may be NULL to skip. Returns bytes consumed, or <0 on error. */
int at3dec_decode(At3Dec *d, const uint8_t *in, int inBytes, int outChannels, int16_t *out, int *outSamples);

#ifdef __cplusplus
}
#endif
