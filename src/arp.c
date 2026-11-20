#include "arp.h"

#include <stdlib.h>
#include <string.h>

#include "params.h"

void arp_init(Arp *a)
{
    memset(a, 0, sizeof *a);
    a->playing = -1;
    a->rng     = 0x1234567u;
}

static void list_remove(Arp *a, int note)
{
    for (int i = 0; i < a->count; i++) {
        if (a->order[i] == note) {
            memmove(&a->order[i], &a->order[i + 1], sizeof(int) * (a->count - i - 1));
            memmove(&a->vel[i], &a->vel[i + 1], sizeof(int) * (a->count - i - 1));
            a->count--;
            return;
        }
    }
}

void arp_note_on(Arp *a, int note, int vel)
{
    int hold = param_geti(P_ARP_HOLD);
    /* "Segurar": um acorde novo (depois de soltar tudo) substitui o anterior */
    if (hold && a->down_count == 0) a->count = 0;
    if (!a->down[note]) { a->down[note] = 1; a->down_count++; }
    list_remove(a, note);
    if (a->count < ARP_MAX_NOTES) {
        a->order[a->count] = note;
        a->vel[a->count]   = vel;
        a->count++;
    }
    if (a->count == 1) a->samples_to_step = 0; /* começa imediatamente */
}

void arp_note_off(Arp *a, int note)
{
    if (a->down[note]) { a->down[note] = 0; a->down_count--; }
    if (!param_geti(P_ARP_HOLD)) list_remove(a, note);
}

static void send(NoteSink sink, void *user, EventType type, int note, int vel)
{
    Event e = {(uint8_t)type, SRC_ARP, (uint8_t)note, (uint8_t)vel, (float)note};
    sink(user, &e);
}

void arp_clear(Arp *a, NoteSink sink, void *user)
{
    if (a->playing >= 0) send(sink, user, EV_NOTE_OFF, a->playing, 0);
    int hold_prev = a->hold_prev;
    arp_init(a);
    a->hold_prev = hold_prev;
}

static int sorted_cmp(const void *x, const void *y) { return *(const int *)x - *(const int *)y; }

/* Monta a sequência do passo atual e devolve a nota (e a velocity) */
static int pick_note(Arp *a, int *vel_out)
{
    int notes[ARP_MAX_NOTES], vels[ARP_MAX_NOTES], n = a->count;
    memcpy(notes, a->order, sizeof(int) * n);
    memcpy(vels, a->vel, sizeof(int) * n);
    int mode = param_geti(P_ARP_MODE);
    if (mode != 4) qsort(notes, n, sizeof(int), sorted_cmp);

    int octaves = param_geti(P_ARP_OCT);
    int len     = n * octaves;
    int idx;
    switch (mode) {
    case 1: idx = len - 1 - (a->step % len); break;                     /* desce */
    case 2: {                                                           /* sobe e desce */
        int cycle = len > 1 ? 2 * len - 2 : 1;
        int p = a->step % cycle;
        idx = p < len ? p : cycle - p;
        break;
    }
    case 3: a->rng = a->rng * 1664525u + 1013904223u; idx = (int)((a->rng >> 8) % (unsigned)len); break;
    default: idx = a->step % len; break;                                /* sobe / como tocado */
    }
    int note = notes[idx % n] + 12 * (idx / n);
    *vel_out = vels[0];
    for (int i = 0; i < n; i++) if (a->order[i] == notes[idx % n]) *vel_out = a->vel[i];
    return note > 127 ? 127 : note;
}

void arp_process(Arp *a, NoteSink sink, void *user, int frames, float sr, float bpm)
{
    /* Desligou "Segurar": tira da lista o que não está mais pressionado */
    int hold = param_geti(P_ARP_HOLD);
    if (a->hold_prev && !hold) {
        for (int i = a->count - 1; i >= 0; i--)
            if (!a->down[a->order[i]]) list_remove(a, a->order[i]);
    }
    a->hold_prev = hold;

    static const float division[5] = {1.0f, 0.5f, 1.0f / 3.0f, 0.25f, 0.125f};
    double period = 60.0 / bpm * division[param_geti(P_ARP_RATE)] * sr;
    double gate   = period * param_get(P_ARP_GATE);

    /* Confere o que já venceu e só depois desconta o tempo deste bloco */
    if (a->playing >= 0 && a->samples_to_off <= 0) {
        send(sink, user, EV_NOTE_OFF, a->playing, 0);
        a->playing = -1;
    }
    if (a->count == 0) {
        a->step = 0;
        a->samples_to_step = 0; /* próxima nota começa na hora */
    } else if (a->samples_to_step <= 0) {
        if (a->playing >= 0) send(sink, user, EV_NOTE_OFF, a->playing, 0);
        int vel;
        a->playing = pick_note(a, &vel);
        send(sink, user, EV_NOTE_ON, a->playing, vel);
        a->samples_to_off   = gate;
        a->samples_to_step += period;
        if (a->samples_to_step <= 0) a->samples_to_step = period;
        a->step++;
    }
    if (a->count > 0) a->samples_to_step -= frames;
    if (a->playing >= 0) a->samples_to_off -= frames;
}
