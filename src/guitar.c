#include "guitar.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "params.h"

#define LEVEL_HOPS (PITCH_LEVEL_WIN / PITCH_HOP > 0 ? PITCH_LEVEL_WIN / PITCH_HOP : 1)

int guitar_feed(Guitar *g, const float *mono, int frames)
{
    int done = 0;
    for (int i = 0; i < frames; i++) {
        g->hop_buf[g->hop_fill++] = mono[i];
        if (g->hop_fill == PITCH_HOP) {
            guitar_process_hop(g, g->hop_buf);
            g->hop_fill = 0;
            done++;
        }
    }
    return done;
}

void guitar_process_hop(Guitar *g, const float *hop)
{
    /* gate e ganho podem ser mexidos na interface a qualquer momento */
    float gate = param_get(P_GTR_GATE), gain = param_get(P_GTR_GAIN);
    g->tracker.on_db  = gate;
    g->tracker.off_db = gate - GATE_HYSTERESIS_DB;

    /* acrescenta as amostras novas, já filtradas, logo depois da janela */
    float *dst = g->window + PITCH_WINDOW;
    float energy = 0.0f;
    for (int i = 0; i < PITCH_HOP; i++) {
        float x = hop[i] * gain;
        float y = x - g->dc_x1 + 0.995f * g->dc_y1; /* bloqueador de DC */
        g->dc_x1 = x;
        g->dc_y1 = y;
        g->lp_y1 += g->lp_coef * (y - g->lp_y1);    /* passa-baixa de 1 polo */
        dst[i] = g->lp_y1;
        energy += y * y;
    }
    /* volume nas últimas PITCH_LEVEL_WIN amostras (os passos curtos
       demais oscilariam dentro de um ciclo de uma corda grave) */
    g->hop_energy[g->energy_pos] = energy;
    g->energy_pos = (g->energy_pos + 1) % LEVEL_HOPS;
    float esum = 0.0f;
    for (int i = 0; i < LEVEL_HOPS; i++) esum += g->hop_energy[i];
    float db = 20.0f * log10f(sqrtf(esum / (LEVEL_HOPS * PITCH_HOP)) + 1e-9f);

    /* Atualiza as três análises só com o que entrou e saiu da janela
       (custo pequeno e igual em todo passo) e depois anda a janela. */
    yin_slide(&g->yin_short, g->window + PITCH_WINDOW - PITCH_SHORT_WINDOW, PITCH_HOP);
    yin_slide(&g->yin_mid, g->window + PITCH_WINDOW - PITCH_MID_WINDOW, PITCH_HOP);
    yin_slide(&g->yin, g->window, PITCH_HOP);
    memmove(g->window, g->window + PITCH_HOP, PITCH_WINDOW * sizeof(float));

    float hz = -1.0f;
    if (db > g->tracker.off_db) {
        /* Janela curta primeiro: uma nota aguda é reconhecida assim que
           ~11 ms dela entram na janela, sem esperar as maiores. O YIN
           sempre escolhe o MENOR período que passa no teste, então uma
           janela menor não inventa erros de oitava que a longa não teria. */
        float conf = 0.0f;
        Yin *used = &g->yin_short;
        float hs = yin_eval(&g->yin_short, &conf);
        float cm = 0;
        if (hs >= PITCH_SHORT_MIN_HZ && conf > 0.85f) hz = hs;
        else if ((hz = yin_eval(&g->yin_mid, &cm)) > 0 && cm > 0.85f) {
            used = &g->yin_mid; /* janela média: cordas graves sem esperar a longa */
            conf = cm;
        }
        else { hz = yin_eval(&g->yin, &conf); used = &g->yin; }
        g->tracker.conf = hz > 0 ? conf : 0;

        /* Correção de oitava por continuidade: com uma nota soando, uma
           leitura de 2x, 3x ou 4x a frequência dela costuma ser um
           harmônico forte. Se o período da nota atual ainda "encaixa"
           bem na curva do YIN, fica com a nota atual. Logo depois de uma
           palhetada a correção fica desligada: aí o salto é de verdade. */
        if (hz > 0 && g->tracker.current >= 0 && g->held_hz > 0 && !g->tracker.attack_pending) {
            float ratio = hz / g->held_hz;
            int k = (int)lroundf(ratio);
            if (k >= 2 && k <= 4 && fabsf(ratio - k) < 0.03f * k) {
                float tau = g->sr / (hz / k);
                if (tau > used->tau_max && used != &g->yin) { /* janela curta não alcança */
                    yin_eval(&g->yin, NULL);
                    used = &g->yin;
                }
                /* Numa oitava de verdade os dois períodos encaixam igualmente
                   bem; no erro de harmônico, o da nota atual encaixa melhor. */
                float fit_cur = yin_cmnd_at(used, tau);
                float fit_new = yin_cmnd_at(used, g->sr / hz);
                if (fit_cur < 0.35f && fit_cur < 0.7f * fit_new) hz /= (float)k;
            }
        }
    }

    Event ev[4];
    int n = tracker_process(&g->tracker, hz, db, ev, 4);
    for (int i = 0; i < n; i++) evq_push(g->q, ev[i]);
    if (g->tracker.current < 0) g->held_hz = 0;
    else if (hz > 0 && (int)lroundf(hz_to_midi(hz)) == g->tracker.current) g->held_hz = hz;

    float level = (db - gate + 20.0f) / 40.0f; /* gate-20 dB -> 0, gate+20 dB -> 1 */
    atomic_store(&g->level, level < 0 ? 0 : (level > 1 ? 1 : level));
    atomic_store(&g->st_note, g->tracker.current);
    atomic_store(&g->st_hz, hz);
    atomic_store(&g->st_db, db);
}

void guitar_reset(Guitar *g)
{
    g->dc_x1 = g->dc_y1 = g->lp_y1 = 0.0f;
    memset(g->window, 0, sizeof g->window);
    for (int t = 0; t <= g->yin.tau_max; t++) g->yin.acc[t] = 0;
    for (int t = 0; t <= g->yin_mid.tau_max; t++) g->yin_mid.acc[t] = 0;
    for (int t = 0; t <= g->yin_short.tau_max; t++) g->yin_short.acc[t] = 0;
    memset(g->hop_energy, 0, sizeof g->hop_energy);
    g->energy_pos = 0;
    g->hop_fill = 0;
    tracker_init(&g->tracker, param_get(P_GTR_GATE), GATE_HYSTERESIS_DB);
    tracker_set_refractory(&g->tracker, 60.0f, g->sr, PITCH_HOP);
    tracker_set_settle(&g->tracker, (int)(PITCH_SETTLE_MS / 1000.0f * g->sr / PITCH_HOP + 0.5f));
    tracker_set_hop(&g->tracker, PITCH_HOP);
    g->tracker.fast_conf = PITCH_FAST_CONF;
    atomic_store(&g->st_note, -1);
    atomic_store(&g->st_hz, -1.0f);
    atomic_store(&g->st_db, -120.0f);
    atomic_store(&g->level, 0.0f);
    g->held_hz = 0;
}

int guitar_init(Guitar *g, float sr, EventQueue *q)
{
    memset(g, 0, sizeof *g);
    g->q       = q;
    g->sr      = sr;
    g->lp_coef = 1.0f - expf(-2.0f * 3.14159265f * 1600.0f / sr);
    atomic_init(&g->st_note, -1);
    atomic_init(&g->st_hz, -1.0f);
    atomic_init(&g->st_db, -120.0f);
    atomic_init(&g->level, 0.0f);
    if (yin_init(&g->yin, sr, PITCH_WINDOW, PITCH_FMIN, PITCH_FMAX, PITCH_THRESHOLD) < 0) return -1;
    if (yin_init(&g->yin_short, sr, PITCH_SHORT_WINDOW, PITCH_SHORT_FMIN, PITCH_FMAX, PITCH_THRESHOLD) < 0) return -1;
    if (yin_init(&g->yin_mid, sr, PITCH_MID_WINDOW, PITCH_MID_FMIN, PITCH_FMAX, PITCH_THRESHOLD) < 0) return -1;
    guitar_reset(g);
    return 0;
}

int guitar_start(Guitar *g)
{
    if (g->started) return 0;
    guitar_reset(g);
    g->started = 1;
    return 0;
}

void guitar_stop(Guitar *g)
{
    if (!g->started) return;
    g->started = 0;
    if (g->tracker.current >= 0) { /* não deixa nota presa */
        Event off = {EV_NOTE_OFF, SRC_AUDIO, (uint8_t)g->tracker.current, 0, 0};
        evq_push(g->q, off);
    }
    guitar_reset(g);
}

void guitar_free(Guitar *g)
{
    guitar_stop(g);
    yin_free(&g->yin);
    yin_free(&g->yin_short);
    yin_free(&g->yin_mid);
}
