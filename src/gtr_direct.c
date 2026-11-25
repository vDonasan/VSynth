/* Modo direto da guitarra: ver gtr_direct.h. */
#include "gtr_direct.h"

#include <math.h>
#include <string.h>

#include "params.h"

#define TWO_PI 6.28318530718f
#define MIN_HZ 55.0f
#define MAX_HZ 1400.0f

static void set_band(Direct *d);

void direct_init(Direct *d, float sr)
{
    memset(d, 0, sizeof *d);
    d->sr = sr;
    direct_reset(d);
}

void direct_reset(Direct *d)
{
    float sr = d->sr;
    memset(d, 0, sizeof *d);
    d->sr     = sr;
    d->state  = -1;
    d->period = sr / 110.0f;
    d->center = 0.0f;
    set_band(d);
    d->aenv.stage = d->fenv.stage = ENV_IDLE;
}

float direct_hz(const Direct *d) { return d->have_period && d->gate ? d->sr / d->period : 0.0f; }

static float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }

static void set_band(Direct *d)
{
    float lo = 50.0f, hi = 500.0f; /* banda aberta: logo depois da palhetada */
    if (d->center > 0.0f) {
        lo = 0.7f * d->center;
        hi = 1.5f * d->center;
    }
    if (hi > 1800.0f) hi = 1800.0f;
    d->hp_coef = 1.0f - expf(-TWO_PI * lo / d->sr);
    d->lp_coef = 1.0f - expf(-TWO_PI * hi / d->sr);
}

void direct_process(Direct *d, const Patch *p, const float *in, float *out, int n, unsigned picks, float hint_hz)
{
    const float sr = d->sr;
    const float gain = param_get(P_GTR_GAIN);
    const float gate_on  = powf(10.0f, param_get(P_GTR_GATE) / 20.0f) * 1.41f; /* gate em dB eficaz -> pico */
    const float gate_off = gate_on * 0.4f;                                    /* -8 dB de histerese */
    const int   oct = param_geti(P_GTR_OCTAVE);
    const float oct_mul = exp2f((float)oct);
    const float detune_mul = exp2f(p->osc2_detune / 12.0f);
    const float lvl_att = 1.0f - expf(-1.0f / (0.0005f * sr));
    const float lvl_rel = expf(-1.0f / (0.060f * sr));
    const float env_rel = expf(-1.0f / (0.030f * sr));
    const float k = 2.0f - 1.95f * p->resonance;
    const float fast_rel = expf(-1.0f / (0.004f * sr));
    const float ref_rel  = expf(-1.0f / (0.080f * sr));
    const float low_coef = 1.0f - expf(-1.0f / (0.020f * sr));
    const float sync_coef = 1.0f - expf(-1.0f / (0.004f * sr));

    /* palhetada nova (vem do detector de agudos da guitarra): abre a banda,
       porque a próxima nota pode ser qualquer uma */
    int picked = picks != d->last_pick;
    d->last_pick = picks;
    if (picked) {
        d->off_time = 0.0f;
        d->center = 0.0f;
        d->hint_agree = 0;
        d->env_y = 0.0f;
        d->suspect = 0;
    }
    /* O reconhecimento (YIN) diz qual é a nota: a banda se fecha em volta
       dela. Uma nota diferente da banda atual só muda a banda depois de 3
       leituras seguidas (uma corda ressoando não arrasta a banda). */
    if (hint_hz > 0.0f) {
        if (d->center <= 0.0f) d->center = hint_hz;
        else if (fabsf(12.0f * log2f(hint_hz / d->center)) > 1.5f) {
            if (++d->hint_agree >= 3) { d->center = hint_hz; d->hint_agree = 0; }
        } else d->hint_agree = 0;
    }
    set_band(d);

    for (int i = 0; i < n; i++) {
        /* ---- análise: volume e ciclos da corda ---- */
        float x = in[i] * gain;
        float y0 = x - d->dc_x1 + 0.995f * d->dc_y1;
        d->dc_x1 = x;
        d->dc_y1 = y0;
        float a = fabsf(y0);
        d->level = a > d->level ? d->level + (a - d->level) * lvl_att : d->level * lvl_rel;
        /* corda abafada: o volume despenca (>18 dB em poucos ms), o que uma
           corda decaindo sozinha nunca faz */
        d->fast = a > d->fast ? a : d->fast * fast_rel;
        d->ref = d->fast > d->ref ? d->fast : d->ref * ref_rel;

        /* banda em volta da nota: sobra praticamente só a fundamental dela,
           sem harmônicos (que o comparador contaria como ciclos) e sem as
           cordas mais graves ressoando */
        d->hp1 += d->hp_coef * (y0 - d->hp1);
        float h1 = y0 - d->hp1;
        d->hp2 += d->hp_coef * (h1 - d->hp2);
        float h2 = h1 - d->hp2;
        d->hp3 += d->hp_coef * (h2 - d->hp3); /* 4 polos: corta forte as cordas mais graves */
        float h3 = h2 - d->hp3;
        d->hp4 += d->hp_coef * (h3 - d->hp4);
        h2 = h3 - d->hp4;
        d->lp1 += d->lp_coef * (h2 - d->lp1);
        d->lp2 += d->lp_coef * (d->lp1 - d->lp2);
        float y = d->lp2;
        float ay = fabsf(y);
        d->env_y = ay > d->env_y ? ay : d->env_y * env_rel;
        float h = 0.25f * d->env_y; /* histerese: ignora ondulações pequenas */

        d->since_edge += 1.0f;
        int edge = 0, rise = 0;
        if (d->state < 0 && y > h) {
            d->state = 1;
            rise = 1;
            /* posição exata da subida (interpolando entre as amostras) */
            float frac = (y - h) / fmaxf(y - d->y_prev, 1e-9f);
            frac = clampf(frac, 0.0f, 1.0f);
            float P = d->since_edge - frac;
            d->since_edge = frac;
            /* com a banda fechada, ciclos fora dela são de outra coisa (ruído,
               outra corda): ignora */
            float fP = sr / P;
            int in_band = d->center <= 0.0f || (fP > 0.7f * d->center && fP < 1.42f * d->center);
            /* Ciclo longe da nota confirmada = outra corda soando (a nota
               acabou). O dobro da frequência não conta: é só o harmônico
               da própria nota cruzando a mais. */
            if (d->center > 0.0f && P >= sr / MAX_HZ && P <= sr / MIN_HZ) {
                float st = 12.0f * log2f(fP / d->center);
                if (fabsf(st) > 0.8f && fabsf(st - 12.0f) > 0.8f) d->off_time += P;
                else if (fabsf(st) <= 0.8f) d->off_time = 0.0f;
            }
            if (P >= sr / MAX_HZ && P <= sr / MIN_HZ && d->level > gate_off && in_band) {
                /* ciclo muito mais curto que o anterior pode ser um harmônico
                   passando: só aceita se o próximo confirmar */
                if (d->center <= 0.0f && d->have_period && P < 0.6f * d->period && d->suspect == 0) d->suspect = 1;
                else {
                    d->period = P;
                    d->have_period = 1;
                    d->suspect = 0;
                }
                edge = 1;
                d->edges++;
            }
        } else if (d->state > 0 && y < -h) d->state = -1;
        d->y_prev = y;

        /* ---- gate e envelopes ---- */
        /* o som só abre com um ataque (palhetada, ou volume saindo de baixo
           do gate): cordas ressoando e zumbido constantes não abrem */
        /* "saindo do silêncio" só vale se o som estava mesmo baixo nos
           últimos ~20 ms (abaixo do fim do gate); ressonância batendo em
           volta do gate não conta */
        d->low = d->level < d->low ? d->level : d->low + (d->level - d->low) * low_coef;
        int crossed = d->level > gate_on && d->prev_level <= gate_on && d->low < gate_off;
        d->prev_level = d->level;
        if (!d->gate && d->level > gate_on && (picked || crossed)) {
            d->gate = 1;
            picked = 1;
        } else if (d->gate && (d->level < gate_off || (d->center > 0.0f && d->env_y < 0.5f * gate_off) ||
                               d->off_time > 0.030f * sr || (d->ref > gate_on && d->fast < 0.12f * d->ref))) {
            /* a nota acabou quando o ciclo medido fica 30 ms longe dela */
            /* com a nota confirmada, conta só o som dentro da banda dela:
               cordas ressoando fora da banda não seguram o som */
            d->gate = 0;
            d->aenv.stage = d->fenv.stage = ENV_RELEASE;
        }
        if (picked && d->gate) {
            d->aenv.stage = d->fenv.stage = ENV_ATTACK;
            d->velocity = clampf(d->level * 4.0f, 0.25f, 1.0f);
            picked = 0;
        }
        float amp_env = synth_adsr_next(&d->aenv, &p->amp_env, sr);
        float fenv    = synth_adsr_next(&d->fenv, &p->filt_env, sr);
        if (d->aenv.stage == ENV_IDLE && amp_env <= 0.0f) continue; /* em silêncio */

        /* ---- modulação (mesma matriz do synth) ---- */
        d->lfo_ph += p->lfo_rate / sr;
        if (d->lfo_ph >= 1.0f) d->lfo_ph -= 1.0f;
        float lfo = sinf(TWO_PI * d->lfo_ph);
        float lvl = clampf(d->level * 4.0f, 0.0f, 1.0f);
        float src[MOD_SRC_COUNT] = {1.0f, lfo, fenv, d->velocity, p->mod_wheel, lvl};
        float dst[DST_COUNT] = {0};
        for (int m = 0; m < MAX_MOD_SLOTS; m++) {
            const ModSlot *s = &p->mods[m];
            if (s->source == MOD_NONE || s->amount == 0.0f) continue;
            dst[s->dest] += src[s->source] * src[s->via] * s->amount;
        }

        /* ---- OSC1: a própria onda da corda, transformada (atraso zero) ---- */
        if (rise) {
            if (d->since_edge_prev > 2.0f) d->last_gap = d->since_edge_prev;
            d->since_edge_prev = 0.0f;
        }
        d->since_edge_prev += 1.0f;
        float gap = d->last_gap > 2.0f ? d->last_gap : d->period;
        float sq = (float)d->state;
        d->sq_lp += 0.6f * (sq - d->sq_lp); /* tira a quina mais dura (aliasing) */
        float o1_shaped;
        switch (p->osc1_wave) {
        case WAVE_SINE: /* a fundamental isolada pela banda, com volume normalizado */
            o1_shaped = clampf(y / fmaxf(d->env_y, 1e-5f), -1.0f, 1.0f);
            break;
        case WAVE_SAW: /* rampa que recomeça a cada subida da onda */
            o1_shaped = 2.0f * clampf(d->since_edge_prev / gap, 0.0f, 1.0f) - 1.0f;
            break;
        case WAVE_TRIANGLE: /* quadrada integrada */
            d->tri += sq * 4.0f / gap;
            d->tri = clampf(d->tri, -1.0f, 1.0f);
            o1_shaped = d->tri;
            break;
        default:
            o1_shaped = d->sq_lp;
            break;
        }

        /* ---- osciladores medidos (OSC2 e oitavas): presos ao período ---- */
        float f1  = sr / d->period * oct_mul * exp2f(dst[DST_PITCH] / 12.0f);
        float dt1 = fminf(f1 / sr, 0.5f), dt2 = fminf(f1 * detune_mul / sr, 0.5f);
        if (edge) {
            /* a cada ciclo da corda, puxa a fase para o começo (com oitavas
               abaixo, a cada 2 ou 4 ciclos); só metade do erro: sem estalos */
            int every = oct >= 0 ? 1 : (1 << -oct);
            if (d->edges % every == 0) {
                float err = -d->ph1;
                if (err < -0.5f) err += 1.0f;
                d->ph1 += 0.5f * err;
                if (d->ph1 < 0.0f) d->ph1 += 1.0f;
            }
        }
        float o1_synced = synth_oscillator(p->osc1_wave, d->ph1, dt1);
        float o2 = synth_oscillator(p->osc2_wave, d->ph2, dt2);
        d->ph1 += dt1; if (d->ph1 >= 1.0f) d->ph1 -= 1.0f;
        d->ph2 += dt2; if (d->ph2 >= 1.0f) d->ph2 -= 1.0f;
        /* liberados só com a nota confirmada (banda fechada em volta dela) */
        float want_sync = d->center > 0.0f ? 1.0f : 0.0f;
        d->synced += (want_sync - d->synced) * sync_coef;
        float o1 = oct == 0 ? o1_shaped : o1_synced * d->synced;
        float mix = clampf(p->osc_mix + dst[DST_OSC_MIX], 0.0f, 1.0f);
        float s = (1.0f - mix) * o1 + mix * o2 * d->synced;

        /* ---- filtro (o mesmo do synth) ---- */
        float fc = clampf(p->cutoff * exp2f(dst[DST_CUTOFF]), 20.0f, sr * 0.45f);
        float g  = tanf(3.14159265f * fc / sr);
        float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
        float v3 = s - d->ic2;
        float v1 = a1 * d->ic1 + a2 * v3;
        float v2 = d->ic2 + a2 * d->ic1 + a3 * v3;
        d->ic1 = 2.0f * v1 - d->ic1;
        d->ic2 = 2.0f * v2 - d->ic2;

        /* volume: envelope × dinâmica da corda (comprimida: a corda cai rápido) */
        float dyn = sqrtf(clampf(d->level * 3.0f, 0.0f, 2.0f));
        float gain_out = amp_env * dyn * (0.35f + 0.65f * d->velocity) * clampf(1.0f + dst[DST_AMP], 0.0f, 2.0f);
        out[i] += tanhf(v2 * gain_out * p->volume);
    }
}
