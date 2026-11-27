/*
 * Motor de síntese subtrativa com matriz de modulação.
 * Os valores vêm da tabela de parâmetros (params.h) a cada bloco.
 *
 *   OSC1 ─┐
 *         ├─ mix ─> FILTRO (SVF passa-baixa) ─> VCA ─> saída
 *   OSC2 ─┘           ▲                          ▲
 *                     │                          │
 *        ENV2, LFO, velocidade, nível da guitarra... (matriz de modulação)
 *
 * A parte "modular" é a matriz: cada ModSlot liga uma FONTE a um DESTINO
 * com uma intensidade, opcionalmente escalada por uma segunda fonte (via).
 * Ex.: LFO -> PITCH, intensidade 0,5 semitom, via MODWHEEL = vibrato
 * controlado pela roda de modulação.
 */
#ifndef SYNTH_H
#define SYNTH_H

#include "config.h"
#include "queue.h"

typedef enum { WAVE_SINE, WAVE_SAW, WAVE_SQUARE, WAVE_TRIANGLE, WAVE_COUNT } Waveform;

typedef enum {
    MOD_NONE,         /* slot vazio / "via" sem escala */
    MOD_LFO,          /* -1..1 */
    MOD_ENV2,         /* 0..1  envelope do filtro */
    MOD_VELOCITY,     /* 0..1 */
    MOD_MODWHEEL,     /* 0..1  CC1 do MIDI ou teclas U/I */
    MOD_INPUT_LEVEL,  /* 0..1  intensidade com que a guitarra está sendo tocada */
    MOD_SRC_COUNT
} ModSource;

typedef enum {
    DST_PITCH,   /* semitons */
    DST_CUTOFF,  /* oitavas */
    DST_AMP,     /* ganho adicional (-1..1) */
    DST_OSC_MIX, /* 0..1 */
    DST_COUNT
} ModDest;

typedef struct {
    ModSource source;
    ModSource via;
    ModDest   dest;
    float     amount;
} ModSlot;

typedef struct { float attack, decay, sustain, release; } AdsrParams;

typedef enum { ENV_IDLE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE } EnvStage;
typedef struct { EnvStage stage; float level; } Adsr;

typedef struct {
    Waveform   osc1_wave, osc2_wave;
    float      osc2_detune;   /* semitons */
    float      osc_mix;       /* 0 = só OSC1, 1 = só OSC2 */
    float      cutoff;        /* Hz */
    float      resonance;     /* 0..0,98 */
    AdsrParams amp_env, filt_env;
    float      lfo_rate;      /* Hz */
    float      glide;         /* s – portamento das notas vindas da guitarra */
    float      volume;        /* 0..1 */
    float      mod_wheel;     /* 0..1 */
    ModSlot    mods[MAX_MOD_SLOTS];
} Patch;

typedef struct {
    int      active;
    int      note;
    int      source;
    float    velocity;
    float    freq, target_freq;
    float    phase1, phase2;
    float    ic1eq, ic2eq;    /* estado do filtro */
    Adsr     amp_env, filt_env;
    unsigned age;
    int      sustained;       /* soltou a tecla com o pedal pressionado */
    int      pending;         /* guitarra: começou na palhetada, nota ainda não confirmada */
    float    damp;            /* 1 = abafada (nota provisória) ... 0 = normal */
    float    kill;            /* > 0: sumindo rápido (palhetada que não virou nota) */
    unsigned t_on;            /* amostras desde que a voz começou */
    int      fast_glide;      /* guitarra: correção de meio tom logo no começo, desliza rápido */
} Voice;

typedef struct {
    float    sr;
    Patch    patch;
    Voice    voices[MAX_VOICES];
    float    lfo_phase;
    float    pitch_bend;      /* semitons */
    int      sustain;         /* pedal de sustain (CC64 ou barra de espaço) */
    float    input_level;     /* nível da guitarra 0..1 (do detector) */
    float    input_level_smooth;
    unsigned counter;
} Synth;

void        synth_init(Synth *s, float sample_rate);
void        synth_load_params(Synth *s); /* copia a tabela de parâmetros para o patch */
void        synth_handle_event(Synth *s, const Event *e);
void        synth_render(Synth *s, float *out, int frames); /* mono */
int         synth_active_voices(const Synth *s);
/* Ataque imediato da guitarra: começa a nota abafada, numa altura
   provisória, assim que a palhetada é detectada. O NOTE_ON seguinte
   (SRC_AUDIO) acerta a altura na hora e tira o abafamento. */
void        synth_onset(Synth *s, int velocity, float guess_note);
void        synth_onset_cancel(Synth *s); /* palhetada sem nota: solta a provisória */
const char *waveform_name(Waveform w);
float       synth_adsr_next(Adsr *e, const AdsrParams *p, float sr);
float       synth_oscillator(Waveform w, float phase, float dt);

#endif
