/* sceSasCore: the PSP's software mixer (it runs on the Media Engine), HLE'd.
 *
 * A C port of PPSSPP's Core/HW/SasAudio.cpp (voices, VAG ADPCM decoder, ADSR envelope, mixing),
 * Core/HW/SasReverb.cpp (the PSX-SPU-style reverb with the PSP's presets) and the entry points
 * in Core/HLE/sceSas.cpp. All GPL-2.0+. The ATRAC3 voice type isn't ported; this game doesn't
 * import __sceSasSetVoiceATRAC3.
 *
 * The game (Square Enix's CDevSd sound driver) runs one AUDIO MIXER thread that calls
 * __sceSasCoreWithMix every grain and hands the result to sceAudioOutput2OutputBlocking.
 * Movie audio arrives as a PCM voice.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define A0 (s->r[4])
#define A1 (s->r[5])
#define A2 (s->r[6])
#define A3 (s->r[7])
#define T0 (s->r[8])
#define T1 (s->r[9])
#define T2 (s->r[10])

void sr_hle_register(uint32_t nid, const char *name, HleFn fn);

enum {
    SAS_ERR_INVALID_GRAIN = 0x80420001, SAS_ERR_INVALID_MAX_VOICES = 0x80420002,
    SAS_ERR_INVALID_OUTPUT_MODE = 0x80420003, SAS_ERR_INVALID_SAMPLE_RATE = 0x80420004,
    SAS_ERR_BAD_ADDRESS = 0x80420005, SAS_ERR_INVALID_VOICE = 0x80420010,
    SAS_ERR_INVALID_NOISE_FREQ = 0x80420011, SAS_ERR_INVALID_PITCH = 0x80420012,
    SAS_ERR_INVALID_ADSR_CURVE_MODE = 0x80420013, SAS_ERR_INVALID_PARAMETER = 0x80420014,
    SAS_ERR_INVALID_LOOP_POS = 0x80420015, SAS_ERR_VOICE_PAUSED = 0x80420016,
    SAS_ERR_INVALID_VOLUME = 0x80420018, SAS_ERR_INVALID_ADSR_RATE = 0x80420019,
    SAS_ERR_INVALID_PCM_SIZE = 0x8042001A, SAS_ERR_REV_INVALID_TYPE = 0x80420020,
    SAS_ERR_REV_INVALID_FEEDBACK = 0x80420021, SAS_ERR_REV_INVALID_DELAY = 0x80420022,
    SAS_ERR_REV_INVALID_VOLUME = 0x80420023, SAS_ERR_ATRAC3_ALREADY_SET = 0x80420040,
    KERNEL_ERR_BAD_ARGUMENT = 0x80000004,
};

enum {
    VOICES_MAX = 32, VOL_MAX = 0x1000, MAX_GRAIN = 2048,
    PITCH_BASE = 0x1000, PITCH_MASK = 0xFFF, PITCH_SHIFT = 12, PITCH_MAX = 0x4000,
    ENV_HEIGHT_MAX = 0x40000000,
};
enum { CURVE_LINEAR_INC = 0, CURVE_LINEAR_DEC = 1, CURVE_LINEAR_BENT = 2, CURVE_EXP_DEC = 3, CURVE_EXP_INC = 4, CURVE_DIRECT = 5 };
enum { ST_KEYON_STEP = -42, ST_KEYON = -2, ST_OFF = -1, ST_ATTACK = 0, ST_DECAY = 1, ST_SUSTAIN = 2, ST_RELEASE = 3 };
enum { VT_OFF, VT_VAG, VT_NOISE, VT_TRIWAVE, VT_PULSEWAVE, VT_PCM, VT_ATRAC3 };
enum { EFFECT_OFF = -1, EFFECT_ECHO = 6, EFFECT_DELAY = 7, EFFECT_MAX = 8 };

static inline int16_t clamp_s16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

/* ---- VAG ADPCM (PPSSPP VagDecoder) ---------------------------------------------------- */

static const uint8_t vag_f[16][2] = {
    {   0,   0 }, {  60,   0 }, { 115,  52 }, {  98,  55 }, { 122,  60 },
    /* past index 4 the hardware reads off the end of its own table */
    {   0,   0 }, {   0,   0 }, {  52,   0 }, {  55,   2 }, {  60, 125 },
    {   0,   0 }, {   0,  91 }, {   0,   0 }, {   2, 216 }, { 125,   6 }, {   0, 151 },
};

typedef struct {
    int16_t samples[28];
    int curSample;
    uint32_t data, read;
    int curBlock, loopStartBlock, numBlocks;
    int s1, s2;
    int loopEnabled, loopAtNextBlock, end;
} VagDecoder;

static void vag_start(VagDecoder *v, uint32_t data, uint32_t size, int loop) {
    v->loopEnabled = loop; v->loopAtNextBlock = 0; v->loopStartBlock = -1;
    v->numBlocks = (int)(size / 16); v->end = 0;
    v->data = data; v->read = data;
    v->curSample = 28; v->curBlock = -1;
    v->s1 = 0; v->s2 = 0;
}

static void vag_decode_block(VagDecoder *v, const uint8_t **rp) {
    if (v->curBlock == v->numBlocks - 1) { v->end = 1; return; }
    const uint8_t *p = *rp;
    int predict = *p++;
    const int shift = predict & 0xf;
    predict >>= 4;
    const int flags = *p++;
    if (flags == 7) { v->end = 1; return; }
    if (flags == 6) v->loopStartBlock = v->curBlock;
    else if (flags == 3 && v->loopEnabled) v->loopAtNextBlock = 1;
    int s1 = v->s1, s2 = v->s2;
    const int c1 = vag_f[predict][0], c2 = -vag_f[predict][1];
    for (int i = 0; i < 28; i += 2) {
        const uint8_t d = *p++;
        const int a = (int16_t)((d & 0xf) << 12) >> shift;
        const int b = (int16_t)((d & 0xf0) << 8) >> shift;
        s2 = clamp_s16(a + ((s1 * c1 + s2 * c2) >> 6));
        s1 = clamp_s16(b + ((s2 * c1 + s1 * c2) >> 6));
        v->samples[i] = (int16_t)s2;
        v->samples[i + 1] = (int16_t)s1;
    }
    v->s1 = s1; v->s2 = s2;
    v->curSample = 0;
    v->curBlock++;
    *rp = p;
}

static void vag_get_samples(VagDecoder *v, int16_t *out, int n) {
    if (v->end) { memset(out, 0, (size_t)n * 2); return; }
    if (!sr_inrange(v->read) || !sr_inrange(v->data + (uint32_t)v->numBlocks * 16 - 1)) { memset(out, 0, (size_t)n * 2); return; }
    const uint8_t *rp = (const uint8_t *)SR_HOST(v->read), *orig = rp;
    for (int i = 0; i < n; i++) {
        if (v->curSample == 28) {
            if (v->loopAtNextBlock) {
                v->read = v->data + 16 * (uint32_t)v->loopStartBlock + 16;
                rp = orig = (const uint8_t *)SR_HOST(v->read);
                v->curBlock = v->loopStartBlock;
                v->loopAtNextBlock = 0;
            }
            vag_decode_block(v, &rp);
            if (v->end) { memset(&out[i], 0, (size_t)(n - i) * 2); return; }
        }
        out[i] = v->samples[v->curSample++];
    }
    if (rp > orig) v->read += (uint32_t)(rp - orig);
}

/* ---- ADSR envelope (PPSSPP ADSREnvelope) ---------------------------------------------- */

typedef struct {
    int attackRate, decayRate, sustainRate, sustainLevel, releaseRate;
    int attackType, decayType, sustainType, releaseType;
    int state;
    int64_t height;
} Envelope;

static int simple_rate(int n) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    int r = (int)(((uint32_t)(7 - (n & 3)) << 26) >> (n >> 2));
    return r ? r : 1;
}
static int exponent_rate(int n) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    int r = (int)(((uint32_t)(7 - (n & 3)) << 24) >> (n >> 2));
    return r ? r : 1;
}

static void env_set_simple(Envelope *e, uint32_t env1, uint32_t env2) {
    e->attackRate = simple_rate((int)(env1 >> 8));
    e->attackType = (env1 & 0x8000) == 0 ? CURVE_LINEAR_INC : CURVE_LINEAR_BENT;
    const int dn = (int)((env1 >> 4) & 0xF);
    e->decayRate = dn == 0 ? 0x7FFFFFFF : (int)(0x80000000u >> dn);
    e->decayType = CURVE_EXP_DEC;
    e->sustainType = (int)((env2 >> 14) & 3);
    e->sustainRate = e->sustainType == CURVE_EXP_DEC ? exponent_rate((int)(env2 >> 6)) : simple_rate((int)(env2 >> 6));
    e->releaseType = (env2 & 0x20) == 0 ? CURVE_LINEAR_DEC : CURVE_EXP_DEC;
    const int rn = (int)(env2 & 0x1F);
    if (rn == 31) e->releaseRate = 0;
    else if (e->releaseType == CURVE_LINEAR_DEC) e->releaseRate = rn == 30 ? 0x40000000 : rn == 29 ? 1 : (int)(0x10000000u >> rn);
    else e->releaseRate = rn == 0 ? 0x7FFFFFFF : (int)(0x80000000u >> rn);
    e->sustainLevel = (int)(((env1 & 0xF) + 1) << 26);
}

static void env_set_state(Envelope *e, int st) {
    if (e->height > ENV_HEIGHT_MAX) e->height = ENV_HEIGHT_MAX;
    e->state = st;
}

static void env_walk(Envelope *e, int type, int rate) {
    int64_t d;
    switch (type) {
    case CURVE_LINEAR_INC: e->height += rate; break;
    case CURVE_LINEAR_DEC: e->height -= rate; break;
    case CURVE_LINEAR_BENT: e->height += e->height <= (int64_t)ENV_HEIGHT_MAX * 3 / 4 ? rate : rate / 4; break;
    case CURVE_EXP_DEC:
        d = e->height - ENV_HEIGHT_MAX;
        d += (-d * rate) >> 32;
        e->height = d + ENV_HEIGHT_MAX - (int64_t)(((uint64_t)(uint32_t)rate + 3u) / 4u);
        break;
    case CURVE_EXP_INC:
        d = e->height - ENV_HEIGHT_MAX;
        d += (-d * rate) >> 32;
        e->height = d + 0x4000 + ENV_HEIGHT_MAX;
        break;
    case CURVE_DIRECT: e->height = rate; break;
    }
}

static void env_step(Envelope *e) {
    switch (e->state) {
    case ST_ATTACK:
        env_walk(e, e->attackType, e->attackRate);
        if (e->height >= ENV_HEIGHT_MAX || e->height < 0) env_set_state(e, ST_DECAY);
        break;
    case ST_DECAY:
        env_walk(e, e->decayType, e->decayRate);
        if (e->height < e->sustainLevel) env_set_state(e, ST_SUSTAIN);
        break;
    case ST_SUSTAIN:
        env_walk(e, e->sustainType, e->sustainRate);
        if (e->height <= 0) { e->height = 0; env_set_state(e, ST_RELEASE); }
        break;
    case ST_RELEASE:
        env_walk(e, e->releaseType, e->releaseRate);
        if (e->height <= 0) { e->height = 0; env_set_state(e, ST_OFF); }
        break;
    case ST_KEYON:
        e->height = 0;
        env_set_state(e, ST_KEYON_STEP);
        break;
    case ST_KEYON_STEP:   /* 32 steps at 0 before attack starts (matches hardware tests) */
        e->height++;
        if (e->height >= 31) { e->height = 0; env_set_state(e, ST_ATTACK); }
        break;
    default: break;
    }
}

static int env_height(const Envelope *e) { return (int)(e->height > ENV_HEIGHT_MAX ? ENV_HEIGHT_MAX : e->height); }

/* ---- voices and mixing (PPSSPP SasVoice / SasInstance) -------------------------------- */

typedef struct {
    int playing, paused, on;
    int type;
    uint32_t vagAddr; int vagSize;
    uint32_t pcmAddr; int pcmSize, pcmIndex, pcmLoopPos;
    uint32_t sampleFrac;
    int pitch, loop, noiseFreq;
    int volumeLeft, volumeRight, effectLeft, effectRight;
    int16_t resampleHist[2];
    Envelope env;
    VagDecoder vag;
} Voice;

/* Reverb (PPSSPP SasReverb). */
typedef struct {
    int32_t size;
    int16_t dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL;
    int16_t vAPF1, vAPF2, mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2;
    int16_t dLSAME, dRSAME, mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4;
    int16_t dLDIFF, dRDIFF, mLAPF1, mRAPF1, mLAPF2, mRAPF2;
} RevPreset;

#define S16(x) ((int16_t)(x))
static const RevPreset rev_presets[9] = {
    { 0x26C0, 0x007D,0x005B,0x6D80,0x54B8,S16(0xBED0),0x0000,0x0000,S16(0xBA80),
      0x5800,0x5300,0x04D6,0x0333,0x03F0,0x0227,0x0374,0x01EF,
      0x0336,0x01B7,0x0335,0x01B6,0x0334,0x01B5,0x0334,0x01B5,
      0x0334,0x01B5,0x01B4,0x0136,0x00B8,0x005C },                          /* Room */
    { 0x1F40, 0x0033,0x0025,0x70F0,0x4FA8,S16(0xBCE0),0x4410,S16(0xC0F0),S16(0x9C00),
      0x5280,0x4EC0,0x03E4,0x031B,0x03A4,0x02AF,0x0372,0x0266,
      0x031C,0x025D,0x025C,0x018E,0x022F,0x0135,0x01D2,0x00B7,
      0x018F,0x00B5,0x00B4,0x0080,0x004C,0x0026 },                          /* Studio Small */
    { 0x4840, 0x00B1,0x007F,0x70F0,0x4FA8,S16(0xBCE0),0x4510,S16(0xBEF0),S16(0xB4C0),
      0x5280,0x4EC0,0x0904,0x076B,0x0824,0x065F,0x07A2,0x0616,
      0x076C,0x05ED,0x05EC,0x042E,0x050F,0x0305,0x0462,0x02B7,
      0x042F,0x0265,0x0264,0x01B2,0x0100,0x0080 },                          /* Studio Medium */
    { 0x6FE0, 0x00E3,0x00A9,0x6F60,0x4FA8,S16(0xBCE0),0x4510,S16(0xBEF0),S16(0xA680),
      0x5680,0x52C0,0x0DFB,0x0B58,0x0D09,0x0A3C,0x0BD9,0x0973,
      0x0B59,0x08DA,0x08D9,0x05E9,0x07EC,0x04B0,0x06EF,0x03D2,
      0x05EA,0x031D,0x031C,0x0238,0x0154,0x00AA },                          /* Studio Large */
    { 0xADE0, 0x01A5,0x0139,0x6000,0x5000,0x4C00,S16(0xB800),S16(0xBC00),S16(0xC000),
      0x6000,0x5C00,0x15BA,0x11BB,0x14C2,0x10BD,0x11BC,0x0DC1,
      0x11C0,0x0DC3,0x0DC0,0x09C1,0x0BC4,0x07C1,0x0A00,0x06CD,
      0x09C2,0x05C1,0x05C0,0x041A,0x0274,0x013A },                          /* Hall */
    { 0xF6C0, 0x033D,0x0231,0x7E00,0x5000,S16(0xB400),S16(0xB000),0x4C00,S16(0xB000),
      0x6000,0x5400,0x1ED6,0x1A31,0x1D14,0x183B,0x1BC2,0x16B2,
      0x1A32,0x15EF,0x15EE,0x1055,0x1334,0x0F2D,0x11F6,0x0C5D,
      0x1056,0x0AE1,0x0AE0,0x07A2,0x0464,0x0232 },                          /* Space Echo */
    { 0x18040, 0x0003,0x0003,0x7FFF,0x7FFF,0x0000,0x0000,0x0000,S16(0x8100),
      0x0000,0x0000,0x1FFD,0x0FFD,0x1009,0x0009,0x0000,0x0000,
      0x1009,0x0009,0x1FFF,0x1FFF,0x1FFE,0x1FFE,0x1FFE,0x1FFE,
      0x1FFE,0x1FFE,0x1008,0x1004,0x0008,0x0004 },                          /* Echo */
    { 0x18040, 0x0003,0x0003,0x7FFF,0x7FFF,0x0000,0x0000,0x0000,0x0000,
      0x0000,0x0000,0x1FFD,0x0FFD,0x1009,0x0009,0x0000,0x0000,
      0x1009,0x0009,0x1FFF,0x1FFF,0x1FFE,0x1FFE,0x1FFE,0x1FFE,
      0x1FFE,0x1FFE,0x1008,0x1004,0x0008,0x0004 },                          /* Delay */
    { 0x3C00, 0x0017,0x0013,0x70F0,0x4FA8,S16(0xBCE0),0x4510,S16(0xBEF0),S16(0x8500),
      0x5F80,0x54C0,0x0371,0x02AF,0x02E5,0x01DF,0x02B0,0x01D7,
      0x0358,0x026A,0x01D6,0x011E,0x012D,0x00B1,0x011F,0x0059,
      0x01A0,0x00E3,0x0058,0x0040,0x0028,0x0014 },                          /* Half Echo */
};

#define REV_BUFSIZE 0x20000
static int16_t s_revWork[REV_BUFSIZE];
static RevPreset s_rev;
static int s_revPreset = -1, s_revPos = 0, s_revDelay = 0, s_revFeedback = 0;

static void rev_apply_params(void) {
    if (s_revPreset == -1) return;
    s_rev = rev_presets[s_revPreset];
    if (s_revPreset != EFFECT_ECHO && s_revPreset != EFFECT_DELAY) return;
    const int d16 = (s_revDelay + 1) * 16;
    s_rev.mLSAME = (int16_t)(d16 * 2 - s_rev.dAPF1);
    s_rev.mRSAME = (int16_t)(d16 - s_rev.dAPF2);
    s_rev.mLCOMB1 = (int16_t)(s_rev.mRCOMB1 + d16);
    s_rev.dLSAME = (int16_t)(s_rev.dRSAME + d16);
    s_rev.mLAPF1 = (int16_t)(s_rev.mLAPF2 + d16);
    s_rev.mRAPF1 = (int16_t)(d16 + (s_rev.mLAPF2 >> 1));
    s_rev.vWALL = (int16_t)(-(s_revFeedback << 8));
}

static void rev_set_preset(int preset) {
    if (preset >= -1 && preset < 9) s_revPreset = preset;
    if (s_revPreset != -1) {
        s_revPos = REV_BUFSIZE - rev_presets[s_revPreset].size;
        memset(s_revWork, 0, sizeof(s_revWork));
    } else {
        s_revPos = 0;
    }
    rev_apply_params();
}

/* Index into the upper part of the workspace, wrapping inside the preset's size. */
static inline int16_t *rb(int index) {
    const int size = s_rev.size, base = REV_BUFSIZE - size;
    int addr = s_revPos + index;
    if (addr >= REV_BUFSIZE) addr -= size;
    if (addr < base) addr += size;
    return &s_revWork[addr];
}

static void rev_process(int16_t *out, const int16_t *in, int n, int volL, int volR) {
    if (s_revPreset == -1) {
        for (int i = 0; i < n; i++) {
            out[i * 4 + 0] = clamp_s16((int)in[i * 2 + 0] * volL >> 15);
            out[i * 4 + 1] = clamp_s16((int)in[i * 2 + 1] * volR >> 15);
            out[i * 4 + 2] = clamp_s16((int)in[i * 2 + 0] * volL >> 15);
            out[i * 4 + 3] = clamp_s16((int)in[i * 2 + 1] * volR >> 15);
        }
        return;
    }
    volL <<= 1; volR <<= 1;    /* the ME's reverb return is (evol * out) >> 11 */
    const RevPreset *d = &s_rev;
    for (int i = 0; i < n; i++) {
        const int16_t Lin = (int16_t)(in[i * 2] >> 2), Rin = (int16_t)(in[i * 2 + 1] >> 2);
        *rb(d->mLSAME) = clamp_s16(Lin + (*rb(d->dLSAME) * d->vWALL >> 15) - (*rb(d->mLSAME - 1) * d->vIIR >> 15) + *rb(d->mLSAME - 1));
        *rb(d->mRSAME) = clamp_s16(Rin + (*rb(d->dRSAME) * d->vWALL >> 15) - (*rb(d->mRSAME - 1) * d->vIIR >> 15) + *rb(d->mRSAME - 1));
        *rb(d->mLDIFF) = clamp_s16(Lin + (*rb(d->dRDIFF) * d->vWALL >> 15) - (*rb(d->mLDIFF - 1) * d->vIIR >> 15) + *rb(d->mLDIFF - 1));
        *rb(d->mRDIFF) = clamp_s16(Rin + (*rb(d->dLDIFF) * d->vWALL >> 15) - (*rb(d->mRDIFF - 1) * d->vIIR >> 15) + *rb(d->mRDIFF - 1));
        int32_t Lout = (d->vCOMB1 * *rb(d->mLCOMB1) + d->vCOMB2 * *rb(d->mLCOMB2) + d->vCOMB3 * *rb(d->mLCOMB3) + d->vCOMB4 * *rb(d->mLCOMB4)) >> 15;
        int32_t Rout = (d->vCOMB1 * *rb(d->mRCOMB1) + d->vCOMB2 * *rb(d->mRCOMB2) + d->vCOMB3 * *rb(d->mRCOMB3) + d->vCOMB4 * *rb(d->mRCOMB4)) >> 15;
        *rb(d->mLAPF1) = clamp_s16(Lout - (d->vAPF1 * *rb(d->mLAPF1 - d->dAPF1) >> 15));
        Lout = *rb(d->mLAPF1 - d->dAPF1) + (*rb(d->mLAPF1) * d->vAPF1 >> 15);
        *rb(d->mRAPF1) = clamp_s16(Rout - (d->vAPF1 * *rb(d->mRAPF1 - d->dAPF1) >> 15));
        Rout = *rb(d->mRAPF1 - d->dAPF1) + (*rb(d->mRAPF1) * d->vAPF1 >> 15);
        *rb(d->mLAPF2) = clamp_s16(Lout - (d->vAPF2 * *rb(d->mLAPF2 - d->dAPF2) >> 15));
        Lout = *rb(d->mLAPF2 - d->dAPF2) + (*rb(d->mLAPF2) * d->vAPF2 >> 15);
        *rb(d->mRAPF2) = clamp_s16(Rout - (d->vAPF2 * *rb(d->mRAPF2 - d->dAPF2) >> 15));
        Rout = *rb(d->mRAPF2 - d->dAPF2) + (*rb(d->mRAPF2) * d->vAPF2 >> 15);
        out[i * 4 + 0] = clamp_s16((Lout * volL) >> 15);
        out[i * 4 + 1] = clamp_s16((Rout * volR) >> 15);
        out[i * 4 + 2] = 0;
        out[i * 4 + 3] = 0;
        if (++s_revPos >= REV_BUFSIZE) s_revPos -= s_rev.size;
    }
}

static Voice s_v[VOICES_MAX];
static int s_grain = 0, s_outputMode = 0;
static int32_t s_mix[MAX_GRAIN * 2], s_send[MAX_GRAIN * 2];
static int16_t s_sendDown[MAX_GRAIN], s_sendProc[MAX_GRAIN * 2];
static int16_t s_mixTemp[MAX_GRAIN * 4 + 2 + 16];
static struct { int type, delay, feedback, leftVol, rightVol, isDryOn, isWetOn; } s_fx = { EFFECT_OFF, 0, 0, 0, 0, 1, 0 };

static void voice_read(Voice *v, int16_t *out, int n) {
    switch (v->type) {
    case VT_VAG: vag_get_samples(&v->vag, out, n); break;
    case VT_PCM: {
        int needed = n;
        while (needed > 0) {
            int size = v->pcmSize - v->pcmIndex;
            if (size > needed) size = needed;
            if (!v->on) { v->pcmIndex = 0; break; }
            const uint32_t src = v->pcmAddr + (uint32_t)v->pcmIndex * 2;
            if (sr_inrange(src) && sr_inrange(src + (uint32_t)size * 2 - 1)) memcpy(out, SR_HOST(src), (size_t)size * 2);
            else memset(out, 0, (size_t)size * 2);
            v->pcmIndex += size; needed -= size; out += size;
            if (v->pcmIndex >= v->pcmSize) {
                if (!v->loop) break;
                v->pcmIndex = v->pcmLoopPos;
            }
        }
        if (needed > 0) memset(out, 0, (size_t)needed * 2);
        break;
    }
    default: memset(out, 0, (size_t)n * 2); break;   /* noise/waves: silent in PPSSPP too */
    }
}

static int voice_ended(const Voice *v) {
    switch (v->type) {
    case VT_VAG: return v->vag.end;
    case VT_PCM: return v->pcmIndex >= v->pcmSize;
    default: return 0;
    }
}

static void mix_voice(Voice *v) {
    if (v->type == VT_VAG && !v->vagAddr) return;
    if (v->type == VT_PCM && !v->pcmAddr) return;
    int delay = 0;
    const int keyon = v->env.state == ST_KEYON;
    if (keyon) {
        const int ignorePitch = v->type == VT_PCM && v->pitch > PITCH_BASE;
        delay = ignorePitch ? 32 : (int)((32 * (uint32_t)v->pitch) >> PITCH_SHIFT);
        if (v->type == VT_VAG) ++delay;
    }
    s_mixTemp[0] = v->resampleHist[0];
    s_mixTemp[1] = v->resampleHist[1];
    const int pitch = v->pitch;
    uint32_t frac = v->sampleFrac;
    int toRead = (int)((frac + (uint32_t)pitch * (uint32_t)(s_grain - delay > 0 ? s_grain - delay : 0)) >> PITCH_SHIFT);
    const int tempMax = (int)(sizeof(s_mixTemp) / sizeof(s_mixTemp[0])) - 2;
    if (toRead > tempMax) toRead = tempMax;
    int readPos = 2;
    if (keyon) { readPos = 0; toRead += 2; }
    voice_read(v, &s_mixTemp[readPos], toRead);
    const int tempPos = readPos + toRead;
    for (int i = 0; i < delay; ++i) env_step(&v->env);
    const int interp = pitch != PITCH_BASE || (frac & PITCH_MASK) != 0;
    for (int i = delay; i < s_grain; i++) {
        const int16_t *sp = s_mixTemp + (frac >> PITCH_SHIFT);
        int sample = sp[0];
        if (interp) { const int f = (int)(frac & PITCH_MASK); sample = sp[0] - (((sp[0] - sp[1]) * f) >> PITCH_SHIFT); }
        frac += (uint32_t)pitch;
        int envv = env_height(&v->env);
        env_step(&v->env);
        envv = (envv + (1 << 14)) >> 15;
        sample = ((sample * envv) + (1 << 14)) >> 15;
        s_mix[i * 2]      += (sample * v->volumeLeft) >> 12;
        s_mix[i * 2 + 1]  += (sample * v->volumeRight) >> 12;
        s_send[i * 2]     += sample * v->effectLeft >> 12;
        s_send[i * 2 + 1] += sample * v->effectRight >> 12;
    }
    v->resampleHist[0] = s_mixTemp[tempPos - 2];
    v->resampleHist[1] = s_mixTemp[tempPos - 1];
    v->sampleFrac = frac - (uint32_t)(tempPos - 2) * PITCH_BASE;
    if (voice_ended(v)) { env_set_state(&v->env, ST_OFF); v->env.height = 0; }
    if (v->env.state == ST_OFF) { v->playing = 0; v->on = 0; }
}

static void sas_mix(uint32_t outAddr, uint32_t inAddr, int leftVol, int rightVol) {
    for (int i = 0; i < VOICES_MAX; i++) if (s_v[i].playing && !s_v[i].paused) mix_voice(&s_v[i]);
    int16_t *outp = (int16_t *)SR_HOST(outAddr);
    if (s_outputMode == 0) {
        const int dry = s_fx.isDryOn != 0, wet = s_fx.isWetOn != 0;
        if (wet) {
            for (int i = 0; i < s_grain / 2; i++) {
                s_sendDown[i * 2] = clamp_s16(s_send[i * 4]);
                s_sendDown[i * 2 + 1] = clamp_s16(s_send[i * 4 + 1]);
            }
            rev_process(s_sendProc, s_sendDown, s_grain / 2, s_fx.leftVol << 3, s_fx.rightVol << 3);
        }
        const int16_t *inp = inAddr ? (const int16_t *)SR_HOST(inAddr) : NULL;
        for (int i = 0; i < s_grain * 2; i += 2) {
            int l = 0, r = 0;
            if (inp) { l = (inp[i] * leftVol) >> 12; r = (inp[i + 1] * rightVol) >> 12; }
            if (dry) { l += s_mix[i]; r += s_mix[i + 1]; }
            if (wet) { l += s_sendProc[i]; r += s_sendProc[i + 1]; }
            outp[i] = clamp_s16(l);
            outp[i + 1] = clamp_s16(r);
        }
    } else {
        int16_t *oL = outp, *oR = outp + s_grain, *sL = outp + s_grain * 2, *sR = outp + s_grain * 3;
        for (int i = 0; i < s_grain * 2; i += 2) {
            *oL++ = clamp_s16(s_mix[i]); *oR++ = clamp_s16(s_mix[i + 1]);
            *sL++ = clamp_s16(s_send[i]); *sR++ = clamp_s16(s_send[i + 1]);
        }
    }
    memset(s_mix, 0, sizeof(int32_t) * (size_t)s_grain * 2);
    memset(s_send, 0, sizeof(int32_t) * (size_t)s_grain * 2);
}

/* PPSSPP SasInstance::EstimateMixUs: the thread waits this long for the mix. */
static uint32_t mix_us(void) {
    int n = 0;
    for (int i = 0; i < VOICES_MAX; i++) if (s_v[i].playing && !s_v[i].paused) n++;
    int c = 20 + n * 68 + (s_grain * 60) / 100;
    return (uint32_t)(c < 1200 ? c : 1200);
}

/* ---- entry points (PPSSPP sceSas.cpp) ------------------------------------------------- */

#define VOICE_OR_ERR(idx) if ((int)(idx) < 0 || (int)(idx) >= VOICES_MAX) return SAS_ERR_INVALID_VOICE; Voice *v = &s_v[(int)(idx)]

static uint32_t h_Init(CpuState *s) {
    /* (core, grainSize, maxVoices, outputMode, sampleRate) */
    const uint32_t core = A0, grain = A1, maxVoices = A2, outputMode = A3, rate = T0;
    if (!sr_inrange(core) || (core & 0x3F)) return SAS_ERR_BAD_ADDRESS;
    if (maxVoices == 0 || maxVoices > VOICES_MAX) return SAS_ERR_INVALID_MAX_VOICES;
    if (grain < 0x40 || grain > 0x800 || (grain & 0x1F)) return SAS_ERR_INVALID_GRAIN;
    if (outputMode > 1) return SAS_ERR_INVALID_OUTPUT_MODE;
    if (rate != 44100) return SAS_ERR_INVALID_SAMPLE_RATE;
    s_grain = (int)grain;
    s_outputMode = (int)outputMode;
    memset(s_v, 0, sizeof(s_v));
    for (int i = 0; i < VOICES_MAX; i++) {
        Voice *v = &s_v[i];
        v->pitch = PITCH_BASE;
        v->volumeLeft = v->volumeRight = v->effectLeft = v->effectRight = VOL_MAX;
        v->env.state = ST_OFF;
        v->env.decayType = v->env.sustainType = v->env.releaseType = CURVE_LINEAR_DEC;
        v->vag.end = 1;
    }
    memset(s_mix, 0, sizeof(s_mix)); memset(s_send, 0, sizeof(s_send));
    return 0;
}

static uint32_t h_Core(CpuState *s) {
    if (!sr_inrange(A1) || !s_grain) return SAS_ERR_INVALID_PARAMETER;
    sas_mix(A1, 0, 0, 0);
    sched_delay_current(mix_us());
    return 0;
}

static uint32_t h_CoreWithMix(CpuState *s) {
    /* (core, inoutAddr, leftVolume, rightVolume) */
    if (!sr_inrange(A1) || !s_grain) return SAS_ERR_INVALID_PARAMETER;
    if (s_outputMode == 1) return KERNEL_ERR_BAD_ARGUMENT;
    sas_mix(A1, A1, (int)A2, (int)A3);
    sched_delay_current(mix_us());
    return 0;
}

static uint32_t h_GetEndFlag(CpuState *s) {
    (void)s;
    uint32_t m = 0;
    for (int i = 0; i < VOICES_MAX; i++) if (!s_v[i].playing) m |= 1u << i;
    return m;
}

static uint32_t h_SetVoice(CpuState *s) {
    /* (core, voice, vagAddr, size, loop) */
    VOICE_OR_ERR(A1);
    int size = (int)A3, loop = (int)T0;
    if (size == 0 || ((uint32_t)size & 0xF)) return SAS_ERR_INVALID_PARAMETER;
    if (loop != 0 && loop != 1) return SAS_ERR_INVALID_LOOP_POS;
    if (!sr_inrange(A2)) return 0;
    if (v->type == VT_ATRAC3) return SAS_ERR_ATRAC3_ALREADY_SET;
    if (size < 0) size = 0;
    const int reset = v->type != VT_VAG || v->vagAddr != A2 || v->vagSize != size || v->loop != (loop != 0);
    v->type = VT_VAG;
    v->vagAddr = A2; v->vagSize = size; v->loop = loop != 0;
    if (v->on) v->playing = 1;
    if (reset) vag_start(&v->vag, A2, (uint32_t)size, loop != 0);
    return 0;
}

static uint32_t h_SetVoicePCM(CpuState *s) {
    /* (core, voice, pcmAddr, size, loopPos) */
    VOICE_OR_ERR(A1);
    const int size = (int)A3, loopPos = (int)T0;
    if (size <= 0 || size > 0x10000) return SAS_ERR_INVALID_PCM_SIZE;
    if (loopPos >= size) return SAS_ERR_INVALID_LOOP_POS;
    if (!sr_inrange(A2)) return 0;
    if (v->type == VT_ATRAC3) return SAS_ERR_ATRAC3_ALREADY_SET;
    v->type = VT_PCM;
    v->pcmAddr = A2; v->pcmSize = size; v->pcmIndex = 0;
    v->pcmLoopPos = loopPos >= 0 ? loopPos : 0;
    v->loop = loopPos >= 0;
    v->playing = 1;
    return 0;
}

static uint32_t h_GetPauseFlag(CpuState *s) {
    (void)s;
    uint32_t m = 0;
    for (int i = 0; i < VOICES_MAX; i++) if (s_v[i].paused) m |= 1u << i;
    return m;
}

static uint32_t h_SetPause(CpuState *s) {
    uint32_t bits = A1;
    for (int i = 0; bits && i < VOICES_MAX; i++, bits >>= 1)
        if (bits & 1) s_v[i].paused = A2 != 0;
    return 0;
}

static uint32_t h_SetVolume(CpuState *s) {
    /* (core, voice, l, r, effectL, effectR) */
    VOICE_OR_ERR(A1);
    const int l = (int)A2, r = (int)A3, el = (int)T0, er = (int)T1;
    if (abs(l) > VOL_MAX || abs(r) > VOL_MAX || abs(el) > VOL_MAX || abs(er) > VOL_MAX) return SAS_ERR_INVALID_VOLUME;
    v->volumeLeft = l; v->volumeRight = r; v->effectLeft = el; v->effectRight = er;
    return 0;
}

static uint32_t h_SetPitch(CpuState *s) {
    VOICE_OR_ERR(A1);
    if ((int)A2 < 0 || (int)A2 > PITCH_MAX) return SAS_ERR_INVALID_PITCH;
    v->pitch = (int)A2;
    return 0;
}

static uint32_t h_SetKeyOn(CpuState *s) {
    VOICE_OR_ERR(A1);
    if (v->paused || v->on) return SAS_ERR_VOICE_PAUSED;
    env_set_state(&v->env, ST_KEYON);
    if (v->type == VT_VAG) {
        if (!sr_inrange(v->vagAddr)) return 0;
        vag_start(&v->vag, v->vagAddr, (uint32_t)v->vagSize, v->loop);
    }
    v->playing = 1; v->on = 1; v->paused = 0; v->sampleFrac = 0;
    return 0;
}

/* KeyOff starts the release phase (a sound can play entirely in release). */
static uint32_t h_SetKeyOff(CpuState *s) {
    VOICE_OR_ERR(A1);
    if (v->paused || !v->on) return SAS_ERR_VOICE_PAUSED;
    v->on = 0;
    env_set_state(&v->env, ST_RELEASE);
    return 0;
}

static uint32_t h_SetNoise(CpuState *s) {
    VOICE_OR_ERR(A1);
    if ((int)A2 < 0 || (int)A2 >= 64) return SAS_ERR_INVALID_NOISE_FREQ;
    v->type = VT_NOISE; v->noiseFreq = (int)A2;
    return 0;
}

static uint32_t h_SetSL(CpuState *s) { VOICE_OR_ERR(A1); v->env.sustainLevel = (int)A2; return 0; }

static uint32_t h_SetADSR(CpuState *s) {
    /* (core, voice, flag, a, d, s, r) */
    VOICE_OR_ERR(A1);
    const int flag = (int)A2, a = (int)A3, d = (int)T0, su = (int)T1, r = (int)T2;
    const int invalid = (a < 0 ? 1 : 0) | (d < 0 ? 2 : 0) | (su < 0 ? 4 : 0) | (r < 0 ? 8 : 0);
    if (invalid & flag) return SAS_ERR_INVALID_ADSR_RATE;
    if (flag & 1) v->env.attackRate = a;
    if (flag & 2) v->env.decayRate = d;
    if (flag & 4) v->env.sustainRate = su;
    if (flag & 8) v->env.releaseRate = r;
    return 0;
}

static uint32_t h_SetADSRmode(CpuState *s) {
    VOICE_OR_ERR(A1);
    const int flag = (int)A2;
    const int a = (int)(A3 & 0x7FFFFFFFu), d = (int)(T0 & 0x7FFFFFFFu), su = (int)(T1 & 0x7FFFFFFFu), r = (int)(T2 & 0x7FFFFFFFu);
    int invalid = 0;
    if (a > 5 || (a & 1) != 0) invalid |= 1;
    if (d > 5 || (d & 1) != 1) invalid |= 2;
    if (su > 5) invalid |= 4;
    if (r > 5 || (r & 1) != 1) invalid |= 8;
    if (invalid & flag) return SAS_ERR_INVALID_ADSR_CURVE_MODE;
    if (flag & 1) v->env.attackType = a;
    if (flag & 2) v->env.decayType = d;
    if (flag & 4) v->env.sustainType = su;
    if (flag & 8) v->env.releaseType = r;
    return 0;
}

static uint32_t h_SetSimpleADSR(CpuState *s) {
    VOICE_OR_ERR(A1);
    if ((A3 >> 13) & 1) return SAS_ERR_INVALID_ADSR_CURVE_MODE;
    env_set_simple(&v->env, A2 & 0xFFFF, A3 & 0xFFFF);
    return 0;
}

static uint32_t h_GetEnvelopeHeight(CpuState *s) { VOICE_OR_ERR(A1); return (uint32_t)env_height(&v->env); }

static uint32_t h_GetAllEnvelopeHeights(CpuState *s) {
    if (!sr_inrange(A1)) return SAS_ERR_INVALID_PARAMETER;
    for (int i = 0; i < VOICES_MAX; i++) MEM_W32(A1 + (uint32_t)i * 4, (uint32_t)env_height(&s_v[i].env));
    return 0;
}

static uint32_t h_RevType(CpuState *s) {
    const int type = (int)A1;
    if (type < EFFECT_OFF || type > EFFECT_MAX) return SAS_ERR_REV_INVALID_TYPE;
    if (type != s_fx.type) { s_fx.type = type; rev_set_preset(type); }
    return 0;
}

static uint32_t h_RevParam(CpuState *s) {
    const int delay = (int)A1, feedback = (int)A2;
    if (delay < 0 || delay >= 128) return SAS_ERR_REV_INVALID_DELAY;
    if (feedback < 0 || feedback >= 128) return SAS_ERR_REV_INVALID_FEEDBACK;
    s_fx.delay = delay; s_fx.feedback = feedback;
    s_revDelay = delay; s_revFeedback = feedback;
    rev_apply_params();
    return 0;
}

static uint32_t h_RevEVOL(CpuState *s) {
    if (A1 > 0x1000 || A2 > 0x1000) return SAS_ERR_REV_INVALID_VOLUME;
    s_fx.leftVol = (int)A1; s_fx.rightVol = (int)A2;
    return 0;
}

static uint32_t h_RevVON(CpuState *s) { s_fx.isDryOn = A1 != 0; s_fx.isWetOn = A2 != 0; return 0; }

static uint32_t h_GetGrain(CpuState *s) { (void)s; return (uint32_t)s_grain; }
static uint32_t h_SetGrain(CpuState *s) {
    if (A1 < 0x40 || A1 > 0x800 || (A1 & 0x1F)) return SAS_ERR_INVALID_GRAIN;
    s_grain = (int)A1;
    return 0;
}
static uint32_t h_GetOutputmode(CpuState *s) { (void)s; return (uint32_t)s_outputMode; }
static uint32_t h_SetOutputmode(CpuState *s) {
    if (A1 > 1) return SAS_ERR_INVALID_OUTPUT_MODE;
    s_outputMode = (int)A1;
    return 0;
}

void sr_hle_init_sas(void) {
    sr_hle_register(0x42778a9f, "__sceSasInit", h_Init);
    sr_hle_register(0xa3589d81, "__sceSasCore", h_Core);
    sr_hle_register(0x50a14dfc, "__sceSasCoreWithMix", h_CoreWithMix);
    sr_hle_register(0x68a46b95, "__sceSasGetEndFlag", h_GetEndFlag);
    sr_hle_register(0x440ca7d8, "__sceSasSetVolume", h_SetVolume);
    sr_hle_register(0xad84d37f, "__sceSasSetPitch", h_SetPitch);
    sr_hle_register(0x99944089, "__sceSasSetVoice", h_SetVoice);
    sr_hle_register(0xb7660a23, "__sceSasSetNoise", h_SetNoise);
    sr_hle_register(0x019b25eb, "__sceSasSetADSR", h_SetADSR);
    sr_hle_register(0x9ec3676a, "__sceSasSetADSRmode", h_SetADSRmode);
    sr_hle_register(0x5f9529f6, "__sceSasSetSL", h_SetSL);
    sr_hle_register(0x74ae582a, "__sceSasGetEnvelopeHeight", h_GetEnvelopeHeight);
    sr_hle_register(0xcbcd4f79, "__sceSasSetSimpleADSR", h_SetSimpleADSR);
    sr_hle_register(0xa0cf2fa4, "__sceSasSetKeyOff", h_SetKeyOff);
    sr_hle_register(0x76f01aca, "__sceSasSetKeyOn", h_SetKeyOn);
    sr_hle_register(0xf983b186, "__sceSasRevVON", h_RevVON);
    sr_hle_register(0xd5a229c9, "__sceSasRevEVOL", h_RevEVOL);
    sr_hle_register(0x33d4ab37, "__sceSasRevType", h_RevType);
    sr_hle_register(0x267a6dd2, "__sceSasRevParam", h_RevParam);
    sr_hle_register(0x2c8e6ab3, "__sceSasGetPauseFlag", h_GetPauseFlag);
    sr_hle_register(0x787d04d5, "__sceSasSetPause", h_SetPause);
    sr_hle_register(0xbd11b7c2, "__sceSasGetGrain", h_GetGrain);
    sr_hle_register(0xd1e0a01e, "__sceSasSetGrain", h_SetGrain);
    sr_hle_register(0xe175ef66, "__sceSasGetOutputmode", h_GetOutputmode);
    sr_hle_register(0xe855bf76, "__sceSasSetOutputmode", h_SetOutputmode);
    sr_hle_register(0x07f58c24, "__sceSasGetAllEnvelopeHeights", h_GetAllEnvelopeHeights);
    sr_hle_register(0xe1cd9561, "__sceSasSetVoicePCM", h_SetVoicePCM);
}
