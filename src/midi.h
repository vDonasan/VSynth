/*
 * Entrada MIDI (PortMidi). Notas, pitch bend e pedal viram eventos; os
 * outros controladores (knobs, faders) mudam parâmetros diretamente,
 * conforme o mapa de "MIDI learn".
 */
#ifndef MIDI_H
#define MIDI_H

#include "queue.h"

#define MIDI_MAX_DEVS 32

int         midi_available(void);              /* compilado com PortMidi? */
int         midi_refresh(void);                /* relê a lista; retorna quantas entradas */
const char *midi_device_name(int i);
int         midi_open(int i, EventQueue *q);   /* fecha o anterior; 0 = ok */
void        midi_close(void);
int         midi_is_open(void);
const char *midi_open_name(void);

/* Saída MIDI (a lista é relida junto com a de entradas em midi_refresh) */
int         midi_out_count(void);
const char *midi_out_name(int i);
int         midi_virtual_supported(void);         /* Linux (ALSA) e macOS sim; Windows não */
int         midi_out_open(int i, EventQueue *q);  /* i = -1: porta virtual "Sintetizador" */
void        midi_out_close(void);
int         midi_out_is_open(void);
const char *midi_out_open_name(void);
void        midi_out_test(void);                  /* toca um Dó de teste */
int         midi_out_activity(void);              /* contador de mensagens enviadas */

/* MIDI learn */
int  midi_cc_param(int cc);                    /* parâmetro ligado ao CC, ou -1 */
int  midi_param_cc(int param);                 /* CC ligado ao parâmetro, ou -1 */
void midi_map(int cc, int param);              /* param = -1 desfaz */
int  midi_last_cc(int *counter);               /* último CC recebido (+contador) */
int  midi_map_save(const char *path);
int  midi_map_load(const char *path);

#endif
