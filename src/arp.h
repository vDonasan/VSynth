/*
 * Arpejador: recebe as notas seguradas (de qualquer entrada) e as toca
 * uma de cada vez, no ritmo do BPM.
 */
#ifndef ARP_H
#define ARP_H

#include "synth.h"

#define ARP_MAX_NOTES 32

/* Para onde vão as notas geradas (synth, saída MIDI...) */
typedef void (*NoteSink)(void *user, const Event *e);

typedef struct {
    int      order[ARP_MAX_NOTES];   /* notas na ordem em que foram tocadas */
    int      vel[ARP_MAX_NOTES];
    int      count;
    unsigned char down[128];         /* teclas fisicamente pressionadas */
    int      down_count;
    double   samples_to_step;        /* até o próximo passo */
    double   samples_to_off;         /* até soltar a nota atual */
    int      step;
    int      playing;                /* nota soando (-1 = nenhuma) */
    unsigned rng;
    int      hold_prev;
} Arp;

void arp_init(Arp *a);
void arp_note_on(Arp *a, int note, int vel);
void arp_note_off(Arp *a, int note);
void arp_clear(Arp *a, NoteSink sink, void *user);
/* Avança `frames` amostras, mandando note on/off para `sink`. */
void arp_process(Arp *a, NoteSink sink, void *user, int frames, float sr, float bpm);

#endif
