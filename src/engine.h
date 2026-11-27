/*
 * Motor de áudio: junta tudo o que acontece dentro do callback.
 *
 *  teclado/piano ─fila─┐
 *  MIDI ──────────fila─┼─> [arpejador] ─> Synth ─┐
 *  guitarra (YIN) fila─┘                         ├─> efeitos ─> looper ─> volume ─> saída
 *  guitarra (som limpo) ─────────────────────────┘                  │        + metrônomo
 *                                                                   └─> gravação WAV
 */
#ifndef ENGINE_H
#define ENGINE_H

#include <stdatomic.h>
#include <stdbool.h>

#include "arp.h"
#include "fx.h"
#include "guitar.h"
#include "gtr_direct.h"
#include "looper.h"
#include "queue.h"
#include "synth.h"

#define ENGINE_CHUNK   32    /* resolução de tempo do arpejador/metrônomo (0,7 ms) */
#define SCOPE_SIZE     2048
#define MAX_AUDIO_DEVS 32
#define REC_RING_SIZE  (1u << 19)

/* Os detalhes do miniaudio ficam escondidos em engine.c: no Windows o
   miniaudio.h inclui windows.h, que tem nomes iguais aos do raylib. */
typedef struct EngineAudio EngineAudio;

typedef struct {
    char name[256];
    int  is_default;
    int  asio;      /* >= 0: driver ASIO (entrada e saída juntas); -1: miniaudio */
} AudioDevice;

typedef struct {
    float  sr;
    Synth  synth;
    Fx     fx;
    Arp    arp;
    Looper looper;
    Guitar guitar;

    EventQueue q_ui;    /* teclado do computador e piano na tela */
    EventQueue q_midi;
    EventQueue q_gtr;
    EventQueue q_mout;  /* notas que vão para a saída MIDI (thread do MIDI envia) */

    /* dispositivos */
    EngineAudio *audio;
    int         null_backend; /* testes: áudio "de mentira", mas no tempo certo */
    int         dev_ok;
    unsigned    dev_rate;       /* taxa real do dispositivo */
    int         period_frames;  /* tamanho do buffer pedido (64, 128, 256...) */
    int         exclusive;      /* modo exclusivo (WASAPI no Windows): menos atraso */
    int         exclusive_failed;
    int         asio_open;      /* o dispositivo aberto é um driver ASIO */
    char        asio_error[256];
    float       dev_latency_ms; /* tamanho do buffer em ms */
    float       bridge_ms;      /* > 0: entrada e saída em aparelhos diferentes, atraso da ponte */
    int         force_duplex;   /* não usa a ponte mesmo com aparelhos diferentes */
    AudioDevice play[MAX_AUDIO_DEVS], cap[MAX_AUDIO_DEVS];
    int         nplay, ncap;
    atomic_int  cap_channel; /* a interface pode trocar com o áudio rodando */
    char        dev_name[256], cap_name[256];

    /* estado do callback */
    int    arp_was_on;
    int    gtr_arp_note;
    Direct direct;           /* modo direto da guitarra */
    int    direct_on;        /* modo em uso (-1 = ainda não lido) */
    int    gtr_pending;      /* ataque imediato: nota provisória soando */
    int    gtr_pending_age;  /* amostras desde a palhetada */
    float  gtr_last_note;    /* última nota da guitarra: palpite para a próxima */
    int    harm_map[2][128][2]; /* notas de harmonia por tecla (MIDI, teclado) */
    int    harm_audio[2];       /* harmonia da nota atual da guitarra */
    int    mout_audio_note, mout_prev;
    double metro_counter;
    int    metro_beat;
    float  click_env, click_phase, click_freq;
    float  l[ENGINE_CHUNK], r[ENGINE_CHUNK], mono[ENGINE_CHUNK];
    float  gtr_in[4096];

    /* monitoramento para a interface */
    _Atomic float scope[SCOPE_SIZE];
    atomic_uint   scope_pos;
    _Atomic float peak;
    atomic_uchar  notes_on[128];
    atomic_int    beat;

    /* medidor de latência real (ida e volta pela placa de som) */
    atomic_int    lat_state;   /* LatState */
    atomic_int    lat_result;  /* amostras entre o clique sair e voltar */
    long long     sample_clock, lat_t0, lat_deadline;
    float         lat_noise;

    /* gravação: o callback escreve no anel, a interface grava no disco */
    FloatRing   rec_ring;
    FloatRing   rec_gtr_ring;  /* gravação para diagnóstico: entrada crua da guitarra + synth */
    atomic_bool rec_on;
} Engine;

typedef enum { LAT_IDLE, LAT_NOISE, LAT_CLICK, LAT_LISTEN, LAT_DONE, LAT_FAIL } LatState;

int  engine_init(Engine *e, float sr);
/* Mede a latência de ida e volta: mede o ruído da entrada, toca um clique
   e espera ele voltar pela entrada (cabo da saída na entrada, ou
   microfone perto do alto-falante). Resultado em lat_result (amostras). */
void engine_latency_start(Engine *e);
void engine_free(Engine *e);

/* in: amostras entrelaçadas de captura (ou NULL); out: estéreo entrelaçado */
void engine_process(Engine *e, const float *in, int in_channels, float *out, int frames);

int  engine_refresh_devices(Engine *e);
/* play/cap: índice na lista, -1 = padrão do sistema; cap = -2 sem entrada */
int  engine_open(Engine *e, int play, int cap, int cap_channel);
void engine_close(Engine *e);
int  engine_capture_channels(Engine *e);
int  engine_has_asio(void);          /* compilado com suporte a ASIO */
void engine_asio_panel(Engine *e);   /* abre o painel do driver ASIO */

/* Notas que o harmonizador acrescenta a `note` (até 2); retorna quantas. */
int  engine_harmony_notes(int note, int out[2]);

/* Liga FTZ/DAZ na thread atual (o callback já chama sozinho). */
void engine_disable_denormals(void);

/* Envia um evento da thread da interface (teclado/piano). */
void engine_ui_event(Engine *e, EventType type, int note, int vel);

#endif
