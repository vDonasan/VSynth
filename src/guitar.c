#include "guitar.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "params.h"

#include "platform.h"

void guitar_push(Guitar *g, const float *mono, int frames)
{
    for (int i = 0; i < frames; i++) fring_push(&g->ring, mono[i]);
}

void guitar_process_hop(Guitar *g, const float *hop)
{
    /* gate e ganho podem ser mexidos na interface a qualquer momento */
    float gate = param_get(P_GTR_GATE), gain = param_get(P_GTR_GAIN);
    g->tracker.on_db  = gate;
    g->tracker.off_db = gate - GATE_HYSTERESIS_DB;

    /* desliza a janela e acrescenta as amostras novas, já filtradas */
    memmove(g->window, g->window + PITCH_HOP, (PITCH_WINDOW - PITCH_HOP) * sizeof(float));
    float *dst = g->window + PITCH_WINDOW - PITCH_HOP;
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
    float db = 20.0f * log10f(sqrtf(energy / PITCH_HOP) + 1e-9f);

    float hz = -1.0f;
    if (db > g->tracker.off_db) {
        /* Janela curta primeiro: uma nota aguda é reconhecida assim que
           ~11 ms dela entram na janela, sem esperar as maiores. O YIN
           sempre escolhe o MENOR período que passa no teste, então uma
           janela menor não inventa erros de oitava que a longa não teria. */
        float conf = 0.0f;
        Yin *used = &g->yin_short;
        float hs = yin_detect(&g->yin_short, g->window + PITCH_WINDOW - PITCH_SHORT_WINDOW, &conf);
        float cm = 0;
        if (hs >= PITCH_SHORT_MIN_HZ && conf > 0.85f) hz = hs;
        else if ((hz = yin_detect(&g->yin_mid, g->window + PITCH_WINDOW - PITCH_MID_WINDOW, &cm)) > 0 && cm > 0.85f) {
            used = &g->yin_mid; /* janela média: cordas graves sem esperar a longa */
            conf = cm;
        }
        else { hz = yin_detect(&g->yin, g->window, &conf); used = &g->yin; }
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
                    yin_detect(&g->yin, g->window, NULL);
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

static void *guitar_thread(void *arg)
{
    Guitar *g = arg;
    float hop[PITCH_HOP];
    while (atomic_load(&g->running)) {
        if (fring_available(&g->ring) < PITCH_HOP) { sleep_ms(1); continue; }
        fring_read(&g->ring, hop, PITCH_HOP);
        guitar_process_hop(g, hop);
    }
    return NULL;
}

void guitar_reset(Guitar *g)
{
    g->dc_x1 = g->dc_y1 = g->lp_y1 = 0.0f;
    memset(g->window, 0, sizeof g->window);
    tracker_init(&g->tracker, param_get(P_GTR_GATE), GATE_HYSTERESIS_DB);
    tracker_set_refractory(&g->tracker, 60.0f, g->sr, PITCH_HOP);
    tracker_set_settle(&g->tracker, PITCH_SETTLE_HOPS);
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
    atomic_init(&g->running, false);
    atomic_init(&g->st_note, -1);
    atomic_init(&g->st_hz, -1.0f);
    atomic_init(&g->st_db, -120.0f);
    atomic_init(&g->level, 0.0f);
    if (fring_init(&g->ring, 32768) < 0) return -1;
    if (yin_init(&g->yin, sr, PITCH_WINDOW, PITCH_FMIN, PITCH_FMAX, PITCH_THRESHOLD) < 0) return -1;
    if (yin_init(&g->yin_short, sr, PITCH_SHORT_WINDOW, PITCH_SHORT_FMIN, PITCH_FMAX, PITCH_THRESHOLD) < 0) return -1;
    if (yin_init(&g->yin_mid, sr, PITCH_MID_WINDOW, PITCH_MID_FMIN, PITCH_FMAX, PITCH_THRESHOLD) < 0) return -1;
    guitar_reset(g);
    return 0;
}

int guitar_start(Guitar *g)
{
    if (g->started) return 0;
    fring_clear(&g->ring);
    guitar_reset(g);
    atomic_store(&g->running, true);
    pthread_t *t = malloc(sizeof *t);
    if (!t || pthread_create(t, NULL, guitar_thread, g) != 0) { free(t); return -1; }
    g->thread = t;
    g->started = 1;
    return 0;
}

void guitar_stop(Guitar *g)
{
    if (!g->started) return;
    atomic_store(&g->running, false);
    pthread_join(*(pthread_t *)g->thread, NULL);
    free(g->thread);
    g->thread = NULL;
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
    fring_free(&g->ring);
}
