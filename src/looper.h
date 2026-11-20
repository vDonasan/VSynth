/*
 * Looper: grava um trecho, repete em loop e permite gravar por cima
 * (overdub). Os comandos chegam da interface por uma variável atômica e
 * são executados dentro do callback de áudio.
 */
#ifndef LOOPER_H
#define LOOPER_H

#include <stdatomic.h>

typedef enum { LOOP_EMPTY, LOOP_RECORDING, LOOP_PLAYING, LOOP_OVERDUB, LOOP_STOPPED } LoopState;
typedef enum { LCMD_NONE, LCMD_REC, LCMD_PLAY, LCMD_STOP, LCMD_CLEAR, LCMD_UNDO } LoopCmd;

typedef struct {
    float     *l, *r;       /* trilha principal */
    float     *ul, *ur;     /* cópia para desfazer o último overdub */
    int        max_len, len, pos;
    LoopState  state;
    int        can_undo;
    int        od_start, od_count; /* trecho salvo para desfazer o overdub */
    atomic_int cmd;
    /* para a interface */
    atomic_int st_state, st_pos, st_len;
} Looper;

int  looper_init(Looper *lp, float sr, float max_seconds);
void looper_free(Looper *lp);
void looper_command(Looper *lp, LoopCmd cmd); /* chamado pela interface */
/* Grava (se for o caso) o sinal ao vivo e soma a reprodução nele. */
void looper_process(Looper *lp, float *l, float *r, int frames, float volume);

#endif
