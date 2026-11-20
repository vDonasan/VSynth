/*
 * Rack de efeitos estéreo: distorção, chorus, phaser, delay e reverb.
 * A ordem é definida pelos parâmetros P_FX_ORDER0..4. Toda a memória é
 * alocada em fx_init: nada de malloc dentro do callback de áudio.
 */
#ifndef FX_H
#define FX_H

typedef enum { FX_DIST, FX_CHORUS, FX_PHASER, FX_DELAY, FX_REVERB, FX_COUNT } FxType;

#define FV_COMBS 8
#define FV_ALLPASS 4
#define PHASER_STAGES 6

typedef struct { float *buf; int size, pos; } Line;

typedef struct {
    float sr;
    /* distorção */
    float dist_lp[2];
    /* chorus */
    Line  cho[2];
    float cho_phase;
    /* phaser */
    float pha_x1[2][PHASER_STAGES], pha_y1[2][PHASER_STAGES], pha_fb[2], pha_phase;
    /* delay */
    Line  dly[2];
    float dly_time_smooth, dly_lp[2];
    /* reverb (Freeverb) */
    Line  comb[2][FV_COMBS];
    float comb_lp[2][FV_COMBS];
    Line  ap[2][FV_ALLPASS];
} Fx;

int         fx_init(Fx *fx, float sr);
void        fx_free(Fx *fx);
void        fx_reset(Fx *fx);
/* Processa no lugar; lê os parâmetros (ordem, ligado/desligado, botões). */
void        fx_process(Fx *fx, float *l, float *r, int frames, float bpm);
const char *fx_name(int type);
int         fx_param_on(int type); /* id do parâmetro "ligado" */

#endif
