/*
 * Tabela central de parâmetros.
 *
 * Todo botão/slider da interface é um parâmetro desta tabela. Os valores
 * são atômicos, então a interface (ou o MIDI) pode mudá-los a qualquer
 * momento e o callback de áudio lê sem travas no começo de cada bloco.
 * A mesma tabela serve para salvar presets e para o "MIDI learn".
 */
#ifndef PARAMS_H
#define PARAMS_H

typedef enum { K_LIN, K_LOG, K_INT, K_BOOL, K_CHOICE } ParamKind;

enum {
    F_NOPRESET = 1, /* não é salvo em presets (configuração do equipamento) */
    F_HIDDEN   = 2  /* não aparece para MIDI learn */
};

typedef enum {
    /* osciladores e filtro */
    P_OSC1_WAVE, P_OSC2_WAVE, P_OSC_MIX, P_DETUNE,
    P_CUTOFF, P_RESONANCE,
    P_ATTACK, P_DECAY, P_SUSTAIN, P_RELEASE,
    P_FATTACK, P_FDECAY, P_FSUSTAIN, P_FRELEASE,
    P_LFO_RATE, P_MODWHEEL, P_GLIDE, P_SYNTH_LEVEL,
    /* matriz de modulação: 6 "cabos" */
    P_MOD0_SRC, P_MOD0_VIA, P_MOD0_DST, P_MOD0_AMT,
    P_MOD1_SRC, P_MOD1_VIA, P_MOD1_DST, P_MOD1_AMT,
    P_MOD2_SRC, P_MOD2_VIA, P_MOD2_DST, P_MOD2_AMT,
    P_MOD3_SRC, P_MOD3_VIA, P_MOD3_DST, P_MOD3_AMT,
    P_MOD4_SRC, P_MOD4_VIA, P_MOD4_DST, P_MOD4_AMT,
    P_MOD5_SRC, P_MOD5_VIA, P_MOD5_DST, P_MOD5_AMT,
    /* entrada */
    P_INPUT_MODE, P_GTR_GATE, P_GTR_GAIN, P_GTR_BLEND, P_GTR_OCTAVE, P_GTR_BENDS, P_KEYS_OCTAVE,
    /* efeitos */
    P_DIST_ON, P_DIST_DRIVE, P_DIST_TONE, P_DIST_MIX,
    P_CHO_ON, P_CHO_RATE, P_CHO_DEPTH, P_CHO_MIX,
    P_PHA_ON, P_PHA_RATE, P_PHA_DEPTH, P_PHA_FB, P_PHA_MIX,
    P_DLY_ON, P_DLY_SYNC, P_DLY_TIME, P_DLY_FB, P_DLY_MIX, P_DLY_PING,
    P_REV_ON, P_REV_SIZE, P_REV_DAMP, P_REV_MIX,
    P_FX_ORDER0, P_FX_ORDER1, P_FX_ORDER2, P_FX_ORDER3, P_FX_ORDER4,
    /* ritmo */
    P_BPM, P_METRO_ON, P_METRO_VOL,
    P_ARP_ON, P_ARP_MODE, P_ARP_RATE, P_ARP_OCT, P_ARP_GATE, P_ARP_HOLD,
    P_LOOP_VOL, P_MASTER,
    /* harmonizador */
    P_HARM_ON, P_HARM_INTERVAL, P_HARM_KEY, P_HARM_SCALE, P_HARM_LEVEL, P_HARM_SRC,
    /* saída MIDI (guitarra -> DAW) */
    P_MOUT_SRC, P_MOUT_CH, P_MOUT_BEND,
    P_COUNT
} ParamId;

#define MOD_SLOTS 6
#define P_MOD_SRC(i) (P_MOD0_SRC + 4 * (i))
#define P_MOD_VIA(i) (P_MOD0_VIA + 4 * (i))
#define P_MOD_DST(i) (P_MOD0_DST + 4 * (i))
#define P_MOD_AMT(i) (P_MOD0_AMT + 4 * (i))

typedef struct {
    const char *key;     /* nome no arquivo de preset */
    const char *label;   /* nome na interface */
    float       min, max, def;
    ParamKind   kind;
    const char *unit;
    const char *choices; /* "A;B;C" para K_CHOICE */
    int         flags;
} ParamDef;

void            params_init(void);           /* todos no valor padrão */
void            params_reset_sound(void);    /* padrão, menos F_NOPRESET */
const ParamDef *param_def(int id);
float           param_get(int id);
int             param_geti(int id);
void            param_set(int id, float v);  /* limita e arredonda */
float           param_get_norm(int id);      /* 0..1 (respeita escala log) */
void            param_set_norm(int id, float n);
int             param_find(const char *key);
void            param_format(int id, char *buf, int len); /* "900 Hz", "35%"... */

/* Valores suavizados (só para a thread de áudio).
 * Um knob girado rápido muda o valor em degraus; ouvido direto isso vira
 * um chiado tipo "zíper". params_smooth_step() aproxima cada valor
 * contínuo do alvo aos poucos (constante de tempo ~30 ms). Parâmetros de
 * escolha/inteiros não são suavizados. */
void  params_smooth_step(float coef); /* chamar a cada pedaço de áudio */
void  params_smooth_snap(void);       /* pula direto para os alvos */
float param_s(int id);                /* valor suavizado */

/* Todos os parâmetros (inclusive os que não vão para presets), para o
   arquivo de configurações. params_read_line devolve 1 se reconheceu. */
#include <stdio.h>
void params_write_all(FILE *f);
int  params_read_line(const char *key, const char *value);

/* Presets: arquivos de texto "chave = valor" */
int         preset_save(const char *path);
int         preset_load(const char *path);
int         factory_preset_count(void);
const char *factory_preset_name(int i);
void        factory_preset_apply(int i);

#endif
