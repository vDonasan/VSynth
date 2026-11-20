#include "looper.h"

#include <stdlib.h>
#include <string.h>

int looper_init(Looper *lp, float sr, float max_seconds)
{
    memset(lp, 0, sizeof *lp);
    lp->max_len = (int)(sr * max_seconds);
    lp->l  = calloc((size_t)lp->max_len, sizeof(float));
    lp->r  = calloc((size_t)lp->max_len, sizeof(float));
    lp->ul = calloc((size_t)lp->max_len, sizeof(float));
    lp->ur = calloc((size_t)lp->max_len, sizeof(float));
    atomic_init(&lp->cmd, LCMD_NONE);
    atomic_init(&lp->st_state, LOOP_EMPTY);
    atomic_init(&lp->st_pos, 0);
    atomic_init(&lp->st_len, 0);
    return (lp->l && lp->r && lp->ul && lp->ur) ? 0 : -1;
}

void looper_free(Looper *lp)
{
    free(lp->l); free(lp->r); free(lp->ul); free(lp->ur);
    lp->l = lp->r = lp->ul = lp->ur = NULL;
}

void looper_command(Looper *lp, LoopCmd cmd) { atomic_store(&lp->cmd, cmd); }

/* Copiar a trilha inteira dentro do callback poderia causar estalos; por
   isso a cópia para "desfazer" é feita amostra a amostra durante o overdub. */
static void handle_command(Looper *lp, LoopCmd cmd)
{
    switch (cmd) {
    case LCMD_REC: /* botão principal: grava -> toca -> overdub -> toca ... */
        switch (lp->state) {
        case LOOP_EMPTY:
            lp->state = LOOP_RECORDING; lp->len = 0; lp->pos = 0; lp->can_undo = 0;
            break;
        case LOOP_RECORDING:
            lp->len = lp->pos > 0 ? lp->pos : 1; lp->pos = 0; lp->state = LOOP_PLAYING;
            break;
        case LOOP_PLAYING:
        case LOOP_STOPPED:
            lp->state = LOOP_OVERDUB; lp->can_undo = 1;
            lp->od_start = lp->pos; lp->od_count = 0;
            break;
        case LOOP_OVERDUB:
            lp->state = LOOP_PLAYING;
            break;
        }
        break;
    case LCMD_PLAY:
        if (lp->state == LOOP_STOPPED) { lp->state = LOOP_PLAYING; lp->pos = 0; }
        break;
    case LCMD_STOP:
        if (lp->state == LOOP_RECORDING) lp->len = lp->pos > 0 ? lp->pos : 1;
        if (lp->state != LOOP_EMPTY) { lp->state = LOOP_STOPPED; lp->pos = 0; }
        break;
    case LCMD_CLEAR:
        lp->state = LOOP_EMPTY; lp->len = 0; lp->pos = 0; lp->can_undo = 0;
        break;
    case LCMD_UNDO:
        if (lp->can_undo && lp->state != LOOP_RECORDING && lp->state != LOOP_EMPTY) {
            int n = lp->od_count < lp->len ? lp->od_count : lp->len;
            for (int k = 0; k < n; k++) {
                int idx = (lp->od_start + k) % lp->len;
                lp->l[idx] = lp->ul[idx];
                lp->r[idx] = lp->ur[idx];
            }
            lp->can_undo = 0;
            if (lp->state == LOOP_OVERDUB) lp->state = LOOP_PLAYING;
        }
        break;
    default: break;
    }
}

void looper_process(Looper *lp, float *l, float *r, int frames, float volume)
{
    LoopCmd cmd = (LoopCmd)atomic_exchange(&lp->cmd, LCMD_NONE);
    if (cmd != LCMD_NONE) handle_command(lp, cmd);

    for (int i = 0; i < frames; i++) {
        switch (lp->state) {
        case LOOP_RECORDING:
            lp->l[lp->pos] = l[i];
            lp->r[lp->pos] = r[i];
            if (++lp->pos >= lp->max_len) { lp->len = lp->max_len; lp->pos = 0; lp->state = LOOP_PLAYING; }
            break;
        case LOOP_PLAYING:
            l[i] += lp->l[lp->pos] * volume;
            r[i] += lp->r[lp->pos] * volume;
            if (++lp->pos >= lp->len) lp->pos = 0;
            break;
        case LOOP_OVERDUB: {
            float pl = lp->l[lp->pos], pr = lp->r[lp->pos];
            if (lp->od_count < lp->len) { lp->ul[lp->pos] = pl; lp->ur[lp->pos] = pr; lp->od_count++; }
            lp->l[lp->pos] = pl + l[i];
            lp->r[lp->pos] = pr + r[i];
            l[i] += pl * volume;
            r[i] += pr * volume;
            if (++lp->pos >= lp->len) lp->pos = 0;
            break;
        }
        default: break;
        }
    }
    atomic_store_explicit(&lp->st_state, lp->state, memory_order_relaxed);
    atomic_store_explicit(&lp->st_pos, lp->pos, memory_order_relaxed);
    atomic_store_explicit(&lp->st_len, lp->state == LOOP_RECORDING ? lp->pos : lp->len, memory_order_relaxed);
}
