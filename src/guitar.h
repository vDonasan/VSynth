/*
 * Guitarra / microfone: transforma o áudio de entrada em notas.
 *
 * A análise roda dentro do próprio callback de áudio, a cada PITCH_HOP
 * amostras (guitar_feed). Assim a nota reconhecida começa no ponto exato
 * do bloco em que foi reconhecida, sem esperar outra thread nem o
 * próximo bloco. Em cada passo:
 *   1. filtra (remove DC e agudos acima de ~1,6 kHz, que confundem o YIN)
 *   2. mede o volume (dB)
 *   3. detecta a frequência: primeiro numa janela curta (rápida, serve
 *      para notas agudas); se não der, na janela longa (notas graves)
 *   4. o NoteTracker gera NOTE_ON/OFF/PITCH_TRACK
 * Gate e ganho são lidos da tabela de parâmetros a cada análise.
 */
#ifndef GUITAR_H
#define GUITAR_H

#include <stdatomic.h>
#include <stdbool.h>

#include "config.h"
#include "pitch.h"
#include "queue.h"

typedef struct {
    EventQueue   *q;
    int           started;
    float         hop_buf[PITCH_HOP];
    int           hop_fill;
    Yin           yin;        /* janela longa (graves) */
    Yin           yin_short;  /* janela curta (agudas, menos atraso) */
    Yin           yin_mid;    /* janela média (graves, menos atraso que a longa) */
    NoteTracker   tracker;
    float         sr;
    float         dc_x1, dc_y1, lp_y1, lp_coef;
    /* volume: seguidor de pico (não oscila dentro de um ciclo da corda) */
    float         env, env_rel;
    /* palhetada: o "tchic" da palheta tem muito agudo; a corda soando, pouco */
    float         hp1, hp2, hp_coef, hf_env, hf_rel;
    float         hf_hist[8];
    int           fresh;        /* amostras desde a última palhetada */
    float         prev_db;
    float         db_hist[6];
    int           pick_hold;    /* o estalo da palhetada anterior ainda está subindo */
    int           hf_pos;
    float         window[PITCH_WINDOW + PITCH_HOP]; /* janela + passo novo */
    float         held_hz;    /* frequência da nota que está soando */
    /* lidos pela interface (afinador, medidor) e pelo motor */
    atomic_int    st_note;
    _Atomic float st_hz, st_db, level; /* level: 0..1, fonte de modulação */
} Guitar;

int  guitar_init(Guitar *g, float sr, EventQueue *q);
int  guitar_start(Guitar *g);                           /* antes de abrir o áudio */
void guitar_stop(Guitar *g);                            /* depois de fechar o áudio */
void guitar_free(Guitar *g);
void guitar_reset(Guitar *g);                           /* esquece o estado (troca de entrada) */
/* Do callback: acumula amostras e analisa a cada PITCH_HOP. Devolve
   quantas análises rodaram (os eventos vão para a fila q). */
int  guitar_feed(Guitar *g, const float *mono, int frames);
void guitar_process_hop(Guitar *g, const float *hop);   /* PITCH_HOP amostras */

#endif
