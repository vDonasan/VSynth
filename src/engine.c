#include "engine.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniaudio.h"
#include "params.h"

/* ------------------------------------------------------------------ */
/* Denormais: quando a cauda de um reverb/delay vai sumindo, os números */
/* ficam tão pequenos que alguns processadores passam a calculá-los    */
/* dezenas de vezes mais devagar -> picos de CPU e estalos. FTZ/DAZ    */
/* manda o processador tratá-los como zero.                            */
/* ------------------------------------------------------------------ */
#if defined(__x86_64__) || defined(_M_X64) || defined(__SSE__)
#include <xmmintrin.h>
void engine_disable_denormals(void) { _mm_setcsr(_mm_getcsr() | 0x8040); } /* FTZ | DAZ */
#elif defined(__aarch64__)
#include <stdint.h>
void engine_disable_denormals(void)
{
    uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ULL << 24); /* FZ */
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
}
#else
void engine_disable_denormals(void) {}
#endif

struct EngineAudio {
    ma_context   ctx;
    int          ctx_ok;
    ma_device    dev;
    ma_device_id play_id[MAX_AUDIO_DEVS], cap_id[MAX_AUDIO_DEVS];
};

#define TWO_PI_F 6.28318530717959f

int engine_init(Engine *e, float sr)
{
    memset(e, 0, sizeof *e);
    e->sr = sr;
    evq_init(&e->q_ui);
    evq_init(&e->q_midi);
    evq_init(&e->q_gtr);
    synth_init(&e->synth, sr);
    arp_init(&e->arp);
    e->gtr_arp_note = -1;
    evq_init(&e->q_mout);
    e->mout_audio_note = -1;
    for (int i = 0; i < 2 * 128 * 2; i++) (&e->harm_map[0][0][0])[i] = -1;
    e->harm_audio[0] = e->harm_audio[1] = -1;
    if (fx_init(&e->fx, sr) < 0) return -1;
    if (looper_init(&e->looper, sr, 60.0f) < 0) return -1;
    if (guitar_init(&e->guitar, sr, &e->q_gtr) < 0) return -1;
    if (fring_init(&e->rec_ring, REC_RING_SIZE) < 0) return -1;
    e->audio = calloc(1, sizeof *e->audio);
    if (!e->audio) return -1;
    atomic_init(&e->scope_pos, 0);
    atomic_init(&e->peak, 0.0f);
    atomic_init(&e->beat, 0);
    atomic_init(&e->rec_on, false);
    for (int i = 0; i < 128; i++) atomic_init(&e->notes_on[i], 0);
    for (int i = 0; i < SCOPE_SIZE; i++) atomic_init(&e->scope[i], 0.0f);
    return 0;
}

void engine_free(Engine *e)
{
    engine_close(e);
    if (e->audio && e->audio->ctx_ok) ma_context_uninit(&e->audio->ctx);
    free(e->audio);
    e->audio = NULL;
    guitar_free(&e->guitar);
    looper_free(&e->looper);
    fx_free(&e->fx);
    fring_free(&e->rec_ring);
}

void engine_ui_event(Engine *e, EventType type, int note, int vel)
{
    Event ev = {(uint8_t)type, SRC_KEYS, (uint8_t)note, (uint8_t)vel, (float)note};
    evq_push(&e->q_ui, ev);
}

/* ------------------------------------------------------------------ */
/* Caminho das notas                                                   */
/*                                                                     */
/*  entrada -> [oitava da guitarra] -> [harmonizador] -> [arpejador]   */
/*          -> to_synth(): synth + saída MIDI                          */
/* ------------------------------------------------------------------ */

/* Saída MIDI: copia para a fila que a thread do MIDI envia */
/* "Guitarra": a nota detectada sai direto da entrada (route), antes de
   harmonizador e arpejador. "Tudo": sai o que chega ao synth (to_synth). */
static void midi_out_emit(Engine *e, const Event *ev)
{
    switch (ev->type) {
    case EV_NOTE_ON:
        if (ev->velocity == 0) break;
        if (ev->source == SRC_AUDIO) {
            /* o synth faz legato, mas no MIDI cada nota precisa de note-off */
            if (e->mout_audio_note >= 0) {
                Event off = {EV_NOTE_OFF, SRC_AUDIO, (uint8_t)e->mout_audio_note, 0, 0};
                evq_push(&e->q_mout, off);
            }
            e->mout_audio_note = ev->note;
        }
        evq_push(&e->q_mout, *ev);
        return;
    case EV_NOTE_OFF:
        if (ev->source == SRC_AUDIO) {
            if (e->mout_audio_note < 0) return;
            Event off = {EV_NOTE_OFF, SRC_AUDIO, (uint8_t)e->mout_audio_note, 0, 0};
            evq_push(&e->q_mout, off);
            e->mout_audio_note = -1;
            return;
        }
        break;
    case EV_PITCH_TRACK:
        if (ev->source != SRC_AUDIO) return; /* bend MIDI vale pro canal todo */
        break;
    case EV_CC:
        if (ev->note != 64 && ev->note != 123 && ev->note != 120) return;
        break;
    case EV_ALL_OFF:
        e->mout_audio_note = -1;
        break;
    default:
        break;
    }
    evq_push(&e->q_mout, *ev);
}

static void to_synth(Engine *e, const Event *ev)
{
    synth_handle_event(&e->synth, ev);
    if (param_geti(P_MOUT_SRC) == 2) midi_out_emit(e, ev);
}

static void sink_to_synth(void *user, const Event *ev) { to_synth((Engine *)user, ev); }

/* Arpejador (se ligado) ou direto para o synth */
static void deliver(Engine *e, Event ev, int arp_on)
{
    if (arp_on && (ev.type == EV_NOTE_ON || ev.type == EV_NOTE_OFF)) {
        if (ev.source == SRC_AUDIO) {
            /* guitarra faz legato sem note-off: solta a anterior no arpejador */
            if (e->gtr_arp_note >= 0) arp_note_off(&e->arp, e->gtr_arp_note);
            e->gtr_arp_note = -1;
            if (ev.type == EV_NOTE_ON) { arp_note_on(&e->arp, ev.note, ev.velocity); e->gtr_arp_note = ev.note; }
        } else if (ev.type == EV_NOTE_ON && ev.velocity > 0) {
            arp_note_on(&e->arp, ev.note, ev.velocity);
        } else {
            arp_note_off(&e->arp, ev.note);
        }
        return;
    }
    if (arp_on && ev.type == EV_PITCH_TRACK) return;
    if (ev.type == EV_ALL_OFF) { arp_clear(&e->arp, sink_to_synth, e); e->gtr_arp_note = -1; }
    to_synth(e, &ev);
}

/* ---------- harmonizador ---------- */
static const int SCALES[2][7] = {{0, 2, 4, 5, 7, 9, 11},  /* maior */
                                 {0, 2, 3, 5, 7, 8, 10}}; /* menor natural */

/* Sobe/desce `steps` graus na escala, mantendo notas fora dela paralelas */
static int diatonic(int note, int key, const int *sc, int steps)
{
    int rel  = ((note - key) % 12 + 12) % 12;
    int base = note - rel;
    int d = 6;
    while (d > 0 && sc[d] > rel) d--;
    int chroma = rel - sc[d];
    int t   = d + steps;
    int oct = t >= 0 ? t / 7 : -((-t + 6) / 7);
    return base + 12 * oct + sc[t - 7 * oct] + chroma;
}

int engine_harmony_notes(int note, int out[2])
{
    static const int steps[8][2] = {{2, 0}, {3, 0}, {4, 0}, {5, 0}, {7, 0}, {-2, 0}, {-7, 0}, {2, 4}};
    static const int semis[8][2] = {{4, 0}, {5, 0}, {7, 0}, {9, 0}, {12, 0}, {-3, 0}, {-12, 0}, {4, 7}};
    int iv = param_geti(P_HARM_INTERVAL), sc = param_geti(P_HARM_SCALE), key = param_geti(P_HARM_KEY);
    int count = iv == 7 ? 2 : 1, n = 0;
    for (int i = 0; i < count; i++) {
        int h = sc == 2 ? note + semis[iv][i] : diatonic(note, key, SCALES[sc], steps[iv][i]);
        if (h >= 0 && h <= 127 && h != note) out[n++] = h;
    }
    return n;
}

static void harm_release(Engine *e, int *slot, int arp_on)
{
    for (int i = 0; i < 2; i++) {
        if (slot[i] < 0) continue;
        Event off = {EV_NOTE_OFF, SRC_HARM, (uint8_t)slot[i], 0, 0};
        deliver(e, off, arp_on);
        slot[i] = -1;
    }
}

static int *harm_slot(Engine *e, const Event *ev)
{
    if (ev->source == SRC_AUDIO) return e->harm_audio;
    if (ev->source == SRC_MIDI || ev->source == SRC_KEYS) return e->harm_map[ev->source][ev->note];
    return NULL;
}

static void route(Engine *e, Event ev, int arp_on)
{
    if (ev.source == SRC_AUDIO) {
        int shift = 12 * param_geti(P_GTR_OCTAVE);
        int n = ev.note + shift;
        if (n < 0 || n > 127) return;
        ev.note = (uint8_t)n;
        ev.value += (float)shift;
        if (ev.type == EV_PITCH_TRACK && !param_geti(P_GTR_BENDS)) return;
        if (!param_geti(P_GTR_BENDS) && ev.type == EV_NOTE_ON) ev.value = (float)ev.note;
        if (param_geti(P_MOUT_SRC) == 1) midi_out_emit(e, &ev);
    }

    int *slot = harm_slot(e, &ev);
    int is_on = ev.type == EV_NOTE_ON && ev.velocity > 0;
    int is_off = ev.type == EV_NOTE_OFF || (ev.type == EV_NOTE_ON && ev.velocity == 0);

    if (ev.type == EV_ALL_OFF) {
        for (int i = 0; i < 2 * 128 * 2; i++) (&e->harm_map[0][0][0])[i] = -1;
        e->harm_audio[0] = e->harm_audio[1] = -1;
        deliver(e, ev, arp_on);
        return;
    }
    if (is_on) {
        if (slot) harm_release(e, slot, arp_on); /* legato da guitarra / mesma tecla de novo */
        deliver(e, ev, arp_on);
        int want = param_geti(P_HARM_ON) && slot && (param_geti(P_HARM_SRC) == 1 || ev.source == SRC_AUDIO);
        if (want) {
            int hs[2], n = engine_harmony_notes(ev.note, hs);
            int vel = (int)(ev.velocity * param_get(P_HARM_LEVEL));
            float bend = ev.value - (float)ev.note; /* guitarra: fração de semitom */
            for (int i = 0; i < n; i++) {
                Event h = {EV_NOTE_ON, SRC_HARM, (uint8_t)hs[i], (uint8_t)(vel < 1 ? 1 : vel), hs[i] + bend};
                deliver(e, h, arp_on);
                slot[i] = hs[i];
            }
        }
        return;
    }
    if (is_off) {
        deliver(e, ev, arp_on);
        if (slot) harm_release(e, slot, arp_on);
        return;
    }
    if (ev.type == EV_PITCH_TRACK && slot) {
        deliver(e, ev, arp_on);
        float bend = ev.value - (float)ev.note;
        for (int i = 0; i < 2; i++) {
            if (slot[i] < 0) continue;
            Event h = {EV_PITCH_TRACK, SRC_HARM, (uint8_t)slot[i], 0, slot[i] + bend};
            deliver(e, h, arp_on);
        }
        return;
    }
    deliver(e, ev, arp_on);
}

static void metronome(Engine *e, float *l, float *r, int n, float bpm, float master)
{
    double spb = 60.0 / bpm * e->sr;
    int on = param_geti(P_METRO_ON);
    float vol = param_s(P_METRO_VOL) * 0.5f;
    float decay = expf(-1.0f / (0.025f * e->sr));
    for (int i = 0; i < n; i++) {
        if (e->metro_counter <= 0) {
            e->metro_counter += spb;
            e->metro_beat = (e->metro_beat + 1) % 4;
            atomic_store_explicit(&e->beat, e->metro_beat, memory_order_relaxed);
            if (on) { e->click_env = 1; e->click_phase = 0; e->click_freq = e->metro_beat == 0 ? 1760.0f : 1100.0f; }
        }
        e->metro_counter -= 1;
        if (e->click_env > 0.001f) {
            float c = sinf(TWO_PI_F * e->click_phase) * e->click_env * vol * master;
            e->click_phase += e->click_freq / e->sr;
            e->click_env *= decay;
            l[i] += c;
            r[i] += c;
        }
    }
}

void engine_process(Engine *e, const float *in, int in_channels, float *out, int frames)
{
    engine_disable_denormals(); /* vale para esta thread; é barato repetir */
    const int   mode   = param_geti(P_INPUT_MODE);
    const int   gtr    = mode == 2 && in != NULL;
    const int   arp_on = param_geti(P_ARP_ON);
    const float bpm    = param_get(P_BPM);
    const float smooth = 1.0f - expf(-(float)ENGINE_CHUNK / (0.03f * e->sr));
    const int   recording  = atomic_load_explicit(&e->rec_on, memory_order_relaxed);
    Event ev;

    /* 1. estado do arpejador */
    if (e->arp_was_on && !arp_on) { arp_clear(&e->arp, sink_to_synth, e); e->gtr_arp_note = -1; }
    if (!e->arp_was_on && arp_on) {
        Event off = {EV_ALL_OFF, SRC_KEYS, 0, 0, 0};
        to_synth(e, &off);
    }
    e->arp_was_on = arp_on;

    /* saída MIDI desligada ou trocada no meio de uma nota: solta tudo lá */
    int mout = param_geti(P_MOUT_SRC);
    if (mout != e->mout_prev) {
        Event off = {EV_ALL_OFF, SRC_KEYS, 0, 0, 0};
        evq_push(&e->q_mout, off);
        e->mout_audio_note = -1;
        e->mout_prev = mout;
    }

    /* 2. eventos que chegaram desde o último bloco */
    while (evq_pop(&e->q_ui, &ev))   route(e, ev, arp_on);
    while (evq_pop(&e->q_midi, &ev)) route(e, ev, arp_on);
    while (evq_pop(&e->q_gtr, &ev))  route(e, ev, arp_on);

    /* 3. entrada da guitarra: mono do canal escolhido -> detector */
    int ch = atomic_load_explicit(&e->cap_channel, memory_order_relaxed);
    if (ch >= in_channels) ch = 0;
    if (gtr) {
        int n = frames < 4096 ? frames : 4096;
        for (int i = 0; i < n; i++) e->gtr_in[i] = in[i * in_channels + ch];
        guitar_push(&e->guitar, e->gtr_in, n);
        e->synth.input_level = atomic_load(&e->guitar.level);
    } else {
        e->synth.input_level = 0;
    }

    /* 4. gera o som em pedaços pequenos (arpejador e metrônomo precisos) */
    float peak = atomic_load_explicit(&e->peak, memory_order_relaxed) * 0.9f;
    unsigned sp = atomic_load_explicit(&e->scope_pos, memory_order_relaxed);
    for (int done = 0; done < frames; done += ENGINE_CHUNK) {
        int n = frames - done < ENGINE_CHUNK ? frames - done : ENGINE_CHUNK;
        /* parâmetros suavizados a cada pedaço de 32 amostras: sem "zíper" */
        params_smooth_step(smooth);
        synth_load_params(&e->synth);
        const float master   = param_s(P_MASTER);
        const float blend    = param_s(P_GTR_BLEND);
        const float synth_gain = gtr ? fminf(1.0f, 2.0f * (1.0f - blend)) : 1.0f;
        const float gtr_gain   = gtr ? fminf(1.0f, 2.0f * blend) : 0.0f;
        const float loop_vol   = param_s(P_LOOP_VOL);
        if (arp_on) arp_process(&e->arp, sink_to_synth, e, n, e->sr, bpm);
        synth_render(&e->synth, e->mono, n);
        for (int i = 0; i < n; i++) {
            float g = gtr && done + i < 4096 ? e->gtr_in[done + i] * gtr_gain : 0.0f;
            e->l[i] = e->r[i] = e->mono[i] * synth_gain + g;
        }
        fx_process(&e->fx, e->l, e->r, n, bpm);
        looper_process(&e->looper, e->l, e->r, n, loop_vol);
        for (int i = 0; i < n; i++) {
            float l = e->l[i] * master, r = e->r[i] * master;
            if (recording) { fring_push(&e->rec_ring, l); fring_push(&e->rec_ring, r); }
            e->l[i] = l;
            e->r[i] = r;
        }
        metronome(e, e->l, e->r, n, bpm, master);
        for (int i = 0; i < n; i++) {
            float l = e->l[i], r = e->r[i];
            l = l > 1 ? 1 : (l < -1 ? -1 : l);
            r = r > 1 ? 1 : (r < -1 ? -1 : r);
            out[2 * (done + i)]     = l;
            out[2 * (done + i) + 1] = r;
            float m = 0.5f * (l + r);
            atomic_store_explicit(&e->scope[sp++ & (SCOPE_SIZE - 1)], m, memory_order_relaxed);
            if (fabsf(m) > peak) peak = fabsf(m);
        }
    }
    atomic_store_explicit(&e->scope_pos, sp, memory_order_relaxed);
    atomic_store_explicit(&e->peak, peak, memory_order_relaxed);

    /* 5. quais notas estão soando (para acender o piano) */
    unsigned char on[128] = {0};
    for (int i = 0; i < MAX_VOICES; i++) {
        const Voice *v = &e->synth.voices[i];
        if (v->active && v->amp_env.stage != ENV_RELEASE && v->note >= 0 && v->note < 128) on[v->note] = 1;
    }
    for (int i = 0; i < 128; i++) atomic_store_explicit(&e->notes_on[i], on[i], memory_order_relaxed);
}

/* ------------------------------------------------------------------ */
/* Dispositivos (miniaudio)                                            */
/* ------------------------------------------------------------------ */
static void data_callback(ma_device *d, void *out, const void *in, ma_uint32 frames)
{
    Engine *e = d->pUserData;
    engine_process(e, in, (int)d->capture.channels, out, (int)frames);
}

int engine_refresh_devices(Engine *e)
{
    EngineAudio *a = e->audio;
    if (!a->ctx_ok) {
        ma_backend null_only[1] = {ma_backend_null};
        if (ma_context_init(e->null_backend ? null_only : NULL, e->null_backend ? 1 : 0, NULL, &a->ctx) != MA_SUCCESS) return -1;
        a->ctx_ok = 1;
    }
    ma_device_info *pi, *ci;
    ma_uint32 np, nc;
    if (ma_context_get_devices(&a->ctx, &pi, &np, &ci, &nc) != MA_SUCCESS) return -1;
    e->nplay = (int)(np < MAX_AUDIO_DEVS ? np : MAX_AUDIO_DEVS);
    e->ncap  = (int)(nc < MAX_AUDIO_DEVS ? nc : MAX_AUDIO_DEVS);
    for (int i = 0; i < e->nplay; i++) {
        snprintf(e->play[i].name, sizeof e->play[i].name, "%s", pi[i].name);
        a->play_id[i] = pi[i].id;
        e->play[i].is_default = pi[i].isDefault;
    }
    for (int i = 0; i < e->ncap; i++) {
        snprintf(e->cap[i].name, sizeof e->cap[i].name, "%s", ci[i].name);
        a->cap_id[i] = ci[i].id;
        e->cap[i].is_default = ci[i].isDefault;
    }
    return 0;
}

int engine_open(Engine *e, int play, int cap, int cap_channel)
{
    EngineAudio *a = e->audio;
    engine_close(e);
    if (!a->ctx_ok && engine_refresh_devices(e) < 0) return -1;

    ma_device_config cfg = ma_device_config_init(cap == -2 ? ma_device_type_playback : ma_device_type_duplex);
    cfg.sampleRate         = (ma_uint32)e->sr;
    cfg.periodSizeInFrames = e->period_frames > 0 ? (ma_uint32)e->period_frames : FRAMES_PER_BUFFER;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    cfg.playback.format    = ma_format_f32;
    cfg.playback.channels  = 2;
    cfg.playback.pDeviceID = (play >= 0 && play < e->nplay) ? &a->play_id[play] : NULL;
    cfg.capture.format     = ma_format_f32;
    cfg.capture.channels   = 0; /* todos os canais do dispositivo */
    cfg.capture.pDeviceID  = (cap >= 0 && cap < e->ncap) ? &a->cap_id[cap] : NULL;
    cfg.dataCallback       = data_callback;
    cfg.pUserData          = e;

    e->exclusive_failed = 0;
    if (e->exclusive) {
        cfg.playback.shareMode = ma_share_mode_exclusive;
        cfg.capture.shareMode  = ma_share_mode_exclusive;
    }
    if (ma_device_init(&a->ctx, &cfg, &a->dev) != MA_SUCCESS) {
        if (!e->exclusive) return -1;
        /* o dispositivo não aceitou o modo exclusivo: tenta o compartilhado */
        cfg.playback.shareMode = ma_share_mode_shared;
        cfg.capture.shareMode  = ma_share_mode_shared;
        if (ma_device_init(&a->ctx, &cfg, &a->dev) != MA_SUCCESS) return -1;
        e->exclusive_failed = 1;
    }
    e->dev_ok         = 1;
    e->dev_rate       = a->dev.playback.internalSampleRate;
    e->dev_latency_ms = e->dev_rate ? 1000.0f * a->dev.playback.internalPeriodSizeInFrames / e->dev_rate : 0;
    atomic_store(&e->cap_channel, cap_channel);
    snprintf(e->dev_name, sizeof e->dev_name, "%s", a->dev.playback.name);
    snprintf(e->cap_name, sizeof e->cap_name, "%s", cap == -2 ? "" : a->dev.capture.name);
    if (cap != -2) guitar_start(&e->guitar);
    if (ma_device_start(&a->dev) != MA_SUCCESS) { engine_close(e); return -1; }
    return 0;
}

void engine_close(Engine *e)
{
    if (e->dev_ok) {
        ma_device_uninit(&e->audio->dev); /* para o callback antes de mexer no resto */
        e->dev_ok = 0;
    }
    guitar_stop(&e->guitar);
}

int engine_capture_channels(Engine *e)
{
    return (e->dev_ok && e->audio->dev.type == ma_device_type_duplex) ? (int)e->audio->dev.capture.channels : 0;
}
