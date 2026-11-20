#include "fx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "params.h"

#define TWO_PI_F 6.28318530717959f
#define PI_F     3.14159265358979f

static int line_init(Line *l, int size)
{
    l->buf  = calloc((size_t)size, sizeof(float));
    l->size = size;
    l->pos  = 0;
    return l->buf ? 0 : -1;
}

static void line_free(Line *l) { free(l->buf); l->buf = NULL; }
static void line_clear(Line *l) { memset(l->buf, 0, sizeof(float) * l->size); l->pos = 0; }

static inline void line_write(Line *l, float x)
{
    l->buf[l->pos] = x;
    if (++l->pos >= l->size) l->pos = 0;
}

/* Lê `delay` amostras atrás (fracionário, interpolação linear). */
static inline float line_read(const Line *l, float delay)
{
    float rp = (float)l->pos - delay;
    while (rp < 0) rp += (float)l->size;
    int   i0 = (int)rp;
    float f  = rp - (float)i0;
    int   i1 = i0 + 1;
    if (i0 >= l->size) i0 -= l->size;
    if (i1 >= l->size) i1 -= l->size;
    return l->buf[i0] + f * (l->buf[i1] - l->buf[i0]);
}

const char *fx_name(int t)
{
    static const char *n[FX_COUNT] = {"Distorção", "Chorus", "Phaser", "Delay", "Reverb"};
    return (t >= 0 && t < FX_COUNT) ? n[t] : "?";
}

int fx_param_on(int t)
{
    static const int ids[FX_COUNT] = {P_DIST_ON, P_CHO_ON, P_PHA_ON, P_DLY_ON, P_REV_ON};
    return ids[t];
}

/* Afinações do Freeverb (em amostras a 44,1 kHz) */
static const int COMB_TUNING[FV_COMBS] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
static const int AP_TUNING[FV_ALLPASS] = {556, 441, 341, 225};
#define FV_SPREAD 23

int fx_init(Fx *fx, float sr)
{
    memset(fx, 0, sizeof *fx);
    fx->sr = sr;
    float k = sr / 44100.0f;
    int err = 0;
    for (int c = 0; c < 2; c++) {
        err |= line_init(&fx->cho[c], (int)(0.05f * sr));
        err |= line_init(&fx->dly[c], (int)(2.1f * sr));
        for (int i = 0; i < FV_COMBS; i++) err |= line_init(&fx->comb[c][i], (int)((COMB_TUNING[i] + c * FV_SPREAD) * k));
        for (int i = 0; i < FV_ALLPASS; i++) err |= line_init(&fx->ap[c][i], (int)((AP_TUNING[i] + c * FV_SPREAD) * k));
    }
    fx->dly_time_smooth = 0.35f * sr;
    return err ? -1 : 0;
}

void fx_free(Fx *fx)
{
    for (int c = 0; c < 2; c++) {
        line_free(&fx->cho[c]);
        line_free(&fx->dly[c]);
        for (int i = 0; i < FV_COMBS; i++) line_free(&fx->comb[c][i]);
        for (int i = 0; i < FV_ALLPASS; i++) line_free(&fx->ap[c][i]);
    }
}

void fx_reset(Fx *fx)
{
    for (int c = 0; c < 2; c++) {
        line_clear(&fx->cho[c]);
        line_clear(&fx->dly[c]);
        for (int i = 0; i < FV_COMBS; i++) { line_clear(&fx->comb[c][i]); fx->comb_lp[c][i] = 0; }
        for (int i = 0; i < FV_ALLPASS; i++) line_clear(&fx->ap[c][i]);
        fx->dist_lp[c] = fx->dly_lp[c] = fx->pha_fb[c] = 0;
        memset(fx->pha_x1[c], 0, sizeof fx->pha_x1[c]);
        memset(fx->pha_y1[c], 0, sizeof fx->pha_y1[c]);
    }
}

/* ---------- Distorção: ganho -> saturação assimétrica -> filtro de tom ---------- */
static void distortion(Fx *fx, float *l, float *r, int n)
{
    float gain = powf(10.0f, param_s(P_DIST_DRIVE) / 20.0f);
    float mix  = param_s(P_DIST_MIX);
    float a    = 1.0f - expf(-TWO_PI_F * param_s(P_DIST_TONE) / fx->sr);
    float makeup = 0.7f / (1.0f + 0.02f * param_s(P_DIST_DRIVE));
    float *ch[2] = {l, r};
    for (int c = 0; c < 2; c++) {
        float lp = fx->dist_lp[c];
        for (int i = 0; i < n; i++) {
            float x = ch[c][i];
            float y = tanhf(gain * x + 0.15f) - 0.1489f; /* +0.15: harmônicos pares (mais "valvulado") */
            lp += a * (y - lp);
            ch[c][i] = x * (1.0f - mix) + lp * mix * makeup * 1.4f;
        }
        fx->dist_lp[c] = lp;
    }
}

/* ---------- Chorus: duas linhas de atraso moduladas, L e R em oposição ---------- */
static void chorus(Fx *fx, float *l, float *r, int n)
{
    float rate = param_s(P_CHO_RATE), depth = param_s(P_CHO_DEPTH), mix = param_s(P_CHO_MIX);
    float base = 0.012f * fx->sr, swing = 0.008f * fx->sr * depth;
    for (int i = 0; i < n; i++) {
        float mod = sinf(TWO_PI_F * fx->cho_phase);
        fx->cho_phase += rate / fx->sr;
        if (fx->cho_phase >= 1) fx->cho_phase -= 1;
        float in_l = l[i], in_r = r[i];
        line_write(&fx->cho[0], in_l);
        line_write(&fx->cho[1], in_r);
        float wl = line_read(&fx->cho[0], base + swing * (1 + mod) * 0.5f);
        float wr = line_read(&fx->cho[1], base + swing * (1 - mod) * 0.5f);
        l[i] = in_l * (1 - 0.5f * mix) + wl * mix * 0.7f;
        r[i] = in_r * (1 - 0.5f * mix) + wr * mix * 0.7f;
    }
}

/* ---------- Phaser: 6 filtros passa-tudo varridos por um LFO ---------- */
static void phaser(Fx *fx, float *l, float *r, int n)
{
    float rate = param_s(P_PHA_RATE), depth = param_s(P_PHA_DEPTH);
    float fb = param_s(P_PHA_FB), mix = param_s(P_PHA_MIX);
    float *ch[2] = {l, r};
    for (int i = 0; i < n; i++) {
        float ph = fx->pha_phase;
        fx->pha_phase += rate / fx->sr;
        if (fx->pha_phase >= 1) fx->pha_phase -= 1;
        for (int c = 0; c < 2; c++) {
            float lfo = 0.5f + 0.5f * sinf(TWO_PI_F * (ph + c * 0.25f));
            float f   = 200.0f * powf(2.0f, lfo * depth * 4.5f); /* 200 Hz .. ~4,5 kHz */
            float t   = tanf(PI_F * f / fx->sr);
            float a   = (t - 1.0f) / (t + 1.0f);
            float x   = ch[c][i];
            float y   = x + fx->pha_fb[c] * fb;
            for (int s = 0; s < PHASER_STAGES; s++) {
                float out = a * y + fx->pha_x1[c][s] - a * fx->pha_y1[c][s];
                fx->pha_x1[c][s] = y;
                fx->pha_y1[c][s] = out;
                y = out;
            }
            fx->pha_fb[c] = y;
            ch[c][i] = x * (1 - 0.5f * mix) + y * 0.5f * mix;
        }
    }
}

/* ---------- Delay com tempo livre ou sincronizado ao BPM, ping-pong opcional ---------- */
static float delay_seconds(float bpm)
{
    float beat = 60.0f / bpm;
    switch (param_geti(P_DLY_SYNC)) {
    case 1: return beat;               /* 1/4 */
    case 2: return beat * 0.5f;        /* 1/8 */
    case 3: return beat * 0.75f;       /* 1/8 pontuada */
    case 4: return beat * 0.25f;       /* 1/16 */
    case 5: return beat * 2.0f / 3.0f; /* 1/4 tercina */
    default: return param_s(P_DLY_TIME) / 1000.0f;
    }
}

static void delay(Fx *fx, float *l, float *r, int n, float bpm)
{
    float target = fminf(delay_seconds(bpm), 2.0f) * fx->sr;
    float fb = param_s(P_DLY_FB), mix = param_s(P_DLY_MIX);
    int   ping = param_geti(P_DLY_PING);
    float damp = 1.0f - expf(-TWO_PI_F * 5000.0f / fx->sr); /* repetições ficam mais escuras */
    for (int i = 0; i < n; i++) {
        fx->dly_time_smooth += (target - fx->dly_time_smooth) * 0.0005f; /* sem "zíper" ao mudar o tempo */
        float wl = line_read(&fx->dly[0], fx->dly_time_smooth);
        float wr = line_read(&fx->dly[1], fx->dly_time_smooth);
        fx->dly_lp[0] += damp * (wl - fx->dly_lp[0]);
        fx->dly_lp[1] += damp * (wr - fx->dly_lp[1]);
        if (ping) {
            line_write(&fx->dly[0], 0.5f * (l[i] + r[i]) + fx->dly_lp[1] * fb);
            line_write(&fx->dly[1], fx->dly_lp[0] * fb);
        } else {
            line_write(&fx->dly[0], l[i] + fx->dly_lp[0] * fb);
            line_write(&fx->dly[1], r[i] + fx->dly_lp[1] * fb);
        }
        l[i] += wl * mix;
        r[i] += wr * mix;
    }
}

/* ---------- Reverb: Freeverb (8 combs em paralelo + 4 passa-tudo em série) ---------- */
static void reverb(Fx *fx, float *l, float *r, int n)
{
    float room = 0.7f + 0.28f * param_s(P_REV_SIZE);
    float damp = 0.4f * param_s(P_REV_DAMP);
    float mix  = param_s(P_REV_MIX);
    float *ch[2] = {l, r};
    for (int i = 0; i < n; i++) {
        float input = (l[i] + r[i]) * 0.015f;
        for (int c = 0; c < 2; c++) {
            float out = 0;
            for (int k = 0; k < FV_COMBS; k++) {
                Line *cl = &fx->comb[c][k];
                float y = cl->buf[cl->pos];
                fx->comb_lp[c][k] = y * (1 - damp) + fx->comb_lp[c][k] * damp;
                line_write(cl, input + fx->comb_lp[c][k] * room);
                out += y;
            }
            for (int k = 0; k < FV_ALLPASS; k++) {
                Line *al = &fx->ap[c][k];
                float b = al->buf[al->pos];
                line_write(al, out + b * 0.5f);
                out = b - out;
            }
            ch[c][i] = ch[c][i] * (1 - 0.5f * mix) + out * mix;
        }
    }
}

void fx_process(Fx *fx, float *l, float *r, int frames, float bpm)
{
    int used[FX_COUNT] = {0};
    for (int slot = 0; slot < FX_COUNT; slot++) {
        int t = param_geti(P_FX_ORDER0 + slot);
        if (t < 0 || t >= FX_COUNT || used[t]) continue; /* ordem inválida: ignora repetidos */
        used[t] = 1;
        if (!param_geti(fx_param_on(t))) continue;
        switch (t) {
        case FX_DIST:   distortion(fx, l, r, frames); break;
        case FX_CHORUS: chorus(fx, l, r, frames); break;
        case FX_PHASER: phaser(fx, l, r, frames); break;
        case FX_DELAY:  delay(fx, l, r, frames, bpm); break;
        case FX_REVERB: reverb(fx, l, r, frames); break;
        }
    }
}
