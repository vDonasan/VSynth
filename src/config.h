#ifndef CONFIG_H
#define CONFIG_H

/* Áudio */
#define SAMPLE_RATE        48000
#define FRAMES_PER_BUFFER  128      /* ~2,7 ms de latência por buffer */
#define MAX_VOICES         8        /* polifonia */
#define MAX_MOD_SLOTS      8        /* "cabos" da matriz de modulação */

/* Detecção de altura (guitarra / microfone) */
#define PITCH_WINDOW       1280     /* janela longa: 27 ms, cobre as notas graves */
#define PITCH_SHORT_WINDOW 768      /* janela curta: 16 ms, responde rápido nas agudas */
#define PITCH_SHORT_MIN_HZ 180.0f   /* abaixo disso só a janela longa é confiável */
#define PITCH_HOP          128      /* nova análise a cada 2,7 ms */
#define PITCH_FMIN         70.0f    /* Hz – cobre afinação drop-D (D2 = 73 Hz) */
#define PITCH_FMAX         1500.0f  /* Hz */
#define PITCH_THRESHOLD    0.15f    /* limiar do YIN (menor = mais exigente) */
#define GATE_ON_DB         -42.0f   /* nível de entrada para começar uma nota */
#define GATE_HYSTERESIS_DB 8.0f     /* nota termina em GATE_ON_DB - histerese */

#endif
