#include "params.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAVES   "Seno;Serra;Quadrada;Triângulo"
#define MSRC    "Nenhuma;LFO;Env. filtro;Velocity;Roda mod.;Nível guitarra"
#define MDST    "Altura;Corte;Volume;Mistura OSC"

static const ParamDef DEFS[P_COUNT] = {
    [P_OSC1_WAVE]   = {"osc1_onda", "Onda OSC1", 0, 3, 1, K_CHOICE, "", WAVES, 0},
    [P_OSC2_WAVE]   = {"osc2_onda", "Onda OSC2", 0, 3, 2, K_CHOICE, "", WAVES, 0},
    [P_OSC_MIX]     = {"osc_mix", "Mistura OSC", 0, 1, 0.35f, K_LIN, "%", NULL, 0},
    [P_DETUNE]      = {"osc2_desafinacao", "Desafinação", -12, 12, 0.08f, K_LIN, "st", NULL, 0},
    [P_CUTOFF]      = {"corte", "Corte", 40, 18000, 900, K_LOG, "Hz", NULL, 0},
    [P_RESONANCE]   = {"ressonancia", "Ressonância", 0, 0.98f, 0.35f, K_LIN, "%", NULL, 0},
    [P_ATTACK]      = {"ataque", "Ataque", 0.001f, 3, 0.005f, K_LOG, "s", NULL, 0},
    [P_DECAY]       = {"decaimento", "Decaimento", 0.01f, 4, 0.3f, K_LOG, "s", NULL, 0},
    [P_SUSTAIN]     = {"sustentacao", "Sustentação", 0, 1, 0.7f, K_LIN, "%", NULL, 0},
    [P_RELEASE]     = {"release", "Release", 0.01f, 6, 0.35f, K_LOG, "s", NULL, 0},
    [P_FATTACK]     = {"filtro_ataque", "Ataque", 0.001f, 3, 0.005f, K_LOG, "s", NULL, 0},
    [P_FDECAY]      = {"filtro_decaimento", "Decaimento", 0.01f, 4, 0.45f, K_LOG, "s", NULL, 0},
    [P_FSUSTAIN]    = {"filtro_sustentacao", "Sustentação", 0, 1, 0.25f, K_LIN, "%", NULL, 0},
    [P_FRELEASE]    = {"filtro_release", "Release", 0.01f, 6, 0.4f, K_LOG, "s", NULL, 0},
    [P_LFO_RATE]    = {"lfo_velocidade", "LFO", 0.05f, 20, 5.5f, K_LOG, "Hz", NULL, 0},
    [P_MODWHEEL]    = {"roda_mod", "Roda de mod.", 0, 1, 0, K_LIN, "%", NULL, 0},
    [P_GLIDE]       = {"glide", "Glide", 0, 0.5f, 0.025f, K_LIN, "s", NULL, 0},
    [P_SYNTH_LEVEL] = {"nivel_synth", "Nível synth", 0, 1, 0.5f, K_LIN, "%", NULL, 0},

#define MOD(i, s, v, d, a)                                                              \
    [P_MOD##i##_SRC] = {"mod" #i "_fonte", "Fonte", 0, 5, s, K_CHOICE, "", MSRC, 0},    \
    [P_MOD##i##_VIA] = {"mod" #i "_via", "Via", 0, 5, v, K_CHOICE, "", MSRC, 0},        \
    [P_MOD##i##_DST] = {"mod" #i "_destino", "Destino", 0, 3, d, K_CHOICE, "", MDST, 0}, \
    [P_MOD##i##_AMT] = {"mod" #i "_quantidade", "Quantidade", -6, 6, a, K_LIN, "", NULL, 0}
    MOD(0, 2, 0, 1, 3.0f),  /* env. filtro -> corte */
    MOD(1, 3, 0, 1, 1.5f),  /* velocity -> corte */
    MOD(2, 1, 4, 0, 0.6f),  /* LFO (via roda) -> altura = vibrato */
    MOD(3, 5, 0, 1, 2.0f),  /* nível da guitarra -> corte */
    MOD(4, 0, 0, 0, 0.0f),
    MOD(5, 0, 0, 0, 0.0f),
#undef MOD

    [P_INPUT_MODE]  = {"entrada", "Entrada", 0, 2, 0, K_CHOICE, "", "Teclado;MIDI;Guitarra", F_NOPRESET | F_HIDDEN},
    [P_GTR_GATE]    = {"gtr_gate", "Gate", -70, -10, -42, K_LIN, "dB", NULL, F_NOPRESET},
    [P_GTR_GAIN]    = {"gtr_ganho", "Ganho", 0.25f, 16, 1, K_LOG, "x", NULL, F_NOPRESET},
    [P_GTR_BLEND]   = {"gtr_mistura", "Mistura", 0, 1, 0.25f, K_LIN, "mix", NULL, 0},
    [P_GTR_OCTAVE]  = {"gtr_oitava", "Oitava", -2, 2, 0, K_INT, "", NULL, 0},
    [P_GTR_BENDS]   = {"gtr_bends", "Seguir bends", 0, 1, 1, K_BOOL, "", NULL, 0},
    [P_KEYS_OCTAVE] = {"teclado_oitava", "Oitava", 1, 7, 4, K_INT, "", NULL, F_NOPRESET | F_HIDDEN},

    [P_DIST_ON]     = {"dist_ligada", "Distorção", 0, 1, 0, K_BOOL, "", NULL, 0},
    [P_DIST_DRIVE]  = {"dist_drive", "Drive", 0, 40, 18, K_LIN, "dB", NULL, 0},
    [P_DIST_TONE]   = {"dist_tom", "Tom", 400, 12000, 3500, K_LOG, "Hz", NULL, 0},
    [P_DIST_MIX]    = {"dist_mix", "Mix", 0, 1, 1, K_LIN, "%", NULL, 0},
    [P_CHO_ON]      = {"chorus_ligado", "Chorus", 0, 1, 0, K_BOOL, "", NULL, 0},
    [P_CHO_RATE]    = {"chorus_vel", "Velocidade", 0.05f, 5, 0.8f, K_LOG, "Hz", NULL, 0},
    [P_CHO_DEPTH]   = {"chorus_prof", "Profundidade", 0, 1, 0.5f, K_LIN, "%", NULL, 0},
    [P_CHO_MIX]     = {"chorus_mix", "Mix", 0, 1, 0.5f, K_LIN, "%", NULL, 0},
    [P_PHA_ON]      = {"phaser_ligado", "Phaser", 0, 1, 0, K_BOOL, "", NULL, 0},
    [P_PHA_RATE]    = {"phaser_vel", "Velocidade", 0.02f, 5, 0.4f, K_LOG, "Hz", NULL, 0},
    [P_PHA_DEPTH]   = {"phaser_prof", "Profundidade", 0, 1, 0.7f, K_LIN, "%", NULL, 0},
    [P_PHA_FB]      = {"phaser_realim", "Realimentação", 0, 0.9f, 0.5f, K_LIN, "%", NULL, 0},
    [P_PHA_MIX]     = {"phaser_mix", "Mix", 0, 1, 0.8f, K_LIN, "%", NULL, 0},
    [P_DLY_ON]      = {"delay_ligado", "Delay", 0, 1, 0, K_BOOL, "", NULL, 0},
    [P_DLY_SYNC]    = {"delay_sync", "Tempo", 0, 5, 2, K_CHOICE, "", "Livre;1/4;1/8;1/8 pont.;1/16;1/4 tercina", 0},
    [P_DLY_TIME]    = {"delay_ms", "Tempo livre", 20, 1500, 350, K_LOG, "ms", NULL, 0},
    [P_DLY_FB]      = {"delay_repeticoes", "Repetições", 0, 0.95f, 0.4f, K_LIN, "%", NULL, 0},
    [P_DLY_MIX]     = {"delay_mix", "Mix", 0, 1, 0.3f, K_LIN, "%", NULL, 0},
    [P_DLY_PING]    = {"delay_pingpong", "Ping-pong", 0, 1, 1, K_BOOL, "", NULL, 0},
    [P_REV_ON]      = {"reverb_ligado", "Reverb", 0, 1, 1, K_BOOL, "", NULL, 0},
    [P_REV_SIZE]    = {"reverb_sala", "Tamanho", 0, 1, 0.6f, K_LIN, "%", NULL, 0},
    [P_REV_DAMP]    = {"reverb_abafar", "Abafamento", 0, 1, 0.5f, K_LIN, "%", NULL, 0},
    [P_REV_MIX]     = {"reverb_mix", "Mix", 0, 1, 0.2f, K_LIN, "%", NULL, 0},
    [P_FX_ORDER0]   = {"fx_ordem0", "Ordem 1", 0, 4, 0, K_INT, "", NULL, F_HIDDEN},
    [P_FX_ORDER1]   = {"fx_ordem1", "Ordem 2", 0, 4, 1, K_INT, "", NULL, F_HIDDEN},
    [P_FX_ORDER2]   = {"fx_ordem2", "Ordem 3", 0, 4, 2, K_INT, "", NULL, F_HIDDEN},
    [P_FX_ORDER3]   = {"fx_ordem3", "Ordem 4", 0, 4, 3, K_INT, "", NULL, F_HIDDEN},
    [P_FX_ORDER4]   = {"fx_ordem4", "Ordem 5", 0, 4, 4, K_INT, "", NULL, F_HIDDEN},

    [P_BPM]         = {"bpm", "BPM", 40, 240, 110, K_INT, "", NULL, 0},
    [P_METRO_ON]    = {"metronomo", "Metrônomo", 0, 1, 0, K_BOOL, "", NULL, F_NOPRESET},
    [P_METRO_VOL]   = {"metronomo_vol", "Vol. metrônomo", 0, 1, 0.5f, K_LIN, "%", NULL, F_NOPRESET},
    [P_ARP_ON]      = {"arp_ligado", "Arpejador", 0, 1, 0, K_BOOL, "", NULL, 0},
    [P_ARP_MODE]    = {"arp_modo", "Modo", 0, 4, 0, K_CHOICE, "", "Sobe;Desce;Sobe e desce;Aleatório;Como tocado", 0},
    [P_ARP_RATE]    = {"arp_divisao", "Divisão", 0, 4, 3, K_CHOICE, "", "1/4;1/8;1/8 tercina;1/16;1/32", 0},
    [P_ARP_OCT]     = {"arp_oitavas", "Oitavas", 1, 4, 1, K_INT, "", NULL, 0},
    [P_ARP_GATE]    = {"arp_gate", "Duração", 0.05f, 1, 0.6f, K_LIN, "%", NULL, 0},
    [P_ARP_HOLD]    = {"arp_segurar", "Segurar", 0, 1, 0, K_BOOL, "", NULL, 0},
    [P_LOOP_VOL]    = {"looper_vol", "Vol. looper", 0, 1, 0.8f, K_LIN, "%", NULL, F_NOPRESET},
    [P_MASTER]      = {"volume_geral", "Volume", 0, 1, 0.8f, K_LIN, "%", NULL, F_NOPRESET},

    [P_HARM_ON]       = {"harm_ligado", "Harmonizador", 0, 1, 0, K_BOOL, "", NULL, 0},
    [P_HARM_INTERVAL] = {"harm_intervalo", "Intervalo", 0, 7, 0, K_CHOICE, "",
                         "Terça acima;Quarta acima;Quinta acima;Sexta acima;Oitava acima;Terça abaixo;Oitava abaixo;Acorde (terça+quinta)", 0},
    [P_HARM_KEY]      = {"harm_tom", "Tom", 0, 11, 0, K_CHOICE, "", "C;C#;D;D#;E;F;F#;G;G#;A;A#;B", 0},
    [P_HARM_SCALE]    = {"harm_escala", "Escala", 0, 2, 0, K_CHOICE, "", "Maior;Menor;Cromática (fixo)", 0},
    [P_HARM_LEVEL]    = {"harm_volume", "Volume", 0.05f, 1, 0.7f, K_LIN, "%", NULL, 0},
    [P_HARM_SRC]      = {"harm_fonte", "Aplicar em", 0, 1, 0, K_CHOICE, "", "Só guitarra;Tudo", 0},

    [P_MOUT_SRC]  = {"midi_saida", "Enviar", 0, 2, 0, K_CHOICE, "", "Nada;Guitarra;Tudo (com harmonia e arpejo)", F_NOPRESET | F_HIDDEN},
    [P_MOUT_CH]   = {"midi_saida_canal", "Canal", 1, 16, 1, K_INT, "", NULL, F_NOPRESET | F_HIDDEN},
    [P_MOUT_BEND] = {"midi_saida_bend", "Faixa do bend", 1, 24, 2, K_INT, "", NULL, F_NOPRESET | F_HIDDEN},
};

static _Atomic float g_values[P_COUNT];

const ParamDef *param_def(int id) { return (id >= 0 && id < P_COUNT) ? &DEFS[id] : NULL; }

void params_init(void)
{
    for (int i = 0; i < P_COUNT; i++) atomic_store(&g_values[i], DEFS[i].def);
}

void params_reset_sound(void)
{
    for (int i = 0; i < P_COUNT; i++)
        if (!(DEFS[i].flags & F_NOPRESET)) atomic_store(&g_values[i], DEFS[i].def);
}

float param_get(int id) { return atomic_load_explicit(&g_values[id], memory_order_relaxed); }
int   param_geti(int id) { return (int)lroundf(param_get(id)); }

void param_set(int id, float v)
{
    if (id < 0 || id >= P_COUNT || v != v) return;
    const ParamDef *d = &DEFS[id];
    if (v < d->min) v = d->min;
    if (v > d->max) v = d->max;
    if (d->kind == K_INT || d->kind == K_BOOL || d->kind == K_CHOICE) v = roundf(v);
    atomic_store_explicit(&g_values[id], v, memory_order_relaxed);
}

float param_get_norm(int id)
{
    const ParamDef *d = &DEFS[id];
    float v = param_get(id);
    if (d->kind == K_LOG) return logf(v / d->min) / logf(d->max / d->min);
    return (v - d->min) / (d->max - d->min);
}

void param_set_norm(int id, float n)
{
    const ParamDef *d = &DEFS[id];
    if (n < 0) n = 0;
    if (n > 1) n = 1;
    if (d->kind == K_LOG) param_set(id, d->min * powf(d->max / d->min, n));
    else param_set(id, d->min + n * (d->max - d->min));
}

int param_find(const char *key)
{
    for (int i = 0; i < P_COUNT; i++)
        if (!strcmp(DEFS[i].key, key)) return i;
    return -1;
}

/* Copia a n-ésima opção de "A;B;C" */
static void choice_text(const char *choices, int n, char *buf, int len)
{
    const char *p = choices;
    for (int i = 0; i < n && p; i++) {
        p = strchr(p, ';');
        if (p) p++;
    }
    if (!p) { snprintf(buf, len, "?"); return; }
    const char *end = strchr(p, ';');
    int l = end ? (int)(end - p) : (int)strlen(p);
    snprintf(buf, len, "%.*s", l, p);
}

void param_format(int id, char *buf, int len)
{
    const ParamDef *d = &DEFS[id];
    float v = param_get(id);
    switch (d->kind) {
    case K_CHOICE: choice_text(d->choices, (int)lroundf(v), buf, len); return;
    case K_BOOL:   snprintf(buf, len, v > 0.5f ? "ligado" : "desligado"); return;
    case K_INT:    snprintf(buf, len, d->min < 0 ? "%+d" : "%d", (int)lroundf(v)); return;
    default: break;
    }
    if (!strcmp(d->unit, "%"))        snprintf(buf, len, "%.0f%%", v * 100.0f);
    else if (!strcmp(d->unit, "Hz"))  snprintf(buf, len, v >= 1000 ? "%.1f kHz" : (v < 10 ? "%.2f Hz" : "%.0f Hz"), v >= 1000 ? v / 1000 : v);
    else if (!strcmp(d->unit, "s"))   snprintf(buf, len, v < 1 ? "%.0f ms" : "%.2f s", v < 1 ? v * 1000 : v);
    else if (!strcmp(d->unit, "ms"))  snprintf(buf, len, "%.0f ms", v);
    else if (!strcmp(d->unit, "dB"))  snprintf(buf, len, "%.0f dB", v);
    else if (!strcmp(d->unit, "st"))  snprintf(buf, len, "%+.2f st", v);
    else if (!strcmp(d->unit, "mix")) {
        if (v < 0.02f) snprintf(buf, len, "só synth");
        else if (v > 0.98f) snprintf(buf, len, "só gtr");
        else snprintf(buf, len, "%.0f%% gtr", v * 100.0f);
    }
    else if (!strcmp(d->unit, "x"))   snprintf(buf, len, "%.2fx", v);
    else                              snprintf(buf, len, "%+.2f", v);
}

/* ------------------------------------------------------------------ */
/* Suavização                                                          */
/* ------------------------------------------------------------------ */
static float g_smooth[P_COUNT];
static int   g_smooth_ready = 0;

void params_smooth_snap(void)
{
    for (int i = 0; i < P_COUNT; i++) g_smooth[i] = param_get(i);
    g_smooth_ready = 1;
}

void params_smooth_step(float coef)
{
    if (!g_smooth_ready) { params_smooth_snap(); return; }
    for (int i = 0; i < P_COUNT; i++) {
        float target = param_get(i), s = g_smooth[i];
        switch (DEFS[i].kind) {
        case K_LIN:
            s += (target - s) * coef;
            if (fabsf(target - s) < 1e-6f) s = target;
            break;
        case K_LOG: /* anda em proporção (oitavas), não em Hz */
            s *= powf(target / s, coef);
            if (fabsf(target - s) < 1e-6f * target) s = target;
            break;
        default:
            s = target;
            break;
        }
        g_smooth[i] = s;
    }
}

float param_s(int id)
{
    if (!g_smooth_ready) params_smooth_snap();
    return g_smooth[id];
}

/* ------------------------------------------------------------------ */
/* Presets                                                             */
/* ------------------------------------------------------------------ */
int preset_save(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# Preset do sintetizador: chave = valor\n");
    for (int i = 0; i < P_COUNT; i++)
        if (!(DEFS[i].flags & F_NOPRESET)) fprintf(f, "%s = %g\n", DEFS[i].key, param_get(i));
    fclose(f);
    return 0;
}

void params_write_all(FILE *f)
{
    for (int i = 0; i < P_COUNT; i++) fprintf(f, "%s = %g\n", DEFS[i].key, param_get(i));
}

int params_read_line(const char *key, const char *value)
{
    int id = param_find(key);
    if (id < 0) return 0;
    param_set(id, (float)atof(value));
    return 1;
}

static void apply_line(char *line)
{
    char *eq = strchr(line, '=');
    if (!eq || line[0] == '#') return;
    *eq = 0;
    char *k = line, *v = eq + 1;
    while (*k == ' ' || *k == '\t') k++;
    char *e = k + strlen(k);
    while (e > k && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    int id = param_find(k);
    if (id >= 0 && !(DEFS[id].flags & F_NOPRESET)) param_set(id, (float)atof(v));
}

int preset_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    params_reset_sound(); /* o que não estiver no arquivo volta ao padrão */
    char line[256];
    while (fgets(line, sizeof line, f)) apply_line(line);
    fclose(f);
    return 0;
}

/* Presets de fábrica: só o que muda em relação ao padrão */
static const struct { const char *name, *data; } FACTORY[] = {
    {"Inicial", ""},
    {"Lead de guitarra",
     "osc1_onda=1;osc2_onda=1;osc2_desafinacao=12;osc_mix=0.3;corte=1400;ressonancia=0.45;"
     "sustentacao=0.9;release=0.25;glide=0.04;gtr_mistura=0.2;"
     "dist_ligada=1;dist_drive=14;dist_tom=4000;dist_mix=0.6;delay_ligado=1;delay_sync=3;"
     "delay_repeticoes=0.35;delay_mix=0.25;reverb_mix=0.18"},
    {"Pad espacial",
     "osc1_onda=1;osc2_onda=1;osc2_desafinacao=0.15;osc_mix=0.5;corte=700;ressonancia=0.2;"
     "ataque=0.9;decaimento=1.5;sustentacao=0.8;release=2.5;filtro_ataque=1.2;filtro_sustentacao=0.5;"
     "mod2_fonte=1;mod2_via=0;mod2_destino=1;mod2_quantidade=0.6;lfo_velocidade=0.3;"
     "chorus_ligado=1;chorus_mix=0.6;reverb_sala=0.9;reverb_mix=0.45"},
    {"Baixo grave",
     "osc1_onda=2;osc2_onda=1;osc2_desafinacao=-12;osc_mix=0.5;corte=300;ressonancia=0.55;"
     "decaimento=0.25;sustentacao=0.6;release=0.12;filtro_decaimento=0.2;mod0_quantidade=3.5;"
     "dist_ligada=1;dist_drive=8;dist_mix=0.4;reverb_ligado=0;gtr_oitava=-1"},
    {"Órgão",
     "osc1_onda=0;osc2_onda=0;osc2_desafinacao=12;osc_mix=0.45;corte=6000;ressonancia=0;"
     "ataque=0.004;sustentacao=1;release=0.06;mod0_quantidade=0;mod1_quantidade=0;"
     "mod2_quantidade=0.25;roda_mod=1;lfo_velocidade=6.5;phaser_ligado=1;phaser_vel=0.6;"
     "phaser_mix=0.5;reverb_mix=0.25"},
    {"Pluck com delay",
     "osc1_onda=3;osc2_onda=2;osc_mix=0.25;corte=500;ressonancia=0.5;decaimento=0.35;"
     "sustentacao=0;release=0.35;filtro_decaimento=0.18;filtro_sustentacao=0;mod0_quantidade=4.5;"
     "delay_ligado=1;delay_sync=3;delay_repeticoes=0.5;delay_mix=0.35;reverb_mix=0.25"},
    {"Arpejo trance",
     "osc1_onda=1;osc2_onda=1;osc2_desafinacao=0.12;corte=800;ressonancia=0.6;decaimento=0.2;"
     "sustentacao=0.3;release=0.15;filtro_decaimento=0.15;filtro_sustentacao=0;mod0_quantidade=4;"
     "arp_ligado=1;arp_modo=2;arp_divisao=3;arp_oitavas=2;arp_gate=0.5;bpm=138;"
     "delay_ligado=1;delay_sync=3;delay_mix=0.25;reverb_mix=0.3"},
};

int         factory_preset_count(void) { return (int)(sizeof FACTORY / sizeof FACTORY[0]); }
const char *factory_preset_name(int i) { return FACTORY[i].name; }

void factory_preset_apply(int i)
{
    params_reset_sound();
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", FACTORY[i].data);
    for (char *tok = strtok(buf, ";"); tok; tok = strtok(NULL, ";")) apply_line(tok);
}
