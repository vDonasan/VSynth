/*
 * Modo direto da guitarra (sem reconhecer notas).
 *
 * Como os pedais de synth de guitarra (Boss SY), o modo direto usa a
 * PRÓPRIA onda da corda como fonte do som, em vez de descobrir a nota e
 * tocá-la: o OSC1 é a onda da corda transformada (quadrada = o sinal da
 * onda; seno = a fundamental isolada; serra e triângulo derivados dos
 * cruzamentos). Não há atraso além do buffer de áudio e não existe "nota
 * errada": mesmo quando os harmônicos criam cruzamentos a mais, a onda
 * continua se repetindo no ciclo da nota tocada, só o timbre muda (como
 * num pedal de fuzz). Bends e vibrato vêm junto. Acordes viram um fuzz
 * sintético, como nos synths de guitarra analógicos.
 *
 * O OSC2 (com desafinação) e as oitavas precisam saber o período da nota:
 * esses entram assim que a nota é confirmada pelo reconhecimento.
 *
 * Usa os mesmos controles do synth: formas de onda, OSC2 (desafinação e
 * mistura), filtro, envelopes (disparados pela palhetada), LFO e matriz
 * de modulação. A oitava da guitarra multiplica a frequência medida.
 */
#ifndef GTR_DIRECT_H
#define GTR_DIRECT_H

#include "synth.h"

typedef struct {
    float sr;
    /* análise da corda */
    float dc_x1, dc_y1;
    float lp1, lp2, lp_coef;  /* filtro de banda em volta da nota: passa-baixa ... */
    float hp1, hp2, hp3, hp4, hp_coef;  /* ... e passa-altas (tira as cordas mais graves ressoando) */
    float center;             /* Hz em volta do qual a banda está (0 = banda aberta) */
    int   hint_agree;
    float peak;               /* pico da nota atual (desde a palhetada) */
    float dcs_x, dcs_y;       /* tira o DC da mistura dos osciladores */
    float acc_P;              /* subidas somadas desde o último ciclo inteiro da nota */
    float off_time;           /* amostras seguidas com o ciclo longe da nota confirmada */         /* leituras do reconhecimento pedindo outra nota */
    float y_prev;
    float env_y;              /* tamanho da onda filtrada, para a histerese */
    float level;              /* volume da corda (seguidor) */
    float prev_level;
    float low;                /* volume mínimo recente (~20 ms) */
    float fast, ref;          /* seguidor rápido e referência: percebe a corda abafada */
    int   state;              /* comparador com histerese: +1 / -1 */
    float since_edge;         /* amostras desde a última subida aceita como ciclo */
    float since_edge_prev;    /* amostras desde a última subida (qualquer) */
    float period;             /* amostras por ciclo (o último medido) */
    int   have_period;
    int   suspect;            /* ciclo curto demais: espera confirmar */
    unsigned edges;           /* subidas contadas (sincronia das oitavas abaixo) */
    float last_gap;           /* amostras entre as duas últimas subidas (rampa da serra) */
    float tri;                /* triângulo: quadrada integrada */
    float sq_lp;              /* quadrada suavizada (menos aliasing) */
    float synced;             /* 0..1: osciladores medidos liberados (nota confirmada) */
    /* síntese */
    float ph1, ph2, lfo_ph;
    float ic1, ic2;
    Adsr  aenv, fenv;
    float velocity;
    int   gate;               /* corda soando acima do gate */
    unsigned last_pick;       /* palhetadas vistas (do detector da guitarra) */
} Direct;

void direct_init(Direct *d, float sr);
void direct_reset(Direct *d);
/* in: guitarra crua; out: som do synth (somado em out). picks: contador de
   palhetadas do detector da guitarra (muda = palhetada nova). */
/* hint_hz: leitura confiável do reconhecimento de notas neste instante
   (0 = nenhuma). Serve só para fechar a banda em volta da nota certa; o som
   segue os ciclos da corda. */
void direct_process(Direct *d, const Patch *p, const float *in, float *out, int n, unsigned picks, float hint_hz);
/* frequência que o modo direto está seguindo agora (0 = nenhuma) */
float direct_hz(const Direct *d);

#endif
