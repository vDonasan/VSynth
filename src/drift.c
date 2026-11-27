#include "drift.h"

#include <stdlib.h>
#include <string.h>

int drift_init(Drift *d, int channels, int target_frames)
{
    memset(d, 0, sizeof *d);
    if (channels < 1) channels = 1;
    if (channels > DRIFT_MAX_CH) channels = DRIFT_MAX_CH;
    if (target_frames < 32) target_frames = 32;
    int cap = 4096;
    while (cap < target_frames * 8) cap *= 2;
    d->buf = calloc((size_t)cap * channels, sizeof(float));
    if (!d->buf) return -1;
    d->ch       = channels;
    d->cap      = cap;
    d->target   = target_frames;
    d->ratio    = 1.0;
    d->fill_avg = target_frames;
    atomic_init(&d->wpos, 0);
    atomic_init(&d->rpos, 0);
    return 0;
}

void drift_free(Drift *d)
{
    free(d->buf);
    d->buf = NULL;
}

int drift_fill(const Drift *d)
{
    unsigned w = atomic_load_explicit((atomic_uint *)&d->wpos, memory_order_acquire);
    unsigned r = atomic_load_explicit((atomic_uint *)&d->rpos, memory_order_acquire);
    return (int)(w - r);
}

void drift_push(Drift *d, const float *in, int frames)
{
    unsigned w = atomic_load_explicit(&d->wpos, memory_order_relaxed);
    unsigned r = atomic_load_explicit(&d->rpos, memory_order_acquire);
    /* guarda 4 quadros de folga: a saída ainda lê o quadro anterior a rpos */
    int space = d->cap - 4 - (int)(w - r);
    if (frames > space) {
        d->overruns++;
        frames = space > 0 ? space : 0;
    }
    for (int i = 0; i < frames; i++) {
        float *dst = d->buf + (size_t)((w + (unsigned)i) & (unsigned)(d->cap - 1)) * d->ch;
        memcpy(dst, in + (size_t)i * d->ch, sizeof(float) * d->ch);
    }
    atomic_store_explicit(&d->wpos, w + (unsigned)frames, memory_order_release);
}

static inline const float *frame_at(const Drift *d, unsigned pos)
{
    return d->buf + (size_t)(pos & (unsigned)(d->cap - 1)) * d->ch;
}

void drift_pull(Drift *d, float *out, int frames)
{
    unsigned w = atomic_load_explicit(&d->wpos, memory_order_acquire);
    unsigned r = atomic_load_explicit(&d->rpos, memory_order_relaxed);
    int fill = (int)(w - r);

    if (!d->primed) {
        if (fill < d->target) {
            memset(out, 0, sizeof(float) * (size_t)frames * d->ch);
            return;
        }
        /* começa no alvo exato: o excesso acumulado na espera é descartado */
        r = w - (unsigned)d->target;
        d->frac = 0.0;
        d->primed = 1;
        d->fill_avg = d->target;
        d->integ = 0.0;
        fill = d->target;
    }

    /* Controle do passo: nível médio da fila (~0,5 s) contra o alvo.
       Proporcional + integral; a integral aprende a diferença de relógio,
       e o passo nunca muda mais que 0,2% (inaudível). */
    double fill_now = (double)fill - d->frac;
    double a = (double)frames / 24000.0;
    if (a > 0.2) a = 0.2;
    d->fill_avg += (fill_now - d->fill_avg) * a;
    /* Fila sobe/desce (ratio_ideal - ratio) * 48000 quadros por segundo.
       Proporcional 4e-6/quadro: corrige um erro em ~5 s. Integral bem mais
       lenta (~20 s) que aprende a diferença de relógio sem oscilar. */
    double err = d->fill_avg - d->target;
    d->integ += 2e-7 * err * frames / 48000.0;
    if (d->integ > 0.002) d->integ = 0.002;
    if (d->integ < -0.002) d->integ = -0.002;
    double ratio = 1.0 + 4e-6 * err + d->integ;
    if (ratio > 1.002) ratio = 1.002;
    if (ratio < 0.998) ratio = 0.998;
    d->ratio = ratio;

    double frac = d->frac;
    int i = 0;
    for (; i < frames; i++) {
        /* interpolação cúbica (Hermite) entre r e r+1: precisa de r-1..r+2 */
        if ((int)(w - r) < 3) break;
        const float *xm = frame_at(d, r - 1), *x0 = frame_at(d, r), *x1 = frame_at(d, r + 1), *x2 = frame_at(d, r + 2);
        float t = (float)frac;
        for (int c = 0; c < d->ch; c++) {
            float c1 = 0.5f * (x1[c] - xm[c]);
            float c2 = xm[c] - 2.5f * x0[c] + 2.0f * x1[c] - 0.5f * x2[c];
            float c3 = 0.5f * (x2[c] - xm[c]) + 1.5f * (x0[c] - x1[c]);
            out[(size_t)i * d->ch + c] = ((c3 * t + c2) * t + c1) * t + x0[c];
        }
        frac += ratio;
        while (frac >= 1.0) { frac -= 1.0; r++; }
    }
    if (i < frames) {
        /* faltou entrada (a entrada parou ou atrasou demais): silêncio e
           junta o alvo de novo antes de voltar */
        memset(out + (size_t)i * d->ch, 0, sizeof(float) * (size_t)(frames - i) * d->ch);
        d->underruns++;
        d->primed = 0;
    }
    d->frac = frac;
    atomic_store_explicit(&d->rpos, r, memory_order_release);
}
