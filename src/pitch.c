#include "pitch.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

float hz_to_midi(float hz) { return 69.0f + 12.0f * log2f(hz / 440.0f); }

const char *note_name(int midi, char *buf, int buflen)
{
    static const char *names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (midi < 0) snprintf(buf, buflen, "--");
    else snprintf(buf, buflen, "%s%d", names[midi % 12], midi / 12 - 1);
    return buf;
}

/* ------------------------------------------------------------------ */
/* YIN                                                                 */
/* ------------------------------------------------------------------ */
int yin_init(Yin *y, float sr, int size, float fmin, float fmax, float threshold)
{
    y->sr        = sr;
    y->size      = size;
    y->tau_min   = (int)(sr / fmax);
    y->tau_max   = (int)(sr / fmin);
    y->threshold = threshold;
    if (y->tau_max >= size / 2 + size / 4) return -1; /* janela curta demais para fmin */
    y->d   = calloc((size_t)y->tau_max + 2, sizeof(float));
    y->raw = calloc((size_t)y->tau_max + 2, sizeof(float));
    y->acc = calloc((size_t)y->tau_max + 2, sizeof(double));
    return (y->d && y->raw && y->acc) ? 0 : -1;
}

void yin_free(Yin *y)
{
    free(y->d);
    free(y->raw);
    free(y->acc);
    y->d = y->raw = NULL;
    y->acc = NULL;
}

float yin_detect(Yin *y, const float *x, float *confidence)
{
    const int W = y->size - y->tau_max; /* trecho comparado */
    /* Passo 2: função diferença */
    for (int tau = 1; tau <= y->tau_max; tau++) {
        float sum = 0.0f;
        for (int j = 0; j < W; j++) {
            float diff = x[j] - x[j + tau];
            sum += diff * diff;
        }
        y->acc[tau] = sum;
    }
    return yin_eval(y, confidence);
}

void yin_slide(Yin *y, const float *x, int hop)
{
    /* Cada termo (x[j] - x[j+tau])² que entra na janela sai dela, mais
       tarde, calculado exatamente igual (mesmas amostras, mesma conta em
       float). Somando direto no acumulador em double, entrada e saída se
       cancelam e o erro não cresce. O laço interno corre em tau, que o
       compilador vetoriza. */
    const int W = y->size - y->tau_max, T = y->tau_max;
    double *acc = y->acc + 1;
    for (int j = 0; j < hop; j++) {         /* termos que saem da janela */
        const float a = x[j], *b = x + j + 1;
        for (int t = 0; t < T; t++) {
            float d = a - b[t];
            acc[t] -= (double)(d * d);
        }
    }
    for (int j = W; j < W + hop; j++) {     /* termos que entram */
        const float a = x[j], *b = x + j + 1;
        for (int t = 0; t < T; t++) {
            float d = a - b[t];
            acc[t] += (double)(d * d);
        }
    }
}

float yin_eval(Yin *y, float *confidence)
{
    float *d = y->d;
    float running = 0.0f;
    /* Passo 3: normalização cumulativa */
    d[0] = 1.0f;
    for (int tau = 1; tau <= y->tau_max; tau++) {
        float sum = y->acc[tau] > 0 ? (float)y->acc[tau] : 0.0f;
        running += sum;
        y->raw[tau] = sum;
        d[tau] = running > 0.0f ? sum * (float)tau / running : 1.0f;
    }

    /* Passo 4: primeiro mínimo abaixo do limiar */
    int tau = -1;
    for (int t = y->tau_min; t <= y->tau_max; t++) {
        if (d[t] < y->threshold) {
            while (t + 1 <= y->tau_max && d[t + 1] < d[t]) t++;
            tau = t;
            break;
        }
    }
    if (tau < 0) {
        if (confidence) *confidence = 0.0f;
        return -1.0f;
    }

    /* Passo 5: interpolação parabólica para precisão abaixo de 1 amostra,
       feita na diferença bruta, como no artigo original do YIN. */
    const float *r = y->raw;
    if (tau > 1 && tau < y->tau_max) { /* mínimo local da bruta perto dali */
        if (r[tau - 1] < r[tau] && r[tau - 1] <= r[tau + 1]) tau--;
        else if (r[tau + 1] < r[tau]) tau++;
    }
    float better = (float)tau;
    if (tau > 1 && tau < y->tau_max) {
        float s0 = r[tau - 1], s1 = r[tau], s2 = r[tau + 1];
        float den = s0 - 2.0f * s1 + s2;
        if (fabsf(den) > 1e-9f) better = tau + 0.5f * (s0 - s2) / den;
    }
    if (confidence) *confidence = 1.0f - d[tau];
    if (better < 1.0f) better = 1.0f;
    return y->sr / better;
}

float yin_cmnd_at(const Yin *y, float tau)
{
    int t = (int)lroundf(tau);
    if (t < 1 || t > y->tau_max) return 2.0f;
    float v = y->d[t]; /* pega o menor entre os vizinhos: tau é fracionário */
    if (t > 1 && y->d[t - 1] < v) v = y->d[t - 1];
    if (t < y->tau_max && y->d[t + 1] < v) v = y->d[t + 1];
    return v;
}

/* ------------------------------------------------------------------ */
/* Seguidor de notas                                                   */
/* ------------------------------------------------------------------ */
void tracker_init(NoteTracker *t, float on_db, float hysteresis_db)
{
    t->on_db         = on_db;
    t->off_db        = on_db - hysteresis_db;
    t->stable_needed = 2;
    t->current       = -1;
    t->candidate     = -1;
    t->cand_count    = 0;
    t->last_db       = -120.0f;
    t->attack_pending = 0;
    t->since_on       = 1000;
    t->refractory     = 0;
    t->pend_hops      = 0;
    t->settle         = 0;
    t->last_off       = -1;
    t->since_off      = 1000;
    t->rering         = 0;
    t->hop_scale      = 1;
    t->hist_pos       = 0;
    for (int i = 0; i < 8; i++) t->db_hist[i] = -120.0f;
}

void tracker_set_hop(NoteTracker *t, int hop)
{
    int s = 128 / hop;
    t->hop_scale = s < 1 ? 1 : (s > 7 ? 7 : s);
}

void tracker_set_refractory(NoteTracker *t, float ms, float sr, int hop)
{
    t->refractory = (int)(ms / 1000.0f * sr / hop + 0.5f);
    t->rering     = (int)(40.0f / 1000.0f * sr / hop + 0.5f); /* 40 ms */
}

void tracker_set_settle(NoteTracker *t, int hops) { t->settle = hops; }

static Event mk(EventType type, int note, int vel, float value)
{
    Event e = {(uint8_t)type, SRC_AUDIO, (uint8_t)note, (uint8_t)vel, value};
    return e;
}

static int db_to_velocity(const NoteTracker *t, float db)
{
    int v = (int)(40.0f + (db - t->on_db) * 3.0f);
    return v < 1 ? 1 : (v > 127 ? 127 : v);
}

int tracker_process(NoteTracker *t, float hz, float db, Event *out, int max)
{
    int n = 0;
    /* volume de 128 amostras atrás: com passos menores, as medições se
       sobrepõem e o salto da palhetada se espalha por mais de uma */
    float prev_db = t->db_hist[(t->hist_pos - t->hop_scale + 8) & 7];
    t->db_hist[t->hist_pos] = db;
    t->hist_pos = (t->hist_pos + 1) & 7;
    t->last_db = db;
    if (max < 2) return 0;
    if (t->since_on < 1000) t->since_on++;
    if (t->since_off < 1000) t->since_off++;

    /* Silêncio: solta a nota (histerese evita liga-desliga no decaimento) */
    if (t->current >= 0 && db < t->off_db) {
        out[n++] = mk(EV_NOTE_OFF, t->current, 0, 0.0f);
        t->last_off       = t->current;
        t->since_off      = 0;
        t->current        = -1;
        t->cand_count     = 0;
        t->attack_pending = 0;
        return n;
    }

    /* Salto de volume com nota soando = nova palhetada. A janela de análise
       ainda contém a nota anterior, então espera a nova altura se firmar. */
    if (t->current >= 0 && db - prev_db > 9.0f && t->since_on > t->refractory) {
        t->attack_pending = 1;
        t->pend_hops      = 0;
        t->candidate      = -1;
        t->cand_count     = 0;
        return n;
    }

    if (t->attack_pending) t->pend_hops++;

    int loud_enough = db > t->on_db || (t->current >= 0 && db > t->off_db);
    if (hz <= 0.0f || !loud_enough) return n; /* ruído da palhetada: mantém o estado */

    float midi = hz_to_midi(hz);
    int   note = (int)lroundf(midi);
    if (note < 0 || note > 127) return n;

    if (note == t->current && !t->attack_pending) {
        out[n++] = mk(EV_PITCH_TRACK, note, 0, midi); /* bends e vibrato */
        t->cand_count = 0;
        return n;
    }

    /* Ruído de abafar a corda ainda carrega a afinação: não é nota nova */
    if (t->current < 0 && note == t->last_off && t->since_off <= t->rering) return n;

    /* Mudança sem palhetada (legato) perto do ruído: no fim de uma nota o
       chiado gera leituras aleatórias. Só aceita com sinal bem acima do
       gate e leitura confiável; senão mantém a nota até ela terminar. */
    if (t->current >= 0 && !t->attack_pending && note != t->current &&
        (db < t->on_db + 6.0f || (t->conf > 0 && t->conf < 0.88f))) {
        t->cand_count = 0;
        return n;
    }

    /* Logo após a palhetada a janela ainda "ouve" a nota anterior */
    if (t->attack_pending && note == t->current && t->pend_hops <= t->settle) return n;

    /* Nota nova (ou palhetada nova): só aceita depois de confirmar a
       altura em janelas seguidas, para evitar notas fantasmas. */
    if (note == t->candidate) t->cand_count++;
    else { t->candidate = note; t->cand_count = 1; }

    /* Sem palhetada, um "salto" para um harmônico da nota atual (oitava,
       12ª, 2 oitavas...) quase sempre é o detector se confundindo com um
       harmônico forte, não o músico. Exige confirmação bem mais longa. */
    int needed = t->stable_needed;
    if (t->fast_conf > 0 && t->conf >= t->fast_conf) needed = 1; /* leitura muito limpa: aceita já */
    if (t->current >= 0 && !t->attack_pending) {
        int jump = note - t->current;
        if (jump == 12 || jump == 19 || jump == 24 || jump == 28 || jump == 31 || jump == -12 || jump == -19 || jump == -24)
            needed = t->stable_needed * 8 * t->hop_scale;
    }

    if (t->cand_count >= needed) {
        /* Com palhetada: solta a anterior e ataca de novo.
           Sem palhetada (hammer-on, slide): o sintetizador faz legato. */
        if (t->current >= 0 && t->attack_pending) out[n++] = mk(EV_NOTE_OFF, t->current, 0, 0.0f);
        out[n++] = mk(EV_NOTE_ON, note, db_to_velocity(t, db), midi);
        t->since_on       = 0;
        t->current        = note;
        t->cand_count     = 0;
        t->attack_pending = 0;
    }
    return n;
}
