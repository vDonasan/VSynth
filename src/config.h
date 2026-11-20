#ifndef CONFIG_H
#define CONFIG_H

/* Áudio */
#define SAMPLE_RATE        48000
#define FRAMES_PER_BUFFER  128      /* ~2,7 ms de latência por buffer */
#define MAX_VOICES         8        /* polifonia */
#define MAX_MOD_SLOTS      8        /* "cabos" da matriz de modulação */

/* Detecção de altura (guitarra / microfone) */
#define PITCH_WINDOW       1280     /* janela longa: 27 ms, cobre as notas graves */
#define PITCH_SHORT_WINDOW 512      /* janela curta: 10,7 ms, responde rápido nas agudas */
#define PITCH_SHORT_FMIN   160.0f   /* menor frequência que a janela curta procura */
#define PITCH_SHORT_MIN_HZ 180.0f   /* abaixo disso a curta não é confiável */
#define PITCH_MID_WINDOW   832      /* janela média: 17 ms, para as cordas graves */
#define PITCH_MID_FMIN     78.0f    /* (o Ré do drop-D, 73 Hz, fica com a longa) */
#define PITCH_FAST_CONF    0.92f    /* leitura com confiança acima disso não espera confirmação */
#define PITCH_SETTLE_MS    16.0f    /* tempo ignorando a nota antiga após a palhetada */
#define PITCH_HOP          64       /* nova análise a cada 1,3 ms */
#define PITCH_TAIL_DB      16.0f    /* nota segue até 16 dB abaixo do fim do gate, se a altura bater */
#define PITCH_TAIL_MS      40.0f    /* abaixo do gate: 40 ms sem a nota certa e ela termina */
#define PITCH_START_MS     120.0f   /* nota só começa até 120 ms depois de um ataque */
#define PITCH_LEGATO_MS    20.0f    /* troca de nota sem palhetada: leituras iguais por 20 ms */
#define PITCH_FMIN         70.0f    /* Hz – cobre afinação drop-D (D2 = 73 Hz) */
#define PITCH_FMAX         1500.0f  /* Hz */
#define PITCH_THRESHOLD    0.15f    /* limiar do YIN (menor = mais exigente) */
#define GATE_ON_DB         -42.0f   /* nível de entrada para começar uma nota */
#define GATE_HYSTERESIS_DB 8.0f     /* nota termina em GATE_ON_DB - histerese */

#endif
