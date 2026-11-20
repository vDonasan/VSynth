/*
 * Detecção de altura (YIN) e conversão de áudio em notas.
 *
 * YIN: de Cheveigné & Kawahara (2002). Procura o atraso "tau" em que o
 * sinal mais se parece consigo mesmo; a frequência é sr / tau. É mais
 * confiável que a FFT para instrumentos como a guitarra, cujo harmônico
 * às vezes é mais forte que a fundamental.
 *
 * NoteTracker: transforma a sequência de alturas em eventos NOTE_ON /
 * NOTE_OFF / PITCH_TRACK, com noise gate, confirmação da nota em mais
 * de uma janela (evita notas fantasmas) e detecção de nova palhetada.
 */
#ifndef PITCH_H
#define PITCH_H

#include "queue.h"

typedef struct {
    float  sr;
    int    size;      /* tamanho da janela */
    int    tau_min, tau_max;
    float  threshold;
    float *d;         /* diferença normalizada (CMNDF): escolhe o período */
    float *raw;       /* diferença bruta: usada no ajuste fino (sem viés) */
    double *acc;      /* diferença bruta mantida passo a passo (yin_slide) */
} Yin;

int   yin_init(Yin *y, float sr, int size, float fmin, float fmax, float threshold);
void  yin_free(Yin *y);
/* Retorna a frequência em Hz, ou -1 se não houver altura definida.
   Calcula tudo de novo a partir da janela x (size amostras). */
float yin_detect(Yin *y, const float *x, float *confidence);
/* Versão incremental, para janelas que andam `hop` amostras por vez:
   x aponta para o começo da janela ANTIGA, seguida das `hop` amostras
   novas (size + hop amostras ao todo). Custa hop/size do yin_detect. */
void  yin_slide(Yin *y, const float *x, int hop);
/* Detecta a altura com a diferença mantida por yin_slide. */
float yin_eval(Yin *y, float *confidence);
/* Valor da curva normalizada da última análise num período `tau`
   (0 = periódico perfeito, ~1 = nada a ver). 2 = fora do alcance. */
float yin_cmnd_at(const Yin *y, float tau);

typedef struct {
    float on_db, off_db;
    int   stable_needed;  /* janelas seguidas com a mesma nota */
    int   current;        /* nota soando, -1 = nenhuma */
    int   candidate, cand_count;
    float last_db;
    int   attack_pending; /* palhetada detectada, aguardando a altura */
    int   since_on;       /* análises desde o último NOTE_ON */
    int   refractory;     /* análises em que um novo ataque é ignorado */
    int   pend_hops;      /* análises desde a palhetada */
    int   settle;         /* até a janela se renovar, a nota antiga não vale */
    int   last_off, since_off, rering; /* abafou: a mesma nota não volta já */
    float conf;           /* confiança da última análise (0..1) */
    int   since_onset;    /* análises desde o último EV_ONSET */
    int   external_pick;  /* 1: a palhetada vem de fora (campo pick), não do salto de volume */
    int   pick;           /* entrada: houve palhetada nesta análise */
    int   legato_needed;  /* leituras seguidas para trocar de nota sem palhetada */
    int   attack_age;     /* análises desde o último ataque (palhetada ou saída do silêncio) */
    int   start_window;   /* nota nova só até este número de análises depois do ataque */
    int   tail_miss;      /* análises seguidas abaixo do gate sem a nota soando */
    int   tail_hops;
    int   hop_scale;      /* 128 / tamanho do passo: converte contagens para tempo */
    float db_hist[8];     /* volumes recentes, para achar a palhetada */
    int   hist_pos;
    float fast_conf;      /* acima disso, aceita a nota sem esperar confirmação */
} NoteTracker;

void tracker_init(NoteTracker *t, float on_db, float hysteresis_db);
/* O ataque de uma palhetada se espalha por várias análises; durante
   `ms` depois de um NOTE_ON, novos saltos de volume são ignorados. */
void tracker_set_refractory(NoteTracker *t, float ms, float sr, int hop);
/* Depois de uma palhetada, a janela de análise ainda contém a nota
   anterior por algum tempo; durante `hops` análises ela é ignorada. */
void tracker_set_settle(NoteTracker *t, int hops);
/* Passo de análise em amostras (64, 128...): as confirmações e a busca
   da palhetada se ajustam para durar o mesmo tempo. */
void tracker_set_hop(NoteTracker *t, int hop);
/* Escreve até `max` eventos em `out`; retorna quantos. */
int  tracker_process(NoteTracker *t, float hz, float db, Event *out, int max);

float hz_to_midi(float hz);
const char *note_name(int midi, char *buf, int buflen);

#endif
