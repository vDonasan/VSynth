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

/* amostra do histórico filtrado d amostras atrás do índice absoluto n
   (atraso fracionário por interpolação linear) */
static float hist_at(const Guitar *g, float n)
{
    float fl = floorf(n);
    unsigned i = (unsigned)(long long)fl;
    float fr = n - fl;
    float a = g->hist[i & (GTR_HIST - 1)], b = g->hist[(i + 1) & (GTR_HIST - 1)];
    return a + fr * (b - a);
}

/* Amostra n (índice absoluto) do sinal com os pentes aplicados: produto de
   (1 - z^-P) para cada nota cancelada, expandido nas 2^k combinações. */
static float cancelled_at(const Guitar *g, unsigned n)
{
    float sum = 0.0f;
    int combos = 1 << g->n_cancel;
    for (int m = 0; m < combos; m++) {
        float d = 0.0f;
        int sign = 1;
        for (int k = 0; k < g->n_cancel; k++)
            if (m & (1 << k)) { d += g->cancel_p[k]; sign = -sign; }
        sum += (float)sign * hist_at(g, (float)n - d);
    }
    return sum / (float)combos; /* volta mais ou menos à escala original */
}

/* Energia do trecho [from, to) com os pentes dos períodos p[0..n) */
static double comb_energy(const Guitar *g, const float *p, int n, unsigned from, unsigned to)
{
    double e = 0.0;
    for (unsigned i = from; i < to; i++) {
        float sum = 0.0f;
        for (int m = 0; m < (1 << n); m++) {
            float d = 0.0f;
            int sign = 1;
            for (int k = 0; k < n; k++)
                if (m & (1 << k)) { d += p[k]; sign = -sign; }
            sum += (float)sign * hist_at(g, (float)i - d);
        }
        e += (double)sum * sum / (double)(1 << n);
    }
    return e;
}

/* Na palhetada: das notas candidatas (anteriores e a atual), quais ainda
   estavam soando logo ANTES dela? Uma nota abafada não pode ser cancelada:
   o pente estragaria a nota nova quando as duas são parentes (oitava,
   quinta). Teste: tirar o pente de uma nota faz a energia que sobra subir
   bastante (ela era parte do som). */
static int present_notes(const Guitar *g, const float *cand, int n, float *out)
{
    unsigned to = g->pick_abs > 96 ? g->pick_abs - 96 : 0, from = to > 1200 ? to - 1200 : 0;
    if (n == 0 || to <= from) return 0;
    double raw = comb_energy(g, NULL, 0, from, to) + 1e-12;
    double all = comb_energy(g, cand, n, from, to);
    if (all > 0.35 * raw) { /* nem juntas elas explicam o som: testa uma a uma */
        int m = 0;
        for (int i = 0; i < n; i++)
            if (comb_energy(g, cand + i, 1, from, to) < 0.35 * raw) out[m++] = cand[i];
        return m;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {
        float others[GTR_RING_MAX + 1];
        int k = 0;
        for (int j = 0; j < n; j++) if (j != i) others[k++] = cand[j];
        double without = comb_energy(g, others, k, from, to);
        if (without > 2.0 * all + 0.02 * raw) out[m++] = cand[i]; /* sem ela, sobra bem mais: soava */
    }
    return m;
}

/* Energia do histórico em f Hz no trecho [from, to) (Goertzel com janela de Hann) */
static float goertzel(const Guitar *g, float f, unsigned from, unsigned to)
{
    float w = 6.2831853f * f / g->sr, c = 2.0f * cosf(w), s1 = 0.0f, s2 = 0.0f;
    unsigned n = to - from;
    for (unsigned i = 0; i < n; i++) {
        float win = 0.5f - 0.5f * cosf(6.2831853f * (float)i / (float)n);
        float s0 = win * g->hist[(from + i) & (GTR_HIST - 1)] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / ((float)n * (float)n);
}

/* A nota nova é uma oitava (ou quinta, ou duas oitavas) de uma corda que
   está soando? O pente cancela as duas juntas, mas quando ela entra só os
   harmônicos múltiplos de k da nota antiga ganham força; os outros seguem
   caindo. Devolve a frequência da nota nova, ou 0. */
static float harmonic_newcomer(const Guitar *g, unsigned end)
{
    unsigned pre_to = g->pick_abs > 96 ? g->pick_abs - 96 : 0, pre_from = pre_to > 1024 ? pre_to - 1024 : 0;
    unsigned post_from = end - 768;
    if (pre_to <= pre_from || post_from <= g->pick_abs || g->n_att == 0) return 0.0f;
    /* crescimento em cada harmônico das notas que soavam */
    float fr[GTR_RING_MAX + 1][9], gr[GTR_RING_MAX + 1][9];
    int nh[GTR_RING_MAX + 1];
    for (int r = 0; r < g->n_att; r++) {
        float f0 = g->sr / g->att_p[r];
        nh[r] = 0;
        for (int h = 1; h <= 8 && h * f0 < 1500.0f; h++) {
            float pre = goertzel(g, h * f0, pre_from, pre_to) + 1e-12f;
            fr[r][nh[r]] = h * f0;
            gr[r][nh[r]++] = goertzel(g, h * f0, post_from, end) / pre;
        }
    }
    float best_hz = 0.0f, best = 0.0f;
    for (int r = 0; r < g->n_att; r++)
        for (int k = 2; k <= 4; k++) {
            float f = k * g->sr / g->att_p[r];
            if (f > 1200.0f) break;
            /* os harmônicos das cordas antigas que são da candidata devem ter
               subido; os que não são dela, ficado parados */
            float in = 0.0f, out = 0.0f;
            int ni = 0, no = 0;
            for (int q = 0; q < g->n_att; q++)
                for (int i = 0; i < nh[q]; i++) {
                    float m = fr[q][i] / f;
                    if (fabsf(m - roundf(m)) < 0.03f * roundf(m) && roundf(m) >= 1.0f) { in += gr[q][i]; ni++; }
                    else { out += gr[q][i]; no++; }
                }
            if (ni < 1 || no < 2) continue;
            in /= ni;
            out /= no;
            float contrast = in / (out + 1e-6f);
            if (in > 2.5f && out < 1.3f && contrast > 4.0f && contrast > best) { best = contrast; best_hz = f; }
        }
    return best_hz;
}

/* Quais notas cancelar agora: depois de uma palhetada (nota nova ainda não
   confirmada), as que estavam soando antes dela; durante a nota, as
   anteriores que continuam soando (menos a própria nota). */
static int want_cancel(const Guitar *g, float *p)
{
    if (g->tracker.current >= 0 && g->tracker.attack_pending) {
        memcpy(p, g->att_p, g->n_att * sizeof(float));
        return g->n_att;
    }
    int n = 0;
    for (int i = 0; i < g->n_ring && n < GTR_RING_MAX; i++)
        if (g->tracker.current < 0 || (int)lroundf(hz_to_midi(g->ring_hz[i])) != g->tracker.current)
            p[n++] = g->sr / g->ring_hz[i];
    return n;
}

static void ring_add(Guitar *g, float hz)
{
    for (int i = 0; i < g->n_ring; i++)
        if (fabsf(12.0f * log2f(g->ring_hz[i] / hz)) < 0.5f) { g->ring_hz[i] = hz; g->ring_age[i] = 0; return; }
    if (g->n_ring == GTR_RING_MAX) { /* sai a mais antiga */
        memmove(g->ring_hz, g->ring_hz + 1, (GTR_RING_MAX - 1) * sizeof(float));
        memmove(g->ring_age, g->ring_age + 1, (GTR_RING_MAX - 1) * sizeof(int));
        g->n_ring--;
    }
    g->ring_hz[g->n_ring] = hz;
    g->ring_age[g->n_ring++] = 0;
}

static void ring_remove_note(Guitar *g, int note)
{
    for (int i = 0; i < g->n_ring; i++)
        if ((int)lroundf(hz_to_midi(g->ring_hz[i])) == note) {
            memmove(g->ring_hz + i, g->ring_hz + i + 1, (g->n_ring - i - 1) * sizeof(float));
            memmove(g->ring_age + i, g->ring_age + i + 1, (g->n_ring - i - 1) * sizeof(int));
            g->n_ring--;
            i--;
        }
}

/* Leitura de altura com as três janelas (curta, média, longa) */
static float read_pitch(Guitar *g, Yin *ys, Yin *ym, Yin *yl, int ok_s, int ok_m, int ok_l, float *conf_out, Yin **used_out)
{
    float conf = 0.0f, cm = 0.0f, hz;
    Yin *used = ys;
    float hs = ok_s ? yin_eval(ys, &conf) : -1.0f;
    if (hs >= PITCH_SHORT_MIN_HZ && conf > 0.85f) hz = hs;
    else if (ok_m && (hz = yin_eval(ym, &cm)) > 0 && cm > 0.85f) { used = ym; conf = cm; }
    else if (ok_l) { hz = yin_eval(yl, &conf); used = yl; }
    else { hz = -1.0f; conf = 0.0f; }
    (void)g;
    *conf_out = hz > 0 ? conf : 0.0f;
    if (used_out) *used_out = used;
    return hz;
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
        g->hist[(g->hist_pos + (unsigned)i) & (GTR_HIST - 1)] = g->lp_y1;
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
    if (g->tracker.pick && g->tracker.current >= 0) {
        /* candidatas: as notas anteriores ainda na lista e a que está soando */
        float cand[GTR_RING_MAX + 1];
        int nc = 0;
        for (int i = 0; i < g->n_ring; i++) cand[nc++] = g->sr / g->ring_hz[i];
        if (g->held_hz > 0 && nc < GTR_RING_MAX + 1) cand[nc++] = g->sr / g->held_hz;
        g->pick_abs = g->hist_pos; /* começo deste passo */
        g->n_att = present_notes(g, cand, nc, g->att_p);
        /* as que já não soavam saem da lista */
        for (int i = 0; i < g->n_ring; i++) {
            int keep = 0;
            for (int j = 0; j < g->n_att; j++) if (fabsf(g->att_p[j] - g->sr / g->ring_hz[i]) < 0.5f) keep = 1;
            if (!keep) { ring_remove_note(g, (int)lroundf(hz_to_midi(g->ring_hz[i]))); i--; }
        }
        g->held_present = 0;
        for (int j = 0; j < g->n_att; j++) if (g->held_hz > 0 && fabsf(g->att_p[j] - g->sr / g->held_hz) < 0.5f) g->held_present = 1;
    }

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

    /* Cancelamento: se o conjunto de notas a cancelar mudou, refaz a janela
       cancelada inteira (custa uma análise completa, só nessa hora); senão
       só acrescenta o passo novo, como na análise normal. */
    unsigned end = g->hist_pos + PITCH_HOP; /* índice absoluto logo depois da última amostra */
    float want[GTR_RING_MAX + 1];
    int nw = want_cancel(g, want), changed = nw != g->n_cancel;
    for (int i = 0; i < nw && !changed; i++) changed = fabsf(want[i] - g->cancel_p[i]) > 0.002f * want[i];
    if (nw > 0) {
        int total = PITCH_WINDOW + PITCH_HOP;
        if (changed) {
            memcpy(g->cancel_p, want, nw * sizeof(float));
            g->n_cancel = nw;
            for (int i = 0; i < total; i++) g->cwin[i] = cancelled_at(g, end - (unsigned)(total - i));
            yin_detect(&g->cyin_short, g->cwin + total - PITCH_SHORT_WINDOW, NULL);
            yin_detect(&g->cyin_mid, g->cwin + total - PITCH_MID_WINDOW, NULL);
            yin_detect(&g->cyin, g->cwin + total - PITCH_WINDOW, NULL);
        } else {
            for (int i = 0; i < PITCH_HOP; i++) g->cwin[PITCH_WINDOW + i] = cancelled_at(g, end - (unsigned)(PITCH_HOP - i));
            yin_slide(&g->cyin_short, g->cwin + PITCH_WINDOW - PITCH_SHORT_WINDOW, PITCH_HOP);
            yin_slide(&g->cyin_mid, g->cwin + PITCH_WINDOW - PITCH_MID_WINDOW, PITCH_HOP);
            yin_slide(&g->cyin, g->cwin, PITCH_HOP);
        }
        memmove(g->cwin, g->cwin + PITCH_HOP, PITCH_WINDOW * sizeof(float));
    } else g->n_cancel = 0;
    g->hist_pos = end;
    for (int i = 0; i < g->n_ring; i++) g->ring_age[i] += PITCH_HOP;
    while (g->n_ring > 0 && g->ring_age[0] > (int)(4.0f * g->sr)) ring_remove_note(g, (int)lroundf(hz_to_midi(g->ring_hz[0])));

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
        int cancelled = 0;
        hz = read_pitch(g, &g->yin_short, &g->yin_mid, &g->yin, ok_short, ok_mid, ok_long, &conf, &used);
        if (g->n_cancel > 0) {
            /* Com notas anteriores soando, vale a leitura do sinal cancelado.
               Se nele não sobra nota: logo depois da palhetada, foi a mesma
               nota palhetada de novo (vale a leitura normal se for ela);
               durante a nota, nada muda (não pula para outra corda). */
            float cc = 0.0f;
            Yin *cu = NULL;
            float hc = read_pitch(g, &g->cyin_short, &g->cyin_mid, &g->cyin, ok_short, ok_mid, ok_long, &cc, &cu);
            if (hc > 0) { hz = hc; conf = cc; used = cu; cancelled = 1; }
            else {
                /* nada sobrou: oitava/quinta de uma corda soando, ou a mesma
                   nota palhetada de novo */
                float hn = g->tracker.attack_pending && g->fresh >= 768 ? harmonic_newcomer(g, end) : 0.0f;
                if (hn > 0) { hz = hn; conf = 0.9f; cancelled = 1; }
                else if (hz > 0) {
                    int rn = (int)lroundf(hz_to_midi(hz));
                    if (!(g->tracker.attack_pending && rn == g->tracker.current)) hz = -1.0f;
                }
            }
        }
        g->tracker.conf = hz > 0 ? conf : 0;

        /* Correção de oitava por continuidade: com uma nota soando, uma
           leitura de 2x, 3x ou 4x a frequência dela costuma ser um
           harmônico forte. Se o período da nota atual ainda "encaixa"
           bem na curva do YIN, fica com a nota atual. Logo depois de uma
           palhetada a correção fica desligada: aí o salto é de verdade. */
        if (hz > 0 && !cancelled && g->tracker.current >= 0 && g->held_hz > 0 && !g->tracker.attack_pending) {
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
    int prev_note = g->tracker.current;
    float prev_hz = g->held_hz;
    int n = tracker_process(&g->tracker, hz, db, ev, 4);
    for (int i = 0; i < n; i++) {
        evq_push(g->q, ev[i]);
        if (ev[i].type == EV_NOTE_ON) {
            /* a nota anterior continua soando (corda deixada soar): entra na
               lista para ser cancelada; a nova sai dela */
            if (prev_note >= 0 && prev_note != ev[i].note && prev_hz > 0 && g->held_present) ring_add(g, prev_hz);
            ring_remove_note(g, ev[i].note);
        }
    }
    if (g->tracker.current < 0) g->n_ring = 0; /* silêncio: nada mais soando */
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
    memset(g->hist, 0, sizeof g->hist);
    memset(g->cwin, 0, sizeof g->cwin);
    g->hist_pos = 0;
    g->n_ring = g->n_cancel = g->n_att = 0;
    g->held_present = 0;
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
    if (yin_init(&g->cyin, sr, PITCH_WINDOW, PITCH_FMIN, PITCH_FMAX, PITCH_THRESHOLD) < 0) return -1;
    if (yin_init(&g->cyin_short, sr, PITCH_SHORT_WINDOW, PITCH_SHORT_FMIN, PITCH_FMAX, PITCH_THRESHOLD) < 0) return -1;
    if (yin_init(&g->cyin_mid, sr, PITCH_MID_WINDOW, PITCH_MID_FMIN, PITCH_FMAX, PITCH_THRESHOLD) < 0) return -1;
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
    yin_free(&g->cyin);
    yin_free(&g->cyin_short);
    yin_free(&g->cyin_mid);
}
