#include "synth.h"
#include "params.h"

#include <math.h>
#include <string.h>

#define PI_F     3.14159265358979f
#define TWO_PI_F 6.28318530717959f

static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static float midi_to_hz(float n) { return 440.0f * exp2f((n - 69.0f) / 12.0f); }

const char *waveform_name(Waveform w)
{
    static const char *names[WAVE_COUNT] = {"seno", "serra", "quadrada", "triangulo"};
    return (w >= 0 && w < WAVE_COUNT) ? names[w] : "?";
}

void synth_init(Synth *s, float sample_rate)
{
    memset(s, 0, sizeof *s);
    s->sr = sample_rate;
    synth_load_params(s);
}

void synth_load_params(Synth *s)
{
    Patch *p = &s->patch;
    p->osc1_wave   = (Waveform)param_geti(P_OSC1_WAVE);
    p->osc2_wave   = (Waveform)param_geti(P_OSC2_WAVE);
    p->osc2_detune = param_s(P_DETUNE);
    p->osc_mix     = param_s(P_OSC_MIX);
    p->cutoff      = param_s(P_CUTOFF);
    p->resonance   = param_s(P_RESONANCE);
    p->amp_env     = (AdsrParams){param_get(P_ATTACK), param_get(P_DECAY), param_get(P_SUSTAIN), param_get(P_RELEASE)};
    p->filt_env    = (AdsrParams){param_get(P_FATTACK), param_get(P_FDECAY), param_get(P_FSUSTAIN), param_get(P_FRELEASE)};
    p->lfo_rate    = param_s(P_LFO_RATE);
    p->glide       = param_s(P_GLIDE);
    p->volume      = param_s(P_SYNTH_LEVEL);
    p->mod_wheel   = param_s(P_MODWHEEL);
    for (int i = 0; i < MAX_MOD_SLOTS; i++) {
        if (i < MOD_SLOTS) {
            p->mods[i].source = (ModSource)param_geti(P_MOD_SRC(i));
            p->mods[i].via    = (ModSource)param_geti(P_MOD_VIA(i));
            p->mods[i].dest   = (ModDest)param_geti(P_MOD_DST(i));
            p->mods[i].amount = param_s(P_MOD_AMT(i));
        } else {
            p->mods[i] = (ModSlot){MOD_NONE, MOD_NONE, DST_PITCH, 0};
        }
    }
}

/* ------------------------------------------------------------------ */
/* Envelope ADSR                                                       */
/* ------------------------------------------------------------------ */
static float adsr_next(Adsr *e, const AdsrParams *p, float sr)
{
    switch (e->stage) {
    case ENV_ATTACK:
        e->level += 1.0f / (fmaxf(p->attack, 0.0005f) * sr);
        if (e->level >= 1.0f) { e->level = 1.0f; e->stage = ENV_DECAY; }
        break;
    case ENV_DECAY:
        e->level -= (1.0f - p->sustain) / (fmaxf(p->decay, 0.0005f) * sr);
        if (e->level <= p->sustain) { e->level = p->sustain; e->stage = ENV_SUSTAIN; }
        break;
    case ENV_SUSTAIN:
        e->level = p->sustain;
        break;
    case ENV_RELEASE:
        e->level -= 1.0f / (fmaxf(p->release, 0.0005f) * sr);
        if (e->level <= 0.0f) { e->level = 0.0f; e->stage = ENV_IDLE; }
        break;
    case ENV_IDLE:
        break;
    }
    return e->level;
}

/* ------------------------------------------------------------------ */
/* Osciladores com PolyBLEP (reduz o aliasing de serra e quadrada)     */
/* ------------------------------------------------------------------ */
static float poly_blep(float t, float dt)
{
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

static float oscillator(Waveform w, float t, float dt)
{
    switch (w) {
    case WAVE_SINE:
        return sinf(TWO_PI_F * t);
    case WAVE_SAW:
        return 2.0f * t - 1.0f - poly_blep(t, dt);
    case WAVE_SQUARE: {
        float v  = t < 0.5f ? 1.0f : -1.0f;
        float t2 = t + 0.5f;
        if (t2 >= 1.0f) t2 -= 1.0f;
        return v + poly_blep(t, dt) - poly_blep(t2, dt);
    }
    case WAVE_TRIANGLE:
        return 4.0f * fabsf(t - 0.5f) - 1.0f;
    default:
        return 0.0f;
    }
}

/* ------------------------------------------------------------------ */
/* Gerência de vozes                                                   */
/* ------------------------------------------------------------------ */
static Voice *alloc_voice(Synth *s)
{
    Voice *oldest = NULL, *oldest_rel = NULL;
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &s->voices[i];
        if (!v->active) return v;
        if (v->amp_env.stage == ENV_RELEASE && (!oldest_rel || v->age < oldest_rel->age)) oldest_rel = v;
        if (!oldest || v->age < oldest->age) oldest = v;
    }
    return oldest_rel ? oldest_rel : oldest; /* "rouba" uma voz */
}

static Voice *find_held_audio_voice(Synth *s)
{
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &s->voices[i];
        if (v->active && v->source == SRC_AUDIO && v->amp_env.stage != ENV_RELEASE) return v;
    }
    return NULL;
}

static void voice_start(Synth *s, Voice *v, int note, float exact_note, float vel, int source)
{
    int was_active = v->active;
    v->active      = 1;
    v->note        = note;
    v->source      = source;
    v->velocity    = vel;
    v->target_freq = midi_to_hz(exact_note);
    v->freq        = v->target_freq;
    v->age         = ++s->counter;
    v->sustained   = 0;
    if (!was_active) {
        v->phase1 = v->phase2 = 0.0f;
        v->ic1eq = v->ic2eq = 0.0f;
        v->amp_env.level = v->filt_env.level = 0.0f;
    }
    /* Reaproveita o nível atual do envelope: evita estalo ao redisparar. */
    v->amp_env.stage  = ENV_ATTACK;
    v->filt_env.stage = ENV_ATTACK;
}

static void note_on(Synth *s, const Event *e)
{
    float vel   = e->velocity / 127.0f;
    float exact = (e->source == SRC_AUDIO || e->source == SRC_HARM) ? e->value : (float)e->note;

    if (e->source == SRC_AUDIO) {
        /* Guitarra é monofônica: se já há nota soando, desliza (legato). */
        Voice *v = find_held_audio_voice(s);
        if (v) {
            v->note        = e->note;
            v->target_freq = midi_to_hz(exact);
            return;
        }
    } else {
        /* Mesma tecla de novo: reutiliza a voz. */
        for (int i = 0; i < MAX_VOICES; i++) {
            Voice *v = &s->voices[i];
            if (v->active && v->note == e->note && v->source == e->source) {
                voice_start(s, v, e->note, exact, vel, e->source);
                return;
            }
        }
    }
    voice_start(s, alloc_voice(s), e->note, exact, vel, e->source);
}

static void note_off(Synth *s, const Event *e)
{
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &s->voices[i];
        if (!v->active || v->source != e->source) continue;
        if (e->source == SRC_AUDIO || v->note == e->note) {
            if (s->sustain && e->source != SRC_AUDIO) { v->sustained = 1; continue; }
            if (v->amp_env.stage != ENV_IDLE) v->amp_env.stage = ENV_RELEASE;
            if (v->filt_env.stage != ENV_IDLE) v->filt_env.stage = ENV_RELEASE;
        }
    }
}

static void release_sustained(Synth *s)
{
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &s->voices[i];
        if (v->active && v->sustained) {
            v->sustained      = 0;
            v->amp_env.stage  = ENV_RELEASE;
            v->filt_env.stage = ENV_RELEASE;
        }
    }
}

static void all_off(Synth *s)
{
    s->sustain = 0;
    for (int i = 0; i < MAX_VOICES; i++) {
        s->voices[i].sustained = 0;
        s->voices[i].amp_env.stage  = ENV_RELEASE;
        s->voices[i].filt_env.stage = ENV_RELEASE;
    }
}

void synth_handle_event(Synth *s, const Event *e)
{
    switch (e->type) {
    case EV_NOTE_ON:
        if (e->velocity == 0) note_off(s, e); else note_on(s, e);
        break;
    case EV_NOTE_OFF:
        note_off(s, e);
        break;
    case EV_PITCH_TRACK: {
        if (e->source == SRC_HARM) { /* voz de harmonia acompanha o bend da guitarra */
            for (int i = 0; i < MAX_VOICES; i++) {
                Voice *v = &s->voices[i];
                if (v->active && v->source == SRC_HARM && v->note == e->note && v->amp_env.stage != ENV_RELEASE)
                    v->target_freq = midi_to_hz(e->value);
            }
            break;
        }
        Voice *v = find_held_audio_voice(s);
        if (v) v->target_freq = midi_to_hz(e->value);
        break;
    }
    case EV_PITCH_BEND:
        s->pitch_bend = e->value;
        break;
    case EV_CC:
        /* Os demais controladores viram parâmetros no módulo MIDI. */
        if (e->note == 64) {                    /* pedal de sustain */
            s->sustain = e->velocity >= 64;
            if (!s->sustain) release_sustained(s);
        } else if (e->note == 120 || e->note == 123) {
            all_off(s);
        }
        break;
    case EV_ALL_OFF:
        all_off(s);
        break;
    }
}

int synth_active_voices(const Synth *s)
{
    int n = 0;
    for (int i = 0; i < MAX_VOICES; i++) n += s->voices[i].active;
    return n;
}

/* ------------------------------------------------------------------ */
/* Renderização                                                        */
/* ------------------------------------------------------------------ */
void synth_render(Synth *s, float *out, int frames)
{
    const Patch *p  = &s->patch;
    const float  sr = s->sr;
    const float glide_coef = p->glide > 0.0f ? 1.0f - expf(-1.0f / (p->glide * sr)) : 1.0f;
    const float level_coef = 1.0f - expf(-1.0f / (0.015f * sr));
    const float detune_mul = exp2f(p->osc2_detune / 12.0f);
    const float k = 2.0f - 1.95f * p->resonance; /* amortecimento do filtro */

    for (int i = 0; i < frames; i++) {
        s->input_level_smooth += (s->input_level - s->input_level_smooth) * level_coef;

        float lfo = sinf(TWO_PI_F * s->lfo_phase);
        s->lfo_phase += p->lfo_rate / sr;
        if (s->lfo_phase >= 1.0f) s->lfo_phase -= 1.0f;

        float sum = 0.0f;
        for (int vi = 0; vi < MAX_VOICES; vi++) {
            Voice *v = &s->voices[vi];
            if (!v->active) continue;

            float amp  = adsr_next(&v->amp_env, &p->amp_env, sr);
            float env2 = adsr_next(&v->filt_env, &p->filt_env, sr);
            if (v->amp_env.stage == ENV_IDLE) { v->active = 0; continue; }

            /* Matriz de modulação */
            float src[MOD_SRC_COUNT] = {1.0f, lfo, env2, v->velocity, p->mod_wheel, s->input_level_smooth};
            float dst[DST_COUNT]     = {0};
            for (int m = 0; m < MAX_MOD_SLOTS; m++) {
                const ModSlot *slot = &p->mods[m];
                if (slot->source == MOD_NONE || slot->amount == 0.0f) continue;
                dst[slot->dest] += src[slot->source] * src[slot->via] * slot->amount;
            }

            /* Osciladores */
            v->freq += (v->target_freq - v->freq) * ((v->source == SRC_AUDIO || v->source == SRC_HARM) ? glide_coef : 1.0f);
            float f1  = v->freq * exp2f((s->pitch_bend + dst[DST_PITCH]) / 12.0f);
            float dt1 = fminf(f1 / sr, 0.5f);
            float dt2 = fminf(f1 * detune_mul / sr, 0.5f);
            float o1  = oscillator(p->osc1_wave, v->phase1, dt1);
            float o2  = oscillator(p->osc2_wave, v->phase2, dt2);
            v->phase1 += dt1; if (v->phase1 >= 1.0f) v->phase1 -= 1.0f;
            v->phase2 += dt2; if (v->phase2 >= 1.0f) v->phase2 -= 1.0f;
            float mix = clampf(p->osc_mix + dst[DST_OSC_MIX], 0.0f, 1.0f);
            float x   = (1.0f - mix) * o1 + mix * o2;

            /* Filtro de variáveis de estado (topologia de Simper/Cytomic) */
            float fc = clampf(p->cutoff * exp2f(dst[DST_CUTOFF]), 20.0f, sr * 0.45f);
            float g  = tanf(PI_F * fc / sr);
            float a1 = 1.0f / (1.0f + g * (g + k));
            float a2 = g * a1, a3 = g * a2;
            float v3 = x - v->ic2eq;
            float v1 = a1 * v->ic1eq + a2 * v3;
            float v2 = v->ic2eq + a2 * v->ic1eq + a3 * v3;
            v->ic1eq = 2.0f * v1 - v->ic1eq;
            v->ic2eq = 2.0f * v2 - v->ic2eq;

            /* VCA */
            float gain = amp * (0.35f + 0.65f * v->velocity) * clampf(1.0f + dst[DST_AMP], 0.0f, 2.0f);
            sum += v2 * gain;
        }
        out[i] = tanhf(sum * p->volume); /* saturação suave: nunca passa de ±1 */
    }
}
