#include "guitar.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "params.h"


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
    for (int i = 0; i < PITCH_HOP; i++) {
        float x = hop[i] * gain;
        float y = x - g->dc_x1 + 0.995f * g->dc_y1; /* bloqueador de DC */
        g->dc_x1 = x;
        g->dc_y1 = y;
        g->lp_y1 += g->lp_coef * (y - g->lp_y1);    /* passa-baixa de 1 polo */
        dst[i] = g->lp_y1;
        /* volume: sobe na hora, desce devagar (40 ms) */
        float a = fabsf(y);
        g->env = a > g->env ? a : g->env * g->env_rel;
        /* agudos (passa-altas de 2 polos em ~2 kHz) para achar a palhetada */
        g->hp1 += g->hp_coef * (y - g->hp1);
        float h1 = y - g->hp1;
        g->hp2 += g->hp_coef * (h1 - g->hp2);
        float h = fabsf(h1 - g->hp2);
        g->hf_env = h > g->hf_env ? h : g->hf_env * g->hf_rel;
    }
    /* -3 dB: o pico de uma senoide fica 3 dB acima do valor eficaz, que é
       a referência do gate e da calibração */
    float db = 20.0f * log10f(g->env + 1e-9f) - 3.0f;

    /* Palhetada: os agudos dão um salto em até ~8 ms (o ataque da palheta).
       Batidas entre harmônicos e cordas vibrando juntas mexem no volume,
       mas não fazem esse salto nos agudos. */
    float hf_db = 20.0f * log10f(g->hf_env + 1e-9f);
    float hf_min = hf_db;
    for (int i = 0; i < 6; i++) if (g->hf_hist[i] < hf_min) hf_min = g->hf_hist[i];
    g->hf_hist[g->hf_pos] = hf_db;
    g->hf_pos = (g->hf_pos + 1) % 6;
    /* Quanto os agudos precisam subir depende da nota soando: uma nota grave
       sozinha já oscila ~10 dB nos agudos (muitos harmônicos batendo), e numa
       nota aguda a palhetada nova sobe só ~7-11 dB. Medido nota a nota:
       15 dB no Mi grave, descendo até 6 dB nas notas mais agudas. */
    float thr = 15.0f;
    if (g->tracker.current >= 0) {
        thr = 15.0f - 0.22f * (g->tracker.current - 40);
        if (thr > 15.0f) thr = 15.0f;
        if (thr < 6.0f) thr = 6.0f;
    }
    /* Uma palhetada também aumenta o volume; abafar a corda faz um ruído
       agudo parecido, mas com o volume caindo. */
    float db_min = db;
    for (int i = 0; i < 6; i++) if (g->db_hist[i] < db_min) db_min = g->db_hist[i];
    g->db_hist[(g->hf_pos + 5) % 6] = db;
    /* o estalo dura alguns passos: conta só o começo dele */
    int hf_jump = hf_db - hf_min > thr && hf_db > g->tracker.off_db - 12.0f && db - db_min > 1.0f;
    g->tracker.pick = hf_jump && !g->pick_hold;
    if (g->tracker.pick) atomic_fetch_add_explicit(&g->pick_count, 1, memory_order_relaxed);
    g->pick_hold = hf_jump;

    /* Quanto da janela de análise já é som depois da palhetada. Uma janela
       ainda cheia do que soava antes (fim da nota anterior, cordas soltas
       vibrando) dá leituras erradas. O YIN compara o COMEÇO da janela com
       ela mesma atrasada, e o começo é a parte mais antiga: cada janela só
       vale quando estiver (quase) toda com som novo. */
    int from_silence = g->tracker.current < 0 && db > g->tracker.on_db && g->prev_db <= g->tracker.on_db;
    if (g->tracker.pick || from_silence) g->fresh = 0;
    else if (g->fresh < 1 << 20) g->fresh += PITCH_HOP;
    g->prev_db = db;
    const int ok_short = g->fresh >= PITCH_SHORT_WINDOW * 19 / 20, ok_mid = g->fresh >= PITCH_MID_WINDOW * 19 / 20,
              ok_long = g->fresh >= PITCH_WINDOW * 19 / 20;

    /* Atualiza as três análises só com o que entrou e saiu da janela
       (custo pequeno e igual em todo passo) e depois anda a janela. */
    yin_slide(&g->yin_short, g->window + PITCH_WINDOW - PITCH_SHORT_WINDOW, PITCH_HOP);
    yin_slide(&g->yin_mid, g->window + PITCH_WINDOW - PITCH_MID_WINDOW, PITCH_HOP);
    yin_slide(&g->yin, g->window, PITCH_HOP);
    memmove(g->window, g->window + PITCH_HOP, PITCH_WINDOW * sizeof(float));

    float hz = -1.0f;
    /* com uma nota soando, continua lendo abaixo do gate (o fim da nota é
       decidido pela altura, ver tracker_process) */
    float read_floor = g->tracker.current >= 0 ? g->tracker.off_db - PITCH_TAIL_DB : g->tracker.off_db;
    if (db > read_floor) {
        /* Janela curta primeiro: uma nota aguda é reconhecida assim que
           ~11 ms dela entram na janela, sem esperar as maiores. O YIN
           sempre escolhe o MENOR período que passa no teste, então uma
           janela menor não inventa erros de oitava que a longa não teria. */
        float conf = 0.0f;
        Yin *used = &g->yin_short;
        float hs = ok_short ? yin_eval(&g->yin_short, &conf) : -1.0f;
        float cm = 0;
        if (hs >= PITCH_SHORT_MIN_HZ && conf > 0.85f) hz = hs;
        else if (ok_mid && (hz = yin_eval(&g->yin_mid, &cm)) > 0 && cm > 0.85f) {
            used = &g->yin_mid; /* janela média: cordas graves sem esperar a longa */
            conf = cm;
        }
        else if (ok_long) { hz = yin_eval(&g->yin, &conf); used = &g->yin; }
        else { hz = -1.0f; conf = 0.0f; } /* janelas ainda com o som de antes: espera */
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
    g->env = g->hf_env = g->hp1 = g->hp2 = 0.0f;
    for (int i = 0; i < 8; i++) g->hf_hist[i] = -120.0f;
    for (int i = 0; i < 6; i++) g->db_hist[i] = -120.0f;
    g->hf_pos = 0;
    g->fresh = 1 << 20;
    g->pick_hold = 0;
    g->prev_db = -120.0f;
    g->env_rel = expf(-1.0f / (0.040f * g->sr));
    g->hf_rel  = expf(-1.0f / (0.005f * g->sr));
    g->hp_coef = 1.0f - expf(-6.2831853f * 2000.0f / g->sr);
    g->hop_fill = 0;
    tracker_init(&g->tracker, param_get(P_GTR_GATE), GATE_HYSTERESIS_DB);
    tracker_set_refractory(&g->tracker, 60.0f, g->sr, PITCH_HOP);
    tracker_set_settle(&g->tracker, (int)(PITCH_SETTLE_MS / 1000.0f * g->sr / PITCH_HOP + 0.5f));
    tracker_set_hop(&g->tracker, PITCH_HOP);
    g->tracker.external_pick = 1; /* palhetada vem do detector de agudos acima */
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
