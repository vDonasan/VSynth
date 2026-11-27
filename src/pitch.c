#include "pitch.h"
#include "config.h"

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
    t->picked_on      = 0;
    t->fix_hops       = 60;
    t->refractory     = 0;
    t->pend_hops      = 0;
    t->settle         = 0;
    t->last_off       = -1;
    t->since_off      = 1000;
    t->rering         = 0;
    t->hop_scale      = 1;
    t->since_onset    = 1000;
    t->external_pick  = 0;
    t->pick           = 0;
    t->legato_needed  = 4;
    t->attack_age     = 100000;
    t->start_window   = 45;
    t->tail_miss      = 0;
    t->tail_hops      = 15;
    t->hist_pos       = 0;
    for (int i = 0; i < 8; i++) t->db_hist[i] = -120.0f;
}

void tracker_set_hop(NoteTracker *t, int hop)
{
    int s = 128 / hop;
    t->hop_scale = s < 1 ? 1 : (s > 7 ? 7 : s);
    /* 20 ms de leituras iguais para trocar de nota sem palhetada (48 kHz) */
    t->fix_hops      = (int)(80.0f / 1000.0f * 48000.0f / hop + 0.5f);
    t->legato_needed = (int)(PITCH_LEGATO_MS / 1000.0f * 48000.0f / hop + 0.5f);
    if (t->legato_needed < 2) t->legato_needed = 2;
    t->start_window = (int)(PITCH_START_MS / 1000.0f * 48000.0f / hop + 0.5f);
    t->tail_hops    = (int)(PITCH_TAIL_MS / 1000.0f * 48000.0f / hop + 0.5f);
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
    if (max < 3) return 0;
    if (t->since_on < 1000) t->since_on++;
    if (t->since_off < 1000) t->since_off++;
    if (t->since_onset < 1000) t->since_onset++;

    /* Ataque: palhetada (detector de agudos) ou volume saindo do silêncio.
       Só um ataque pode começar uma nota: zumbido e ruído constantes, mesmo
       acima do gate, não têm ataque e não viram nota. */
    if ((t->external_pick && t->pick) || (db > t->on_db && prev_db <= t->on_db)) t->attack_age = 0;
    else if (t->attack_age < 100000) t->attack_age++;

    /* Abaixo do gate a nota NÃO termina na hora: enquanto a altura lida
       continuar batendo com ela, segue soando (até PITCH_TAIL_DB mais
       baixo). Termina quando o som some, quando a leitura deixa de bater
       por PITCH_TAIL_MS, ou bem abaixo do gate. Assim um gate alto não
       corta a nota cedo. Abaixo do gate nada troca de nota. */
    if (t->current >= 0 && db < t->off_db) {
        int same = hz > 0.0f && fabsf(hz_to_midi(hz) - (float)t->current) < 0.7f && db > t->off_db - PITCH_TAIL_DB;
        if (same) t->tail_miss = 0;
        else t->tail_miss++;
        if (same || (t->tail_miss < t->tail_hops && db > t->off_db - PITCH_TAIL_DB)) {
            t->cand_count = 0;
            return n;
        }
    }
    if (t->current >= 0 && db >= t->off_db) t->tail_miss = 0;

    /* Silêncio: solta a nota */
    if (t->current >= 0 && db < t->off_db) {
        t->tail_miss = 0;
        out[n++] = mk(EV_NOTE_OFF, t->current, 0, 0.0f);
        t->last_off       = t->current;
        t->since_off      = 0;
        t->current        = -1;
        t->cand_count     = 0;
        t->attack_pending = 0;
        return n;
    }

    /* Palhetada a partir do silêncio: avisa já, antes de saber a nota
       (o motor pode começar o som abafado enquanto a altura se confirma) */
    if (t->current < 0 && db > t->on_db && prev_db <= t->on_db && t->since_onset > t->refractory) {
        out[n++] = mk(EV_ONSET, 0, db_to_velocity(t, db), 0.0f);
        t->since_onset = 0;
    }

    /* Palhetada com nota soando: vem do detector de agudos (guitarra) ou,
       sem ele, de um salto de volume. A janela de análise ainda contém a
       nota anterior, então espera a nova altura se firmar. */
    int picked = t->external_pick ? t->pick : db - prev_db > 9.0f;
    if (t->current >= 0 && picked && t->since_on > t->refractory && t->since_onset > t->refractory) {
        out[n++] = mk(EV_ONSET, 0, db_to_velocity(t, db), 0.0f);
        t->since_onset    = 0;
        t->attack_pending = 1;
        t->pend_hops      = 0;
        t->candidate      = -1;
        t->cand_count     = 0;
        return n;
    }

    if (t->attack_pending) t->pend_hops++;

    int loud_enough = db > t->on_db || (t->current >= 0 && db > t->off_db);
    /* sem altura definida (ruído da palhetada, acorde): mantém o estado e
       zera a confirmação, que só vale para leituras seguidas */
    if (hz <= 0.0f || !loud_enough) { t->cand_count = 0; t->candidate = -1; return n; }

    float midi = hz_to_midi(hz);
    int   note = (int)lroundf(midi);
    if (note < 0 || note > 127) return n;
    /* Margem: a corda começa um pouco aguda e vai assentando, e a afinação
       nunca é perfeita. Uma leitura a menos de 0,7 semitom da nota que está
       soando (ou da última, numa palhetada nova) conta como ela. */
    /* Começo de nota palhetada (~80 ms): a corda às vezes chega na nota
       vindo de meio tom abaixo/acima (slide, palhetada forte). Uma mudança
       de 1 semitom aí é a MESMA nota sendo corrigida: troca assim que a
       leitura fica mais perto da vizinha, e o synth desliza sem atacar de
       novo, em vez de soar uma nota errada por 30-50 ms. */
    int fixing = t->current >= 0 && !t->attack_pending && t->picked_on && t->since_on <= t->fix_hops;
    if (t->current >= 0 && fabsf(midi - t->current) < (fixing ? 0.5f : 0.7f)) note = t->current;
    int fix = fixing && abs(note - t->current) == 1;

    if (note == t->current && !t->attack_pending) {
        out[n++] = mk(EV_PITCH_TRACK, note, 0, midi); /* bends e vibrato */
        t->cand_count = 0;
        return n;
    }

    /* Ruído de abafar a corda ainda carrega a afinação: não é nota nova */
    if (t->current < 0 && note == t->last_off && t->since_off <= t->rering) return n;
    /* sem ataque recente, nada começa (zumbido, ruído, corda vibrando sozinha) */
    if (t->current < 0 && t->attack_age > t->start_window) { t->cand_count = 0; return n; }

    /* Mudança sem palhetada (legato) perto do ruído: no fim de uma nota o
       chiado gera leituras aleatórias. Só aceita com sinal bem acima do
       gate e leitura confiável; senão mantém a nota até ela terminar. */
    if (t->current >= 0 && !t->attack_pending && note != t->current && !fix &&
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
    if (t->current >= 0 && !t->attack_pending) {
        /* Troca sem palhetada (hammer-on, slide... ou um harmônico, outra
           corda vibrando, uma nota do acorde): só com leituras seguidas da
           nova nota por ~20 ms. Saltos para um harmônico, bem mais. */
        int jump = note - t->current;
        needed = t->legato_needed;
        if (jump == 12 || jump == 19 || jump == 24 || jump == 28 || jump == 31 || jump == -12 || jump == -19 || jump == -24)
            needed = t->stable_needed * 8 * t->hop_scale;
        if (fix) needed = 2;
    } else if (t->fast_conf > 0 && t->conf >= t->fast_conf) needed = 1; /* palhetada, leitura muito limpa */

    if (t->cand_count >= needed) {
        /* Com palhetada: solta a anterior e ataca de novo.
           Sem palhetada (hammer-on, slide): o sintetizador faz legato. */
        int with_pick = t->current < 0 || t->attack_pending;
        if (t->current >= 0 && t->attack_pending) out[n++] = mk(EV_NOTE_OFF, t->current, 0, 0.0f);
        out[n++] = mk(EV_NOTE_ON, note, db_to_velocity(t, db), midi);
        t->picked_on = with_pick; /* uma correção não abre outra */
        t->since_on       = 0;
        t->current        = note;
        t->cand_count     = 0;
        t->attack_pending = 0;
    }
    return n;
}
