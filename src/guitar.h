/*
 * Guitarra / microfone: transforma o áudio de entrada em notas.
 *
 * O callback de áudio só COPIA as amostras para um anel. Uma thread
 * separada faz a análise pesada (YIN) a cada PITCH_HOP amostras:
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
    FloatRing     ring;
    atomic_bool   running;
    void         *thread;   /* pthread_t, escondido (ver engine.h) */
    int           started;
    Yin           yin;        /* janela longa (graves) */
    Yin           yin_short;  /* janela curta (agudas, menos atraso) */
    NoteTracker   tracker;
    float         sr;
    float         dc_x1, dc_y1, lp_y1, lp_coef;
    float         window[PITCH_WINDOW];
    float         held_hz;    /* frequência da nota que está soando */
    /* lidos pela interface (afinador, medidor) e pelo motor */
    atomic_int    st_note;
    _Atomic float st_hz, st_db, level; /* level: 0..1, fonte de modulação */
} Guitar;

int  guitar_init(Guitar *g, float sr, EventQueue *q);  /* sem thread (testes) */
int  guitar_start(Guitar *g);                           /* inicia a thread */
void guitar_stop(Guitar *g);
void guitar_free(Guitar *g);
void guitar_reset(Guitar *g);                           /* esquece o estado (troca de entrada) */
void guitar_push(Guitar *g, const float *mono, int frames); /* do callback */
void guitar_process_hop(Guitar *g, const float *hop);   /* PITCH_HOP amostras */

#endif
