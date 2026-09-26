/* sceAtrac3plus: real ATRAC3 / ATRAC3+ decoding with the firmware's buffer model.
 *
 * A C port of PPSSPP's Atrac2 context (Core/HLE/AtracCtx2.cpp, the default implementation,
 * written to match hardware-tested pspautotests), the WAVE parser from Core/Util/AtracTrack.cpp
 * and the entry points in Core/HLE/sceAtrac.cpp. All GPL-2.0+. Frames are decoded by the
 * vendored FFmpeg decoders through at3_bridge.
 *
 * Buffer states (SceAtracIdInfo.state): 2 = whole file in memory, 3 = halfway buffer (sized for
 * the whole file, filled as the game reads), 4-6 = streamed through a smaller ring buffer
 * (the game polls GetStreamDataInfo, reads that many bytes from disc, then AddStreamData).
 * The context lives host-side; the game never asks for its guest address
 * (_sceAtracGetContextAddress isn't imported).
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"
#include "at3_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define A0 (s->r[4])
#define A1 (s->r[5])
#define A2 (s->r[6])
#define A3 (s->r[7])
#define T0 (s->r[8])

void sr_hle_register(uint32_t nid, const char *name, HleFn fn);

enum {
    ATRAC_ERR_API_FAIL              = 0x80630002,
    ATRAC_ERR_NO_ATRACID            = 0x80630003,
    ATRAC_ERR_INVALID_CODECTYPE     = 0x80630004,
    ATRAC_ERR_BAD_ATRACID           = 0x80630005,
    ATRAC_ERR_UNKNOWN_FORMAT        = 0x80630006,
    ATRAC_ERR_WRONG_CODECTYPE       = 0x80630007,
    ATRAC_ERR_BAD_CODEC_PARAMS      = 0x80630008,
    ATRAC_ERR_ALL_DATA_LOADED       = 0x80630009,
    ATRAC_ERR_NO_DATA               = 0x80630010,
    ATRAC_ERR_SIZE_TOO_SMALL        = 0x80630011,
    ATRAC_ERR_SECOND_BUFFER_NEEDED  = 0x80630012,
    ATRAC_ERR_INCORRECT_READ_SIZE   = 0x80630013,
    ATRAC_ERR_BAD_ALIGNMENT         = 0x80630014,
    ATRAC_ERR_BAD_SAMPLE            = 0x80630015,
    ATRAC_ERR_BAD_FIRST_RESET_SIZE  = 0x80630016,
    ATRAC_ERR_BAD_SECOND_RESET_SIZE = 0x80630017,
    ATRAC_ERR_ADD_DATA_IS_TOO_BIG   = 0x80630018,
    ATRAC_ERR_NO_LOOP_INFORMATION   = 0x80630021,
    ATRAC_ERR_SECOND_BUFFER_NOT_NEEDED = 0x80630022,
    ATRAC_ERR_BUFFER_IS_EMPTY       = 0x80630023,
    ATRAC_ERR_ALL_DATA_DECODED      = 0x80630024,
    ATRAC_ERR_IS_LOW_LEVEL          = 0x80630031,
    ATRAC_ERR_IS_FOR_SCESAS         = 0x80630040,
    KERNEL_ERR_ILLEGAL_ADDR         = 0x800200d3,
    KERNEL_ERR_BUSY                 = 0x80000021,
    KERNEL_ERR_OUT_OF_MEMORY        = 0x80000022,
};

enum {
    ST_NO_DATA = 1, ST_ALL_DATA_LOADED = 2, ST_HALFWAY_BUFFER = 3,
    ST_STREAMED_WITHOUT_LOOP = 4, ST_STREAMED_LOOP_FROM_END = 5, ST_STREAMED_LOOP_WITH_TRAILER = 6,
    ST_LOW_LEVEL = 8, ST_FOR_SCESAS = 16,
};
#define ST_IS_STREAMING(st) (((st) & 4) != 0)

enum { REMAIN_ALLDATA_ON_MEMORY = -1, REMAIN_NONLOOP_ON_MEMORY = -2, REMAIN_LOOP_ON_MEMORY = -3 };

#define MAX_ATRAC 6

/* SceAtracIdInfo (the firmware's per-ID state), field for field. */
typedef struct {
    int32_t decodePos, endSample, loopStart, loopEnd, firstValidSample;
    uint8_t numSkipFrames, state, curBuffer, numChan;
    uint16_t sampleSize, codec;
    int32_t dataOff, curFileOff, fileDataEnd, loopNum, streamDataByte, streamOff, secondStreamOff;
    uint32_t buffer, secondBuffer, bufferByte, secondBufferByte;
} AtracInfo;

typedef struct {
    int used;
    AtracInfo info;
    At3Dec *dec;
    uint32_t codecErr;
    int outputChannels;
    int16_t decodeTemp[2048 * 2];
} Atrac;

static Atrac s_atrac[MAX_ATRAC];
static uint32_t s_ctxType[MAX_ATRAC] = { AT3_CODEC_AT3PLUS, AT3_CODEC_AT3PLUS, AT3_CODEC_AT3, AT3_CODEC_AT3, 0, 0 };
static int s_inited = 1;
static int s_log = -1;

#define ALOG(...) do { if (s_log) fprintf(stderr, __VA_ARGS__); } while (0)

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int iclamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int spf(const AtracInfo *i)  { return i->codec == AT3_CODEC_AT3PLUS ? 2048 : 1024; }
static int spf_mask(const AtracInfo *i) { return spf(i) - 1; }
static int skip_samples(const AtracInfo *i) { return i->codec == AT3_CODEC_AT3PLUS ? 0x170 : 0x45; }

static int round_down(int size, int grain) { return size - (size % grain); }
static int round_down_off(int offset, int size, int grain) {
    return size > offset ? ((size - offset) / grain) * grain + offset : size;
}
static int skip_frames_for(const AtracInfo *i, int seekPos) {
    return (seekPos & spf_mask(i)) < skip_samples(i) ? 2 : 1;
}
static int file_offset_for(const AtracInfo *i, int seekPos) {
    int frameOffset = ((seekPos / spf(i)) - 1) * i->sampleSize;
    if ((seekPos & spf_mask(i)) < skip_samples(i) && frameOffset != 0) frameOffset -= i->sampleSize;
    return frameOffset + i->dataOff;
}
static int loop_end_file_offset(const AtracInfo *i, int seekPos) {
    return (seekPos / spf(i) + 1) * i->sampleSize + i->dataOff;
}

/* ---- WAVE parsing (PPSSPP ParseWaveAT3) ---------------------------------------------- */

typedef struct {
    uint16_t numChans, blockAlign;
    uint8_t sampleSizeMaybe, tailFlag;
    uint32_t dataOff, endSample, waveDataSize, firstSampleOffset, loopStart, loopEnd;
} TrackInfo;

static uint32_t rd16(const uint8_t *d, int *o) { uint32_t v = (uint32_t)d[*o] | ((uint32_t)d[*o + 1] << 8); *o += 2; return v; }
static uint32_t rd32(const uint8_t *d, int *o) { uint32_t v; memcpy(&v, d + *o, 4); *o += 4; return v; }

static const uint8_t at3_checkbytes[16] = {
    0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44, 0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62,
};

/* Returns the codec (AT3_CODEC_*) or a negative error. */
static int parse_wave_at3(const uint8_t *data, uint32_t len, TrackInfo *t) {
    memset(t, 0, sizeof(*t));
    t->loopStart = t->loopEnd = 0xFFFFFFFFu;
    int retval = (int)ATRAC_ERR_UNKNOWN_FORMAT;
    int off = 0;
    for (;;) {
        if ((uint32_t)off + 0xC >= len) return (int)ATRAC_ERR_SIZE_TOO_SMALL;
        if (rd32(data, &off) != 0x46464952u) return (int)ATRAC_ERR_UNKNOWN_FORMAT;  /* RIFF */
        uint32_t blockSize = (rd32(data, &off) + 1) & ~1u;
        if (rd32(data, &off) == 0x45564157u) break;                                 /* WAVE */
        if (blockSize < 4 || (uint64_t)off + blockSize - 4 > len) return (int)ATRAC_ERR_SIZE_TOO_SMALL;
        off += (int)blockSize - 4;
    }
    int modifiedSampleOffset = 0;
    for (;;) {
        if ((uint32_t)off + 8 >= len) return (int)ATRAC_ERR_SIZE_TOO_SMALL;
        uint32_t id = rd32(data, &off);
        int chunkSize = (int)((rd32(data, &off) + 1) & ~1u);
        int next = off + chunkSize;
        if ((uint32_t)(off + chunkSize) > len && id != 0x61746164u) return (int)ATRAC_ERR_SIZE_TOO_SMALL;
        switch (id) {
        case 0x61746164u: /* data */
            t->waveDataSize = (uint32_t)chunkSize;
            t->dataOff = (uint32_t)off;
            if (!t->firstSampleOffset) t->firstSampleOffset = retval == AT3_CODEC_AT3 ? 0x400 : 0x800;
            if (modifiedSampleOffset && retval == AT3_CODEC_AT3PLUS) {
                t->firstSampleOffset -= 0xb8;
                if (t->loopEnd != 0xFFFFFFFFu) { t->loopEnd -= 0xb8; t->loopStart -= 0xb8; }
            }
            return retval;
        case 0x20746D66u: { /* fmt */
            if (retval != (int)ATRAC_ERR_UNKNOWN_FORMAT || chunkSize < 0x20) return (int)ATRAC_ERR_UNKNOWN_FORMAT;
            uint32_t fmtTag = rd16(data, &off);
            t->numChans = (uint16_t)rd16(data, &off);
            if (t->numChans != 1 && t->numChans != 2) return (int)ATRAC_ERR_UNKNOWN_FORMAT;
            if (rd32(data, &off) != 44100) return (int)ATRAC_ERR_UNKNOWN_FORMAT;
            off += 4;
            t->blockAlign = (uint16_t)rd16(data, &off);
            if (!t->blockAlign) return (int)ATRAC_ERR_UNKNOWN_FORMAT;
            if (fmtTag == 0x270) {
                off += 4;
                if (rd16(data, &off) != 1) return (int)ATRAC_ERR_UNKNOWN_FORMAT;   /* jointStereo */
                off += 4;
                uint32_t sst = rd16(data, &off);
                t->sampleSizeMaybe = (uint8_t)sst; t->tailFlag = (uint8_t)(sst >> 8);
                if (sst != rd16(data, &off)) return (int)ATRAC_ERR_UNKNOWN_FORMAT;
                if (rd32(data, &off) != 1) return (int)ATRAC_ERR_UNKNOWN_FORMAT;
                retval = AT3_CODEC_AT3;
            } else if (fmtTag == 0xFFFE) {
                if (chunkSize < 0x34) return (int)ATRAC_ERR_UNKNOWN_FORMAT;
                if (memcmp(data + off + 10, at3_checkbytes, 16) != 0) return (int)ATRAC_ERR_UNKNOWN_FORMAT;
                t->sampleSizeMaybe = data[off + 0x1c];
                t->tailFlag = data[off + 0x1d];
                if (((uint32_t)t->sampleSizeMaybe << 27) >> 29 != t->numChans) return (int)ATRAC_ERR_UNKNOWN_FORMAT;
                retval = AT3_CODEC_AT3PLUS;
            } else {
                return (int)ATRAC_ERR_UNKNOWN_FORMAT;
            }
            break;
        }
        case 0x6C706D73u: /* smpl */
            if ((int32_t)t->loopStart < 0) {
                if (chunkSize < 0x20) return (int)ATRAC_ERR_UNKNOWN_FORMAT;
                off += 0x1c;
                if (rd32(data, &off) != 0) {
                    if (chunkSize < 0x34) return (int)ATRAC_ERR_SIZE_TOO_SMALL;
                    off += 0xc;
                    t->loopStart = rd32(data, &off);
                    t->loopEnd = rd32(data, &off);
                    if ((int32_t)t->loopEnd <= (int32_t)t->loopStart) return (int)ATRAC_ERR_BAD_CODEC_PARAMS;
                }
            }
            break;
        case 0x74636166u: { /* fact */
            if (chunkSize < 4) return (int)ATRAC_ERR_UNKNOWN_FORMAT;
            t->endSample = rd32(data, &off);
            int rem = chunkSize - 4;
            if (rem == 4) t->firstSampleOffset = rd32(data, &off);
            else if (rem >= 8) { off += 4; t->firstSampleOffset = rd32(data, &off); modifiedSampleOffset = 1; }
            break;
        }
        default: break;
        }
        off = next;
    }
}

/* ---- context setup (PPSSPP InitLengthAndLoop / InitContextFromTrackInfo) ------------- */

static void init_length_and_loop(AtracInfo *c, int endSample, int waveDataSize, int firstSampleOffset,
                                 int loopBegin, int loopEnd) {
    const int off = c->codec == AT3_CODEC_AT3 ? 0x45 : 0x170;
    const int blockShift = 0x100b - c->codec;
    const int firstValidSample = firstSampleOffset + off;
    int numSamplesInFile = endSample == 0 ? (waveDataSize / c->sampleSize) << (blockShift & 0x1f)
                                          : endSample + firstValidSample;
    c->decodePos = firstValidSample;
    c->loopNum = 0;
    c->endSample = numSamplesInFile - 1;
    c->numSkipFrames = (uint8_t)(firstValidSample >> (blockShift & 0x1f));
    if (loopBegin > -1) { c->loopEnd = loopEnd + off; c->loopStart = loopBegin + off; }
    else { c->loopEnd = 0; c->loopStart = 0; }
}

static int compute_state_and_init_second_buffer(AtracInfo *i, uint32_t readSize, uint32_t bufferSize) {
    int state;
    if (bufferSize < (uint32_t)i->fileDataEnd) {
        if (i->streamDataByte < (int32_t)i->sampleSize * 2) return (int)ATRAC_ERR_SIZE_TOO_SMALL;
        state = ST_STREAMED_WITHOUT_LOOP;
        if (i->loopEnd != 0) {
            state = ST_STREAMED_LOOP_FROM_END;
            if (i->loopEnd != i->endSample) {
                int loopEnd = (loop_end_file_offset(i, i->loopEnd) - i->dataOff) + 1;
                i->state = ST_STREAMED_LOOP_WITH_TRAILER;
                if (loopEnd < i->streamDataByte) i->streamDataByte = loopEnd;
                i->secondStreamOff = 0; i->secondBuffer = 0; i->secondBufferByte = 0;
                return 0;
            }
        }
    } else {
        state = readSize >= (uint32_t)i->fileDataEnd ? ST_ALL_DATA_LOADED : ST_HALFWAY_BUFFER;
    }
    i->state = (uint8_t)state;
    return 0;
}

static int init_from_track(AtracInfo *i, const TrackInfo *w, uint32_t bufferAddr, int readSize, int bufferSize) {
    i->numChan = (uint8_t)w->numChans;
    i->firstValidSample = (i->codec == AT3_CODEC_AT3 ? 0x45 : 0x170) + (int)w->firstSampleOffset;
    i->sampleSize = w->blockAlign;
    init_length_and_loop(i, (int)w->endSample, (int)w->waveDataSize, (int)w->firstSampleOffset,
                         (int)w->loopStart, (int)w->loopEnd);
    const int dataOff = (int)w->dataOff;
    i->streamDataByte = readSize - dataOff;
    i->buffer = bufferAddr;
    i->curFileOff = dataOff;
    i->dataOff = dataOff;
    i->fileDataEnd = (int)w->waveDataSize + dataOff;
    i->curBuffer = 0;
    i->bufferByte = (uint32_t)bufferSize;
    i->streamOff = dataOff;
    if (i->sampleSize > (uint32_t)bufferSize) return (int)ATRAC_ERR_BAD_CODEC_PARAMS;
    if (i->loopEnd > i->endSample) return (int)ATRAC_ERR_BAD_CODEC_PARAMS;
    const int numChunks = i->endSample >> ((0x100b - i->codec) & 0x1f);
    if ((uint32_t)numChunks * i->sampleSize < w->waveDataSize)
        return compute_state_and_init_second_buffer(i, (uint32_t)readSize, (uint32_t)bufferSize);
    return (int)ATRAC_ERR_BAD_CODEC_PARAMS;
}

/* ---- remaining-frame / stream-info math (PPSSPP Atrac2) ------------------------------ */

static int space_used(const AtracInfo *i) {
    if (i->decodePos > i->loopEnd && i->curBuffer == 1) {
        int space = (int)i->secondBufferByte;
        if (i->secondStreamOff < space) space = round_down_off(i->secondStreamOff, (int)i->secondBufferByte, i->sampleSize);
        if (i->secondStreamOff <= space && space - i->secondStreamOff < i->streamDataByte)
            return i->streamDataByte - (space - i->secondStreamOff);
        return 0;
    }
    return i->streamDataByte;
}

static int remain_stream(const AtracInfo *i) {
    if (i->streamDataByte >= i->fileDataEnd - i->curFileOff) return REMAIN_NONLOOP_ON_MEMORY;
    return imax(0, i->streamDataByte / i->sampleSize - (int)i->numSkipFrames);
}

static int remain_looped(const AtracInfo *i) {
    const int ls = file_offset_for(i, i->loopStart), le = loop_end_file_offset(i, i->loopEnd);
    const int writeFileOff = i->curFileOff + i->streamDataByte;
    const int leftToRead = writeFileOff - le;
    int remain;
    if (writeFileOff <= le) {
        remain = i->streamDataByte / i->sampleSize;
    } else {
        const int skipAtStart = skip_frames_for(i, i->loopStart);
        const int firstPart = le - ls;
        const int secondPart = leftToRead % firstPart;
        remain = (le - i->curFileOff) / i->sampleSize + (leftToRead / firstPart) * (firstPart / i->sampleSize - skipAtStart);
        if (secondPart > skipAtStart * i->sampleSize) remain += secondPart / i->sampleSize - skipAtStart;
    }
    remain = imax(0, remain - (int)i->numSkipFrames);
    if (i->loopNum < 0) return remain;
    const int end = i->curFileOff + i->streamDataByte;
    if (end >= le && i->loopNum <= (end - le) / (le - ls)) return REMAIN_LOOP_ON_MEMORY;
    return remain;
}

static int remaining_frames(const AtracInfo *i) {
    switch (i->state) {
    case 0: case ST_NO_DATA: return 0;
    case ST_ALL_DATA_LOADED: return REMAIN_ALLDATA_ON_MEMORY;
    case ST_HALFWAY_BUFFER: {
        const int w = i->dataOff + i->streamDataByte;
        return i->curFileOff < w ? imax(0, (w - i->curFileOff) / i->sampleSize - (int)i->numSkipFrames) : 0;
    }
    case ST_STREAMED_WITHOUT_LOOP: return remain_stream(i);
    case ST_STREAMED_LOOP_FROM_END: return remain_looped(i);
    case ST_STREAMED_LOOP_WITH_TRAILER: return i->decodePos <= i->loopEnd ? remain_looped(i) : remain_stream(i);
    default: return (int)ATRAC_ERR_BAD_ATRACID;
    }
}

static int looped_writable(const AtracInfo *i, int ls, int le) {
    const int w = i->curFileOff + i->streamDataByte;
    if (w >= le) { const int len = le - ls; return len - (w - le) % len; }
    return le - w;
}
static int inc_and_loop(int cur, int inc, int ls, int le) {
    const int sum = cur + inc;
    return sum >= le ? ls + (sum - le) % (le - ls) : sum;
}
static int wrap_rounded(int offset, int bufferSize, int addend, int grain) {
    bufferSize = round_down_off(offset, bufferSize, grain);
    const int sum = offset + addend;
    return bufferSize <= sum ? sum - bufferSize : sum;
}

static void stream_data_info(const AtracInfo *i, uint32_t *writePtr, uint32_t *toWrite, uint32_t *readOff) {
    switch (i->state) {
    case ST_ALL_DATA_LOADED:
        *writePtr = i->buffer; *toWrite = 0; *readOff = 0;
        return;
    case ST_HALFWAY_BUFFER: {
        const int fo = i->dataOff + i->streamDataByte;
        *writePtr = i->buffer + (uint32_t)fo; *toWrite = (uint32_t)(i->fileDataEnd - fo); *readOff = (uint32_t)fo;
        return;
    }
    default: break;
    }
    const int streamOff = i->curBuffer != 1 ? i->streamOff : 0;
    const int used = space_used(i);
    const int afterOff = round_down_off(streamOff, (int)i->bufferByte, i->sampleSize);
    const int streamPos = streamOff + used;
    int left = streamPos >= afterOff ? afterOff - used : afterOff - streamPos;
    const int ls = file_offset_for(i, i->loopStart), le = loop_end_file_offset(i, i->loopEnd);
    if (left < 0) left = 0;
    switch (i->state) {
    case ST_STREAMED_WITHOUT_LOOP: {
        *toWrite = (uint32_t)iclamp(i->fileDataEnd - (i->curFileOff + i->streamDataByte), 0, left);
        const int sfo = i->curFileOff + i->streamDataByte;
        if (sfo < i->fileDataEnd) {
            *readOff = (uint32_t)sfo;
            *writePtr = i->buffer + (uint32_t)wrap_rounded(i->streamOff, (int)i->bufferByte, i->streamDataByte, i->sampleSize);
        } else {
            *readOff = 0; *writePtr = i->buffer;
        }
        break;
    }
    case ST_STREAMED_LOOP_FROM_END:
        *toWrite = (uint32_t)imin(looped_writable(i, ls, le), left);
        *readOff = (uint32_t)inc_and_loop(i->curFileOff, i->streamDataByte, ls, le);
        *writePtr = i->buffer + (uint32_t)wrap_rounded(i->streamOff, (int)i->bufferByte, i->streamDataByte, i->sampleSize);
        break;
    case ST_STREAMED_LOOP_WITH_TRAILER:
        if (i->decodePos <= i->loopEnd) {
            *toWrite = (uint32_t)imin(looped_writable(i, ls, le), left);
            *readOff = (uint32_t)inc_and_loop(i->curFileOff, i->streamDataByte, ls, le);
        } else {
            const int sfo = i->curFileOff + i->streamDataByte;
            *toWrite = (uint32_t)iclamp(i->fileDataEnd - sfo, 0, left);
            *readOff = sfo < i->fileDataEnd ? (uint32_t)sfo : 0;
        }
        if (i->decodePos <= i->loopEnd || i->curBuffer != 1)
            *writePtr = i->buffer + (uint32_t)wrap_rounded(i->streamOff, (int)i->bufferByte, i->streamDataByte, i->sampleSize);
        else
            *writePtr = i->buffer + (uint32_t)wrap_rounded(0, (int)i->bufferByte, used, i->sampleSize);
        break;
    default:
        *writePtr = i->buffer; *toWrite = 0; *readOff = 0;
        break;
    }
}

/* ---- decoding (PPSSPP Atrac2::DecodeInternal) ---------------------------------------- */

static int next_samples(const AtracInfo *i) {
    const int endOfFrame = i->decodePos | spf_mask(i);
    const int remainder = imax(0, endOfFrame - i->endSample);
    const int adjusted = (i->decodePos & spf_mask(i)) + remainder;
    return imax(0, spf(i) - adjusted);
}

static uint32_t decode_internal(Atrac *a, uint32_t outAddr, int *samplesNum, int *finish) {
    AtracInfo *i = &a->info;
    const int toDecode = next_samples(i);
    const int nextFileOff = i->curFileOff + i->sampleSize;
    if (nextFileOff > i->fileDataEnd || i->decodePos > i->endSample) { *finish = 1; return ATRAC_ERR_ALL_DATA_DECODED; }
    if (ST_IS_STREAMING(i->state) && i->streamDataByte < i->sampleSize) { *finish = 0; return ATRAC_ERR_BUFFER_IS_EMPTY; }
    if (i->state == ST_HALFWAY_BUFFER && i->dataOff + i->streamDataByte < nextFileOff) { *finish = 0; return ATRAC_ERR_BUFFER_IS_EMPTY; }

    uint32_t bufferPtr, streamOff;
    if (!ST_IS_STREAMING(i->state)) {
        bufferPtr = i->buffer; streamOff = (uint32_t)i->curFileOff;
    } else {
        const int idx = i->curBuffer & 1;
        bufferPtr = idx == 0 ? i->buffer : i->secondBuffer;
        streamOff = (uint32_t)(idx == 0 ? i->streamOff : i->secondStreamOff);
    }
    const uint32_t inAddr = bufferPtr + streamOff;
    if (!sr_inrange(inAddr) || !sr_inrange(inAddr + i->sampleSize - 1)) return ATRAC_ERR_API_FAIL;

    const int full = toDecode == spf(i);
    int16_t *outPtr = full ? (outAddr ? (int16_t *)SR_HOST(outAddr) : NULL) : a->decodeTemp;
    int outSamples = 0;
    int r = at3dec_decode(a->dec, (const uint8_t *)SR_HOST(inAddr), i->sampleSize, a->outputChannels, outPtr, &outSamples);
    if (r < 0) {
        static int nerr = 0;
        if (nerr++ < 10)
            fprintf(stderr, "ATRAC: frame decode failed (codec %04x, %d bytes at file offset %d)\n",
                    i->codec, i->sampleSize, i->curFileOff);
        *finish = 0; a->codecErr = 0x20b; return ATRAC_ERR_API_FAIL;
    }
    a->codecErr = 0;

    i->curFileOff += i->sampleSize;
    if (i->numSkipFrames == 0) {
        *samplesNum = toDecode;
        *finish = i->endSample < i->decodePos + toDecode ? i->loopNum == 0 : 0;
        if (!full && toDecode != 0 && outAddr)
            memcpy(SR_HOST(outAddr), a->decodeTemp, (size_t)toDecode * (size_t)a->outputChannels * 2);
        i->decodePos += toDecode;
        if (i->loopEnd != 0 && i->loopNum != 0 && i->decodePos > i->loopEnd) {
            i->curFileOff = file_offset_for(i, i->loopStart);
            i->numSkipFrames = (uint8_t)skip_frames_for(i, i->loopStart);
            i->decodePos = i->loopStart;
            if (i->loopNum > 0) i->loopNum--;
        }
    } else {
        i->numSkipFrames--;
    }

    if (ST_IS_STREAMING(i->state)) {
        i->streamDataByte -= i->sampleSize;
        if (i->curBuffer == 1) {
            const int next = i->secondStreamOff + i->sampleSize;
            if ((int)i->secondBufferByte < next + i->sampleSize) { i->streamOff = 0; i->secondStreamOff = 0; i->curBuffer = 2; }
            else i->secondStreamOff = next;
        } else {
            const int next = i->streamOff + i->sampleSize;
            i->streamOff = next + i->sampleSize > (int)i->bufferByte ? 0 : next;
            if (i->state == ST_STREAMED_LOOP_WITH_TRAILER && i->curBuffer == 0 &&
                (i->loopEnd == 0 || (i->loopNum == 0 && i->loopEnd < i->decodePos)) &&
                i->curFileOff >= loop_end_file_offset(i, i->loopEnd)) {
                i->curBuffer = 1;
                i->streamDataByte = (int)i->secondBufferByte;
                i->secondStreamOff = 0;
                uint32_t copyLen = i->secondBufferByte % i->sampleSize;
                if (copyLen > i->bufferByte) copyLen = i->bufferByte;
                memcpy(SR_HOST(i->buffer), SR_HOST(i->secondBuffer + (i->secondBufferByte - i->secondBufferByte % i->sampleSize)), copyLen);
            }
        }
    }
    return 0;
}

static uint32_t skip_frames(Atrac *a, int *skipCount) {
    *skipCount = 0;
    int fin;
    while (a->info.numSkipFrames) {
        uint32_t r = decode_internal(a, 0, NULL, &fin);
        if (r) { if (r == ATRAC_ERR_API_FAIL) (*skipCount)++; return r; }
        (*skipCount)++;
    }
    return 0;
}

static void wrap_last_packet(AtracInfo *i) {
    if (!ST_IS_STREAMING(i->state)) return;
    const int distanceToEnd = round_down((int)i->bufferByte - i->streamOff, i->sampleSize);
    if (i->streamDataByte < distanceToEnd) {
        memset(SR_HOST(i->buffer), 0, 128);
    } else {
        const int copyStart = i->streamOff + distanceToEnd;
        memmove(SR_HOST(i->buffer), SR_HOST(i->buffer + (uint32_t)copyStart), (size_t)((int)i->bufferByte - copyStart));
    }
}

static int set_data(Atrac *a, const TrackInfo *t, uint32_t buffer, uint32_t readSize, uint32_t bufferSize) {
    int r = init_from_track(&a->info, t, buffer, (int)readSize, (int)bufferSize);
    if (r < 0) return r;
    AtracInfo *i = &a->info;
    if (a->dec) at3dec_free(a->dec);
    if (i->codec == AT3_CODEC_AT3) {
        int joint = i->numChan == 2 && i->sampleSize == 0xC0;   /* PPSSPP at3HeaderMap */
        uint8_t extra[14] = {0};
        extra[0] = 1; extra[3] = (uint8_t)(i->numChan << 3); extra[6] = (uint8_t)joint; extra[8] = (uint8_t)joint; extra[10] = 1;
        a->dec = at3dec_create(AT3_CODEC_AT3, i->numChan, i->sampleSize, extra, sizeof(extra));
    } else {
        a->dec = at3dec_create(AT3_CODEC_AT3PLUS, i->numChan, i->sampleSize, NULL, 0);
    }
    a->outputChannels = 2;
    ALOG("ATRAC set: codec=%04x ch=%d sampleSize=%d state=%d buf=%08x bufferByte=%u dataOff=%d fileDataEnd=%d "
         "stream=%d endSample=%d loop=%d..%d\n", i->codec, i->numChan, i->sampleSize, i->state, i->buffer,
         i->bufferByte, i->dataOff, i->fileDataEnd, i->streamDataByte, i->endSample, i->loopStart, i->loopEnd);
    int skip = 0;
    r = (int)skip_frames(a, &skip);
    wrap_last_packet(i);
    return r;
}

/* ---- ID management ------------------------------------------------------------------- */

static Atrac *get_atrac(uint32_t id) { return id < MAX_ATRAC && s_atrac[id].used ? &s_atrac[id] : NULL; }

static int alloc_atrac(int codec) {
    for (int i = 0; i < MAX_ATRAC; i++) {
        if (s_ctxType[i] == (uint32_t)codec && !s_atrac[i].used) {
            Atrac *a = &s_atrac[i];
            if (a->dec) at3dec_free(a->dec);
            memset(a, 0, sizeof(*a));
            a->used = 1;
            a->info.codec = (uint16_t)codec;
            a->info.state = ST_NO_DATA;
            a->outputChannels = 2;
            return i;
        }
    }
    return (int)ATRAC_ERR_NO_ATRACID;
}

static void release_atrac(int id) {
    Atrac *a = &s_atrac[id];
    if (a->dec) at3dec_free(a->dec);
    a->dec = NULL;
    a->used = 0;
}

static uint32_t validate_data(const Atrac *a) {
    if (!a) return ATRAC_ERR_BAD_ATRACID;
    if (a->info.state == ST_NO_DATA) return ATRAC_ERR_NO_DATA;
    return 0;
}
static uint32_t validate_managed(const Atrac *a) {
    uint32_t e = validate_data(a);
    if (e) return e;
    if (a->info.state == ST_LOW_LEVEL) return ATRAC_ERR_IS_LOW_LEVEL;
    if (a->info.state == ST_FOR_SCESAS) return ATRAC_ERR_IS_FOR_SCESAS;
    return 0;
}

static void wr32(uint32_t addr, uint32_t v) { if (addr && sr_inrange(addr)) MEM_W32(addr, v); }

/* ---- entry points (PPSSPP sceAtrac.cpp) ---------------------------------------------- */

static uint32_t set_data_and_get_id(CpuState *s, uint32_t buffer, uint32_t readSize, uint32_t bufferSize) {
    TrackInfo t;
    if (readSize < 72) return ATRAC_ERR_SIZE_TOO_SMALL;
    if (!sr_inrange(buffer)) return ATRAC_ERR_UNKNOWN_FORMAT;
    int codec = parse_wave_at3((const uint8_t *)SR_HOST(buffer), readSize, &t);
    if (codec < 0) return (uint32_t)codec;
    int id = alloc_atrac(codec);
    if (id < 0) return (uint32_t)id;
    int r = set_data(&s_atrac[id], &t, buffer, readSize, bufferSize);
    if (r < 0) { release_atrac(id); return (uint32_t)r; }
    sched_delay_current(100);
    (void)s;
    return (uint32_t)id;
}

static uint32_t h_SetDataAndGetID(CpuState *s) {
    int32_t size = (int32_t)A1;
    if (size < 0) size = 0x10000000;
    return set_data_and_get_id(s, A0, (uint32_t)size, (uint32_t)size);
}
static uint32_t h_SetHalfwayBufferAndGetID(CpuState *s) {
    if (A1 > A2) return ATRAC_ERR_INCORRECT_READ_SIZE;
    return set_data_and_get_id(s, A0, A1, A2);
}
static uint32_t set_data_id(CpuState *s, uint32_t id, uint32_t buffer, uint32_t readSize, uint32_t bufferSize) {
    Atrac *a = get_atrac(id);
    if (!a) return ATRAC_ERR_BAD_ATRACID;
    if (readSize > bufferSize) return ATRAC_ERR_INCORRECT_READ_SIZE;
    if (readSize < 72) return ATRAC_ERR_SIZE_TOO_SMALL;
    TrackInfo t;
    int codec = parse_wave_at3((const uint8_t *)SR_HOST(buffer), readSize, &t);
    if (codec < 0) return (uint32_t)codec;
    if ((uint32_t)codec != s_ctxType[id]) return ATRAC_ERR_WRONG_CODECTYPE;
    a->info.codec = (uint16_t)codec;
    int r = set_data(a, &t, buffer, readSize, bufferSize);
    if (r < 0) return (uint32_t)r;
    sched_delay_current(100);
    (void)s;
    return (uint32_t)r;
}
static uint32_t h_SetData(CpuState *s)          { return set_data_id(s, A0, A1, A2, A2); }
static uint32_t h_SetHalfwayBuffer(CpuState *s) { return set_data_id(s, A0, A1, A2, A3); }

static uint32_t h_GetAtracID(CpuState *s) {
    if (A0 != AT3_CODEC_AT3 && A0 != AT3_CODEC_AT3PLUS) return ATRAC_ERR_INVALID_CODECTYPE;
    return (uint32_t)alloc_atrac((int)A0);
}

static uint32_t h_ReleaseAtracID(CpuState *s) {
    if (!get_atrac(A0)) return ATRAC_ERR_BAD_ATRACID;
    release_atrac((int)A0);
    return 0;
}

static uint32_t h_DecodeData(CpuState *s) {
    /* (id, outAddr, *numSamples, *finish, *remainFrames) */
    Atrac *a = get_atrac(A0);
    uint32_t err = validate_data(a);
    if (err) return err;
    const uint32_t outAddr = A1, numAddr = A2, finAddr = A3, remAddr = T0;
    if (outAddr & 1) return ATRAC_ERR_BAD_ALIGNMENT;
    if (outAddr && !sr_inrange(outAddr)) return ATRAC_ERR_SIZE_TOO_SMALL;
    int num = 0, finish = 0, remains = 0;
    uint32_t ret = 0;
    const int tries = a->info.numSkipFrames + 1;
    for (int k = 0; k < tries; k++) {
        ret = decode_internal(a, outAddr, &num, &finish);
        if (ret) { num = 0; break; }
    }
    if (!ret) remains = remaining_frames(&a->info);
    wr32(numAddr, (uint32_t)num);
    wr32(finAddr, (uint32_t)finish);
    if (!ret) wr32(remAddr, (uint32_t)remains);
    if (s_log > 1) ALOG("ATRAC decode id=%u -> %08x n=%d fin=%d remain=%d pos=%d stream=%d\n",
                        A0, ret, num, finish, remains, a->info.decodePos, a->info.streamDataByte);
    if (ret == 0 || ret == ATRAC_ERR_API_FAIL) sched_delay_current(2300);
    return ret;
}

static uint32_t h_AddStreamData(CpuState *s) {
    Atrac *a = get_atrac(A0);
    uint32_t err = validate_managed(a);
    if (err) return err;
    AtracInfo *i = &a->info;
    if (i->state == ST_ALL_DATA_LOADED) return ATRAC_ERR_ALL_DATA_LOADED;
    if (i->state == ST_HALFWAY_BUFFER) {
        const int nfo = i->streamDataByte + i->dataOff + (int)A1;
        if (nfo == i->fileDataEnd) i->state = ST_ALL_DATA_LOADED;
        else if (nfo > i->fileDataEnd) return ATRAC_ERR_ADD_DATA_IS_TOO_BIG;
    }
    i->streamDataByte += (int)A1;
    return 0;
}

static uint32_t h_GetStreamDataInfo(CpuState *s) {
    Atrac *a = get_atrac(A0);
    uint32_t err = validate_managed(a);
    if (err) return err;
    uint32_t wp, wb, ro;
    stream_data_info(&a->info, &wp, &wb, &ro);
    wr32(A1, wp); wr32(A2, wb); wr32(A3, ro);
    return 0;
}

static uint32_t h_GetRemainFrame(CpuState *s) {
    Atrac *a = get_atrac(A0);
    uint32_t err = validate_managed(a);
    if (err) return err;
    if (!sr_inrange(A1)) return KERNEL_ERR_ILLEGAL_ADDR;
    MEM_W32(A1, (uint32_t)remaining_frames(&a->info));
    return 0;
}

static uint32_t h_SetLoopNum(CpuState *s) {
    Atrac *a = get_atrac(A0);
    uint32_t err = validate_data(a);
    if (err) return err;
    if (a->info.loopEnd <= 0) return ATRAC_ERR_NO_LOOP_INFORMATION;
    a->info.loopNum = (int32_t)A1;
    return 0;
}

static uint32_t h_GetLoopStatus(CpuState *s) {
    Atrac *a = get_atrac(A0);
    uint32_t err = validate_data(a);
    if (err) return err;
    const AtracInfo *i = &a->info;
    wr32(A1, (uint32_t)i->loopNum);
    int st = i->loopEnd == 0 ? 0 : i->loopNum != 0 ? 1 : (i->decodePos <= i->loopEnd);
    wr32(A2, (uint32_t)st);
    return 0;
}

static uint32_t h_GetSoundSample(CpuState *s) {
    Atrac *a = get_atrac(A0);
    uint32_t err = validate_managed(a);
    if (err) return err;
    const AtracInfo *i = &a->info;
    wr32(A1, (uint32_t)(i->endSample - i->firstValidSample));
    wr32(A2, i->loopEnd == 0 ? 0xFFFFFFFFu : (uint32_t)(i->loopStart - i->firstValidSample));
    wr32(A3, i->loopEnd == 0 ? 0xFFFFFFFFu : (uint32_t)(i->loopEnd - i->firstValidSample));
    return 0;
}

static uint32_t h_GetBitrate(CpuState *s) {
    Atrac *a = get_atrac(A0);
    uint32_t err = validate_data(a);
    if (err) return err;
    int br = (a->info.sampleSize * 352800) / 1000;
    br = a->info.codec == AT3_CODEC_AT3PLUS ? (int)(((uint32_t)(br >> 11) + 8) & 0xFFFFFFF0u) : (br + 511) >> 10;
    wr32(A1, (uint32_t)br);
    return 0;
}

static uint32_t h_GetChannel(CpuState *s) {
    Atrac *a = get_atrac(A0);
    uint32_t err = validate_data(a);
    if (err) return err;
    wr32(A1, a->info.numChan);
    return 0;
}

static uint32_t h_GetMaxSample(CpuState *s) {
    Atrac *a = get_atrac(A0);
    uint32_t err = validate_data(a);
    if (err) return err;
    wr32(A1, (uint32_t)spf(&a->info));
    return 0;
}

static uint32_t h_GetInternalErrorInfo(CpuState *s) {
    Atrac *a = get_atrac(A0);
    uint32_t err = validate_data(a);
    if (err) return err;
    wr32(A1, a->codecErr);
    return 0;
}

static uint32_t h_GetNextDecodePosition(CpuState *s) {
    Atrac *a = get_atrac(A0);
    uint32_t err = validate_data(a);
    if (err) return err;
    const AtracInfo *i = &a->info;
    if (i->decodePos > i->endSample || i->fileDataEnd - i->curFileOff < i->sampleSize) return ATRAC_ERR_ALL_DATA_DECODED;
    wr32(A1, (uint32_t)(i->decodePos - i->firstValidSample));
    return 0;
}

static uint32_t h_GetNextSample(CpuState *s) {
    Atrac *a = get_atrac(A0);
    uint32_t err = validate_data(a);
    if (err) return err;
    wr32(A1, (uint32_t)next_samples(&a->info));
    return 0;
}

/* Seek support: GetBufferInfoForResetting / ResetPlayPosition (PPSSPP Atrac2). */
typedef struct { uint32_t writePosPtr, writableBytes, minWriteBytes, filePos; } ResetInfo;

static void reset_info(const AtracInfo *i, ResetInfo *first, ResetInfo *second, int seekPos) {
    memset(first, 0, sizeof(*first));
    first->writePosPtr = i->buffer;
    switch (i->state) {
    case ST_HALFWAY_BUFFER: {
        const int streamPos = i->dataOff + i->streamDataByte;
        const int fileOff = i->dataOff + (seekPos / spf(i) + 1) * i->sampleSize;
        first->writePosPtr = i->buffer + (uint32_t)streamPos;
        first->writableBytes = (uint32_t)(i->fileDataEnd - streamPos);
        first->filePos = (uint32_t)streamPos;
        first->minWriteBytes = (uint32_t)imax(0, fileOff - streamPos);
        break;
    }
    case ST_STREAMED_WITHOUT_LOOP:
    case ST_STREAMED_LOOP_FROM_END: {
        const int cfo = file_offset_for(i, seekPos);
        first->writableBytes = (uint32_t)imin(i->fileDataEnd - cfo, round_down((int)i->bufferByte, i->sampleSize));
        first->minWriteBytes = (uint32_t)((skip_frames_for(i, seekPos) + 1) * i->sampleSize);
        first->filePos = (uint32_t)cfo;
        break;
    }
    case ST_STREAMED_LOOP_WITH_TRAILER: {
        const int sfo = file_offset_for(i, seekPos);
        const int le = loop_end_file_offset(i, i->loopEnd) - 1;
        const int bufEnd = round_down((int)i->bufferByte, i->sampleSize);
        const int skipBytes = (skip_frames_for(i, seekPos) + 1) * i->sampleSize;
        const int secEnd = round_down((int)i->secondBufferByte, i->sampleSize);
        if (sfo < le) {
            const int rem = (le - sfo) + 1;
            first->writableBytes = (uint32_t)imin(bufEnd, rem);
            first->minWriteBytes = (uint32_t)imin(skipBytes, rem);
            first->filePos = (uint32_t)sfo;
        } else if (le + secEnd <= sfo) {
            first->writableBytes = (uint32_t)imin(i->fileDataEnd - sfo, bufEnd);
            first->minWriteBytes = (uint32_t)skipBytes;
            first->filePos = (uint32_t)sfo;
        } else if (le + (int)i->secondBufferByte + 1 < i->fileDataEnd) {
            const int endOff = le + secEnd + 1;
            first->writableBytes = (uint32_t)imin(i->fileDataEnd - endOff, bufEnd);
            first->minWriteBytes = (uint32_t)imax(0, sfo + skipBytes - endOff);
            first->filePos = (uint32_t)endOff;
        }
        break;
    }
    default: break;   /* NO_DATA / ALL_DATA_LOADED: nothing to read */
    }
    second->writePosPtr = i->buffer; second->writableBytes = 0; second->minWriteBytes = 0; second->filePos = 0;
}

static uint32_t h_GetBufferInfoForResetting(CpuState *s) {
    Atrac *a = get_atrac(A0);
    uint32_t err = validate_managed(a);
    if (err) return err;
    if (!sr_inrange(A2)) return KERNEL_ERR_ILLEGAL_ADDR;
    const AtracInfo *i = &a->info;
    if (i->state == ST_STREAMED_LOOP_WITH_TRAILER && i->secondBufferByte == 0) return ATRAC_ERR_SECOND_BUFFER_NEEDED;
    const int seekPos = (int)A1 + i->firstValidSample;
    if ((uint32_t)seekPos > (uint32_t)i->endSample) return ATRAC_ERR_BAD_SAMPLE;
    ResetInfo f, sec;
    reset_info(i, &f, &sec, seekPos);
    const uint32_t p = A2;
    MEM_W32(p, f.writePosPtr); MEM_W32(p + 4, f.writableBytes); MEM_W32(p + 8, f.minWriteBytes); MEM_W32(p + 12, f.filePos);
    MEM_W32(p + 16, sec.writePosPtr); MEM_W32(p + 20, sec.writableBytes); MEM_W32(p + 24, sec.minWriteBytes); MEM_W32(p + 28, sec.filePos);
    int skip = 0;
    uint32_t r = skip_frames(a, &skip);
    if (skip) sched_delay_current(300);
    return r;
}

static uint32_t h_ResetPlayPosition(CpuState *s) {
    /* (id, sample, bytesWrittenFirstBuf, bytesWrittenSecondBuf) */
    Atrac *a = get_atrac(A0);
    uint32_t err = validate_managed(a);
    if (err) return err;
    AtracInfo *i = &a->info;
    if (i->state == ST_STREAMED_LOOP_WITH_TRAILER && i->secondBufferByte == 0) return ATRAC_ERR_SECOND_BUFFER_NEEDED;
    const int seekPos = (int)A1 + i->firstValidSample;
    if ((uint32_t)seekPos > (uint32_t)i->endSample) return ATRAC_ERR_BAD_SAMPLE;
    const int w1 = (int)A2, w2 = (int)A3;
    ResetInfo f, sec;
    reset_info(i, &f, &sec, seekPos);
    if ((uint32_t)w1 < f.minWriteBytes || (uint32_t)w1 > f.writableBytes) return ATRAC_ERR_BAD_FIRST_RESET_SIZE;
    if ((uint32_t)w2 < sec.minWriteBytes || (uint32_t)w2 > sec.writableBytes) return ATRAC_ERR_BAD_SECOND_RESET_SIZE;
    i->decodePos = seekPos;
    i->numSkipFrames = (uint8_t)skip_frames_for(i, seekPos);
    i->loopNum = 0;
    i->curFileOff = file_offset_for(i, seekPos);
    a->codecErr = 0x20b;
    switch (i->state) {
    case ST_HALFWAY_BUFFER:
        i->streamDataByte += w1;
        if (i->dataOff + i->streamDataByte >= i->fileDataEnd) i->state = ST_ALL_DATA_LOADED;
        break;
    case ST_STREAMED_WITHOUT_LOOP:
    case ST_STREAMED_LOOP_FROM_END:
        i->streamDataByte = w1; i->curBuffer = 0; i->streamOff = 0;
        break;
    case ST_STREAMED_LOOP_WITH_TRAILER: {
        const int le = loop_end_file_offset(i, i->loopEnd);
        if (i->curFileOff >= le) {
            const int secRounded = round_down((int)i->secondBufferByte, i->sampleSize);
            if (i->curFileOff < le + secRounded) {
                i->streamDataByte = (le + secRounded - i->curFileOff) + w1;
                i->curBuffer = 1;
                i->secondStreamOff = i->curFileOff - le;
            } else {
                i->streamDataByte = w1; i->curBuffer = 2; i->streamOff = 0;
            }
        } else {
            i->streamDataByte = w1; i->curBuffer = 0; i->streamOff = 0;
        }
        break;
    }
    default: break;
    }
    if (a->dec) at3dec_flush(a->dec);
    int skip = 0;
    uint32_t r = skip_frames(a, &skip);
    sched_delay_current(r ? (skip ? 200 : 0) : 3000);
    return r;
}

static uint32_t h_GetSecondBufferInfo(CpuState *s) {
    Atrac *a = get_atrac(A0);
    uint32_t err = validate_managed(a);
    if (err) return err;
    if (!sr_inrange(A1) || !sr_inrange(A2)) return KERNEL_ERR_ILLEGAL_ADDR;
    const AtracInfo *i = &a->info;
    if (i->state != ST_STREAMED_LOOP_WITH_TRAILER) {
        MEM_W32(A1, 0); MEM_W32(A2, 0);
        return ATRAC_ERR_SECOND_BUFFER_NOT_NEEDED;
    }
    const int le = loop_end_file_offset(i, i->loopEnd);
    MEM_W32(A1, (uint32_t)le);
    MEM_W32(A2, (uint32_t)(i->fileDataEnd - le));
    return 0;
}

static uint32_t h_SetSecondBuffer(CpuState *s) {
    Atrac *a = get_atrac(A0);
    uint32_t err = validate_managed(a);
    if (err) return err;
    AtracInfo *i = &a->info;
    const int le = loop_end_file_offset(i, i->loopEnd);
    if (i->sampleSize * 3 <= (int)A2 || i->fileDataEnd - le <= (int)A2) {
        if (i->state != ST_STREAMED_LOOP_WITH_TRAILER) return ATRAC_ERR_SECOND_BUFFER_NOT_NEEDED;
        i->secondBuffer = A1; i->secondBufferByte = A2; i->secondStreamOff = 0;
        return 0;
    }
    return ATRAC_ERR_SIZE_TOO_SMALL;
}

static uint32_t h_IsSecondBufferNeeded(CpuState *s) {
    Atrac *a = get_atrac(A0);
    uint32_t err = validate_managed(a);
    if (err) return err;
    return a->info.state == ST_STREAMED_LOOP_WITH_TRAILER;
}

static uint32_t h_Reinit(CpuState *s) {
    const int at3Count = (int)A0, at3plusCount = (int)A1;
    for (int i = 0; i < MAX_ATRAC; i++) if (s_atrac[i].used) return KERNEL_ERR_BUSY;
    memset(s_ctxType, 0, sizeof(s_ctxType));
    if (at3Count == 0 && at3plusCount == 0) { s_inited = 0; sched_delay_current(200); return 0; }
    int next = 0, space = MAX_ATRAC;
    for (int i = 0; i < at3plusCount; i++) { space -= 2; if (space >= 0) s_ctxType[next++] = AT3_CODEC_AT3PLUS; }
    for (int i = 0; i < at3Count; i++)     { space -= 1; if (space >= 0) s_ctxType[next++] = AT3_CODEC_AT3; }
    uint32_t result = space >= 0 ? 0 : KERNEL_ERR_OUT_OF_MEMORY;
    ALOG("ATRAC reinit at3=%d at3plus=%d -> %08x\n", at3Count, at3plusCount, result);
    if (!s_inited && next) sched_delay_current(400);
    s_inited = 1;
    return result;
}

static uint32_t h_ok(CpuState *s) { (void)s; return 0; }

void sr_hle_init_atrac(void) {
    const char *e = getenv("SR_ATRACLOG");
    s_log = e ? atoi(e) : 0;
    sr_hle_register(0x7db31251, "sceAtracAddStreamData", h_AddStreamData);
    sr_hle_register(0x6a8c3cd5, "sceAtracDecodeData", h_DecodeData);
    sr_hle_register(0xd5c28cc0, "sceAtracReleaseResources", h_ok);
    sr_hle_register(0x780f88d1, "sceAtracGetAtracID", h_GetAtracID);
    sr_hle_register(0xca3ca3d2, "sceAtracGetBufferInfoForReseting", h_GetBufferInfoForResetting);
    sr_hle_register(0x2dd3e298, "sceAtracGetBufferInfoForResetting", h_GetBufferInfoForResetting);
    sr_hle_register(0xa554a158, "sceAtracGetBitrate", h_GetBitrate);
    sr_hle_register(0x31668baa, "sceAtracGetChannel", h_GetChannel);
    sr_hle_register(0xfaa4f89b, "sceAtracGetLoopStatus", h_GetLoopStatus);
    sr_hle_register(0xe88f759b, "sceAtracGetInternalErrorInfo", h_GetInternalErrorInfo);
    sr_hle_register(0xd6a5f2f7, "sceAtracGetMaxSample", h_GetMaxSample);
    sr_hle_register(0xe23e3a35, "sceAtracGetNextDecodePosition", h_GetNextDecodePosition);
    sr_hle_register(0x36faabfb, "sceAtracGetNextSample", h_GetNextSample);
    sr_hle_register(0x9ae849a7, "sceAtracGetRemainFrame", h_GetRemainFrame);
    sr_hle_register(0x83e85ea0, "sceAtracGetSecondBufferInfo", h_GetSecondBufferInfo);
    sr_hle_register(0xa2bba8be, "sceAtracGetSoundSample", h_GetSoundSample);
    sr_hle_register(0x5d268707, "sceAtracGetStreamDataInfo", h_GetStreamDataInfo);
    sr_hle_register(0x61eb33f5, "sceAtracReleaseAtracID", h_ReleaseAtracID);
    sr_hle_register(0x644e5607, "sceAtracResetPlayPosition", h_ResetPlayPosition);
    sr_hle_register(0x3f6e26b5, "sceAtracSetHalfwayBuffer", h_SetHalfwayBuffer);
    sr_hle_register(0x83bf7afd, "sceAtracSetSecondBuffer", h_SetSecondBuffer);
    sr_hle_register(0x0e2a73ab, "sceAtracSetData", h_SetData);
    sr_hle_register(0x7a20e7af, "sceAtracSetDataAndGetID", h_SetDataAndGetID);
    sr_hle_register(0x868120b5, "sceAtracSetLoopNum", h_SetLoopNum);
    sr_hle_register(0x132f1eca, "sceAtracReinit", h_Reinit);
    sr_hle_register(0xeca32a99, "sceAtracIsSecondBufferNeeded", h_IsSecondBufferNeeded);
    sr_hle_register(0x0fae370e, "sceAtracSetHalfwayBufferAndGetID", h_SetHalfwayBufferAndGetID);
}
