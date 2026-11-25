#include "params.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
    [P_GTR_FAST]    = {"gtr_ataque_imediato", "Ataque imediato", 0, 1, 1, K_BOOL, "", NULL, F_NOPRESET},
    [P_GTR_DIRECT]  = {"gtr_modo_direto", "Modo", 0, 1, 0, K_CHOICE, "", "Notas;Direto", F_NOPRESET},
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
int preset_save(const char *path, int category)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# Preset do sintetizador: chave = valor\n");
    fprintf(f, "categoria = %s\n", preset_cat_name(category));
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

int preset_file_category(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return PRESET_CAT_USER;
    char line[256];
    int cat = PRESET_CAT_USER;
    for (int n = 0; n < 4 && fgets(line, sizeof line, f); n++) {
        char v[64];
        if (sscanf(line, " categoria = %63[^\r\n]", v) == 1) {
            int c = preset_cat_find(v);
            if (c >= 0) cat = c;
            break;
        }
    }
    fclose(f);
    return cat;
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

/* Dicas que aparecem ao parar o mouse em cima de um controle */
#define MOD_HELP(i)                                                                                      \
    [P_MOD##i##_SRC] = "De onde vem o movimento: LFO, envelope do filtro, força da nota, roda de mod. ou volume da guitarra.", \
    [P_MOD##i##_VIA] = "Opcional: um segundo sinal que controla quanto da modulação passa (ex.: vibrato só com a roda).", \
    [P_MOD##i##_DST] = "O que vai ser mexido: altura, corte do filtro, volume ou mistura dos osciladores.", \
    [P_MOD##i##_AMT] = "Quanto modular. Negativo inverte o sentido."
static const char *HELP[P_COUNT] = {
    [P_OSC1_WAVE]  = "Forma de onda do oscilador 1. Seno é puro, serra é brilhante, quadrada é oca, triângulo é suave.",
    [P_OSC2_WAVE]  = "Forma de onda do oscilador 2.",
    [P_OSC_MIX]    = "Quanto do oscilador 2 entra no som. 0% = só o OSC1.",
    [P_DETUNE]     = "Afinação do OSC2 em semitons. Valores pequenos engordam o som; 12 = uma oitava acima.",
    [P_CUTOFF]     = "Frequência de corte do filtro. Mais baixo = som mais escuro e abafado.",
    [P_RESONANCE]  = "Realce em volta do corte. Alto dá aquele assobio típico de synth.",
    [P_ATTACK]     = "Tempo para o volume subir quando a nota começa.",
    [P_DECAY]      = "Tempo para o volume cair do pico até a sustentação.",
    [P_SUSTAIN]    = "Volume enquanto a nota continua apertada.",
    [P_RELEASE]    = "Tempo para o som sumir depois que a nota é solta.",
    [P_FATTACK]    = "Tempo para o envelope do filtro abrir.",
    [P_FDECAY]     = "Tempo para o filtro fechar depois do ataque. Curto = som de pluck.",
    [P_FSUSTAIN]   = "Quanto o filtro fica aberto enquanto a nota dura.",
    [P_FRELEASE]   = "Tempo para o filtro fechar depois que a nota é solta.",
    [P_LFO_RATE]   = "Velocidade do LFO, a onda lenta usada para vibrato e movimento.",
    [P_MODWHEEL]   = "Roda de modulação virtual. Por padrão, controla o vibrato.",
    [P_GLIDE]      = "Tempo para deslizar de uma nota para a outra (portamento).",
    [P_SYNTH_LEVEL]= "Volume do sintetizador antes dos efeitos.",
    MOD_HELP(0), MOD_HELP(1), MOD_HELP(2), MOD_HELP(3), MOD_HELP(4), MOD_HELP(5),
    [P_GTR_GATE]   = "Abaixo deste nível a guitarra é ignorada. Use Calibrar gate para achar o valor sozinho.",
    [P_GTR_GAIN]   = "Ganho da entrada da guitarra antes da detecção de notas.",
    [P_GTR_BLEND]  = "Quanto do som limpo da guitarra aparece junto com o synth.",
    [P_GTR_OCTAVE] = "Toca o synth oitavas acima ou abaixo do que a guitarra toca.",
    [P_GTR_BENDS]  = "O synth acompanha bends e vibrato da guitarra.",
    [P_GTR_DIRECT] = "Notas: reconhece cada nota e toca no synth (dá para usar MIDI, harmonizador e arpejador). Direto: o synth segue a onda da corda na hora, sem reconhecer notas: sem atraso e sem nota errada; acordes viram fuzz sintético.",
    [P_GTR_FAST]   = "O som começa já na palhetada, abafado, e a nota se acerta assim que é reconhecida. Bem menos atraso nas cordas graves.",
    [P_DIST_ON]    = "Liga ou desliga a distorção.",
    [P_DIST_DRIVE] = "Quanto saturar. Mais drive = mais sujeira e sustain.",
    [P_DIST_TONE]  = "Brilho da distorção. Baixe se ficar áspero.",
    [P_DIST_MIX]   = "Mistura entre o som limpo e o distorcido.",
    [P_CHO_ON]     = "Liga ou desliga o chorus.",
    [P_CHO_RATE]   = "Velocidade da ondulação do chorus.",
    [P_CHO_DEPTH]  = "Quanto o chorus desafina as cópias. Mais = mais largo.",
    [P_CHO_MIX]    = "Quanto do chorus entra no som.",
    [P_PHA_ON]     = "Liga ou desliga o phaser.",
    [P_PHA_RATE]   = "Velocidade do giro do phaser.",
    [P_PHA_DEPTH]  = "Faixa de frequências que o phaser varre.",
    [P_PHA_FB]     = "Realimentação: deixa o phaser mais marcado e metálico.",
    [P_PHA_MIX]    = "Quanto do phaser entra no som.",
    [P_DLY_ON]     = "Liga ou desliga o delay.",
    [P_DLY_SYNC]   = "Tempo do eco seguindo o BPM, ou Livre para usar milissegundos.",
    [P_DLY_TIME]   = "Tempo do eco em milissegundos (no modo Livre).",
    [P_DLY_FB]     = "Quantas vezes o eco se repete.",
    [P_DLY_MIX]    = "Volume dos ecos.",
    [P_DLY_PING]   = "Os ecos alternam entre esquerda e direita.",
    [P_REV_ON]     = "Liga ou desliga o reverb.",
    [P_REV_SIZE]   = "Tamanho da sala. Mais = cauda mais longa.",
    [P_REV_DAMP]   = "Abafa os agudos da cauda, como uma sala com cortinas.",
    [P_REV_MIX]    = "Quanto de ambiente entra no som.",
    [P_BPM]        = "Andamento em batidas por minuto. Afeta arpejador, delay e metrônomo.",
    [P_METRO_ON]   = "Liga o metrônomo (ele não entra na gravação).",
    [P_METRO_VOL]  = "Volume do metrônomo.",
    [P_ARP_ON]     = "Liga o arpejador: as notas seguradas são tocadas uma por vez, no tempo.",
    [P_ARP_MODE]   = "Ordem em que as notas do arpejo são tocadas.",
    [P_ARP_RATE]   = "Duração de cada passo do arpejo em relação ao tempo.",
    [P_ARP_OCT]    = "Quantas oitavas o arpejo percorre.",
    [P_ARP_GATE]   = "Quanto de cada passo a nota fica soando. Baixo = staccato.",
    [P_ARP_HOLD]   = "Continua arpejando depois que você solta as notas.",
    [P_LOOP_VOL]   = "Volume do que o looper está repetindo.",
    [P_MASTER]     = "Volume geral de saída.",
    [P_HARM_ON]    = "Liga o harmonizador: toca uma segunda voz junto com cada nota.",
    [P_HARM_INTERVAL] = "Distância da segunda voz. Segue a escala escolhida, então a terça muda entre maior e menor sozinha.",
    [P_HARM_KEY]   = "Tom da música. O harmonizador usa as notas dessa escala.",
    [P_HARM_SCALE] = "Escala usada para escolher as notas da harmonia. Fixo = sempre o mesmo número de semitons.",
    [P_HARM_LEVEL] = "Volume da voz harmonizada.",
    [P_HARM_SRC]   = "Harmonizar só o que vem da guitarra ou todas as notas.",
    [P_MOUT_SRC]   = "O que mandar para a saída MIDI (para gravar numa DAW).",
    [P_MOUT_CH]    = "Canal MIDI usado na saída.",
    [P_MOUT_BEND]  = "Faixa de pitch bend configurada no instrumento da DAW, em semitons.",
};
#undef MOD_HELP

const char *param_help(int id) { return (id >= 0 && id < P_COUNT) ? HELP[id] : NULL; }

/* Categorias dos presets (a última é a dos presets do usuário sem categoria) */
static const char *CATS[PRESET_CAT_COUNT] = {"Inicial", "Lead", "Pad", "Baixo", "Teclas", "Pluck", "Arpejo", "Guitarra", "Meus"};
static const char *CAT_TAGS[PRESET_CAT_COUNT] = {"INIT", "LEAD", "PAD", "BAIXO", "TECLAS", "PLUCK", "ARPEJO", "GUITARRA", "MEU"};

const char *preset_cat_name(int c) { return (c >= 0 && c < PRESET_CAT_COUNT) ? CATS[c] : CATS[PRESET_CAT_USER]; }
const char *preset_cat_tag(int c) { return (c >= 0 && c < PRESET_CAT_COUNT) ? CAT_TAGS[c] : CAT_TAGS[PRESET_CAT_USER]; }
int preset_cat_find(const char *name)
{
    for (int c = 0; c < PRESET_CAT_COUNT; c++)
        if (!strcasecmp(name, CATS[c]) || !strcasecmp(name, CAT_TAGS[c])) return c;
    return -1;
}

/* Presets de fábrica: só o que muda em relação ao padrão. Em ordem de
   categoria, para as setas ‹ › passarem por uma categoria de cada vez. */
static const struct { const char *name; int cat; const char *desc, *data; } FACTORY[] = {
    {"Inicial", 0, "Serra simples com filtro: o ponto de partida", ""},

    {"Lead de guitarra", 1, "Duas serras com distorção e delay. Feito para solar",
     "osc1_onda=1;osc2_onda=1;osc2_desafinacao=12;osc_mix=0.3;corte=1400;ressonancia=0.45;"
     "sustentacao=0.9;release=0.25;glide=0.04;gtr_mistura=0.2;"
     "dist_ligada=1;dist_drive=14;dist_tom=4000;dist_mix=0.6;delay_ligado=1;delay_sync=3;"
     "delay_repeticoes=0.35;delay_mix=0.25;reverb_mix=0.18"},
    {"Lead 8 bits", 1, "Onda quadrada seca com glide, som de videogame",
     "osc1_onda=2;osc2_onda=2;osc2_desafinacao=12;osc_mix=0.2;corte=9000;ressonancia=0;"
     "ataque=0.002;sustentacao=1;release=0.05;glide=0.06;mod0_quantidade=0;mod1_quantidade=0;"
     "delay_ligado=1;delay_sync=4;delay_repeticoes=0.25;delay_mix=0.18;reverb_ligado=0"},
    {"Solo cantado", 1, "Serra macia com vibrato e phaser. Use a roda de mod.",
     "osc1_onda=1;osc2_onda=3;osc2_desafinacao=0.1;osc_mix=0.4;corte=2200;ressonancia=0.25;"
     "ataque=0.03;sustentacao=0.85;release=0.4;glide=0.08;roda_mod=0.5;lfo_velocidade=5.8;"
     "phaser_ligado=1;phaser_vel=0.25;phaser_mix=0.5;delay_ligado=1;delay_sync=1;"
     "delay_repeticoes=0.3;delay_mix=0.2;reverb_mix=0.3"},

    {"Pad espacial", 2, "Entrada lenta, filtro que respira e reverb grande",
     "osc1_onda=1;osc2_onda=1;osc2_desafinacao=0.15;osc_mix=0.5;corte=700;ressonancia=0.2;"
     "ataque=0.9;decaimento=1.5;sustentacao=0.8;release=2.5;filtro_ataque=1.2;filtro_sustentacao=0.5;"
     "mod2_fonte=1;mod2_via=0;mod2_destino=1;mod2_quantidade=0.6;lfo_velocidade=0.3;"
     "chorus_ligado=1;chorus_mix=0.6;reverb_sala=0.9;reverb_mix=0.45"},
    {"Cordas quentes", 2, "Naipe de cordas sintético, largo e com chorus",
     "osc1_onda=1;osc2_onda=1;osc2_desafinacao=0.22;osc_mix=0.5;corte=2600;ressonancia=0.1;"
     "ataque=0.35;decaimento=1;sustentacao=0.9;release=1.2;mod0_quantidade=0.5;"
     "chorus_ligado=1;chorus_vel=0.5;chorus_prof=0.7;chorus_mix=0.7;reverb_sala=0.75;reverb_mix=0.35"},
    {"Coral de vidro", 2, "Triângulos brilhantes com phaser lento",
     "osc1_onda=3;osc2_onda=0;osc2_desafinacao=12;osc_mix=0.35;corte=5000;ressonancia=0.15;"
     "ataque=0.6;sustentacao=0.85;release=3;mod0_quantidade=0;"
     "phaser_ligado=1;phaser_vel=0.08;phaser_prof=0.9;phaser_realim=0.7;phaser_mix=0.7;"
     "reverb_sala=0.95;reverb_abafar=0.2;reverb_mix=0.5"},

    {"Baixo grave", 3, "Quadrada com sub uma oitava abaixo e um pouco de sujeira",
     "osc1_onda=2;osc2_onda=1;osc2_desafinacao=-12;osc_mix=0.5;corte=300;ressonancia=0.55;"
     "decaimento=0.25;sustentacao=0.6;release=0.12;filtro_decaimento=0.2;mod0_quantidade=3.5;"
     "dist_ligada=1;dist_drive=8;dist_mix=0.4;reverb_ligado=0;gtr_oitava=-1"},
    {"Baixo ácido", 3, "Serra com ressonância alta e filtro que estala",
     "osc1_onda=1;osc_mix=0;corte=220;ressonancia=0.85;decaimento=0.3;sustentacao=0.7;release=0.08;"
     "filtro_decaimento=0.16;filtro_sustentacao=0;mod0_quantidade=5;mod1_quantidade=2.5;glide=0.05;"
     "dist_ligada=1;dist_drive=12;dist_tom=5000;dist_mix=0.5;reverb_ligado=0"},
    {"Sub limpo", 3, "Seno puro com oitava abaixo: só peso, sem brilho",
     "osc1_onda=0;osc2_onda=0;osc2_desafinacao=-12;osc_mix=0.45;corte=1200;ressonancia=0;"
     "ataque=0.004;sustentacao=1;release=0.1;mod0_quantidade=0;mod1_quantidade=0;reverb_ligado=0;gtr_oitava=-1"},

    {"Órgão", 4, "Senos com vibrato pela roda e phaser de caixa rotativa",
     "osc1_onda=0;osc2_onda=0;osc2_desafinacao=12;osc_mix=0.45;corte=6000;ressonancia=0;"
     "ataque=0.004;sustentacao=1;release=0.06;mod0_quantidade=0;mod1_quantidade=0;"
     "mod2_quantidade=0.25;roda_mod=1;lfo_velocidade=6.5;phaser_ligado=1;phaser_vel=0.6;"
     "phaser_mix=0.5;reverb_mix=0.25"},
    {"Piano elétrico", 4, "Ataque de martelo que some devagar, com chorus",
     "osc1_onda=3;osc2_onda=0;osc2_desafinacao=12;osc_mix=0.3;corte=2400;ressonancia=0.1;"
     "ataque=0.002;decaimento=1.6;sustentacao=0.15;release=0.5;filtro_decaimento=0.8;"
     "mod0_quantidade=1.5;mod1_quantidade=2.5;chorus_ligado=1;chorus_vel=0.9;chorus_mix=0.45;reverb_mix=0.2"},
    {"Sino", 4, "Seno e triângulo afastados, cauda longa",
     "osc1_onda=0;osc2_onda=3;osc2_desafinacao=7;osc_mix=0.35;corte=8000;ressonancia=0;"
     "ataque=0.001;decaimento=2.5;sustentacao=0;release=2.5;mod0_quantidade=0;"
     "reverb_sala=0.85;reverb_abafar=0.3;reverb_mix=0.4"},

    {"Pluck com delay", 5, "Notas curtas que ecoam no tempo da música",
     "osc1_onda=3;osc2_onda=2;osc_mix=0.25;corte=500;ressonancia=0.5;decaimento=0.35;"
     "sustentacao=0;release=0.35;filtro_decaimento=0.18;filtro_sustentacao=0;mod0_quantidade=4.5;"
     "delay_ligado=1;delay_sync=3;delay_repeticoes=0.5;delay_mix=0.35;reverb_mix=0.25"},
    {"Harpa", 5, "Beliscado suave de triângulo, bom para arpejos lentos",
     "osc1_onda=3;osc2_onda=0;osc2_desafinacao=12;osc_mix=0.2;corte=1800;ressonancia=0.15;"
     "decaimento=1.2;sustentacao=0;release=1.2;filtro_decaimento=0.5;filtro_sustentacao=0;"
     "mod0_quantidade=2.5;reverb_sala=0.7;reverb_mix=0.35"},
    {"Kalimba", 5, "Toque curto e oco, como lâminas de metal",
     "osc1_onda=0;osc2_onda=3;osc2_desafinacao=12;osc_mix=0.4;corte=3000;ressonancia=0.3;"
     "ataque=0.001;decaimento=0.45;sustentacao=0;release=0.45;filtro_decaimento=0.08;filtro_sustentacao=0;"
     "mod0_quantidade=2;reverb_sala=0.5;reverb_mix=0.25"},

    {"Arpejo trance", 6, "Arpejador ligado: sobe e desce em semicolcheias a 138 BPM",
     "osc1_onda=1;osc2_onda=1;osc2_desafinacao=0.12;corte=800;ressonancia=0.6;decaimento=0.2;"
     "sustentacao=0.3;release=0.15;filtro_decaimento=0.15;filtro_sustentacao=0;mod0_quantidade=4;"
     "arp_ligado=1;arp_modo=2;arp_divisao=3;arp_oitavas=2;arp_gate=0.5;bpm=138;"
     "delay_ligado=1;delay_sync=3;delay_mix=0.25;reverb_mix=0.3"},
    {"Sequência anos 80", 6, "Quadrada com chorus, arpejo subindo a 118 BPM",
     "osc1_onda=2;osc2_onda=1;osc2_desafinacao=0.1;osc_mix=0.3;corte=1600;ressonancia=0.35;"
     "decaimento=0.2;sustentacao=0.2;release=0.12;filtro_decaimento=0.12;filtro_sustentacao=0.1;"
     "mod0_quantidade=3;arp_ligado=1;arp_modo=0;arp_divisao=3;arp_oitavas=2;arp_gate=0.4;bpm=118;"
     "chorus_ligado=1;chorus_mix=0.5;reverb_mix=0.25"},

    {"Guitarra harmonizada", 7, "Toca uma terça acima junto com a guitarra (escala de Dó)",
     "osc1_onda=1;osc2_onda=1;osc2_desafinacao=0.1;osc_mix=0.4;corte=1800;ressonancia=0.3;"
     "sustentacao=0.85;release=0.3;gtr_mistura=0.55;harm_ligado=1;harm_volume=0.6;"
     "delay_ligado=1;delay_sync=3;delay_repeticoes=0.3;delay_mix=0.2;reverb_mix=0.25"},
    {"Oitavador", 7, "Guitarra com um sub uma oitava abaixo, estilo pedal",
     "osc1_onda=0;osc2_onda=2;osc2_desafinacao=0;osc_mix=0.25;corte=900;ressonancia=0.1;"
     "ataque=0.003;sustentacao=1;release=0.08;gtr_mistura=0.6;gtr_oitava=-1;mod0_quantidade=0;mod3_quantidade=1;"
     "reverb_ligado=0"},
    {"Synth-guitarra fuzz", 7, "A guitarra controla um synth com fuzz e o filtro segue a palhetada",
     "osc1_onda=1;osc2_onda=2;osc2_desafinacao=0.08;osc_mix=0.4;corte=500;ressonancia=0.5;"
     "sustentacao=1;release=0.15;gtr_mistura=0.15;mod3_quantidade=4;"
     "dist_ligada=1;dist_drive=26;dist_tom=3000;dist_mix=0.8;reverb_mix=0.2"},
};

int         factory_preset_count(void) { return (int)(sizeof FACTORY / sizeof FACTORY[0]); }
const char *factory_preset_name(int i) { return FACTORY[i].name; }
int         factory_preset_category(int i) { return FACTORY[i].cat; }
const char *factory_preset_desc(int i) { return FACTORY[i].desc; }

void factory_preset_apply(int i)
{
    params_reset_sound();
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", FACTORY[i].data);
    for (char *tok = strtok(buf, ";"); tok; tok = strtok(NULL, ";")) apply_line(tok);
}
