/*
 * Filas sem trava (lock-free) de um produtor e um consumidor (SPSC).
 *
 * O callback de áudio não pode usar mutex, malloc nem printf: se ele
 * esperar, o som falha (estalos). Por isso cada fonte de entrada
 * (MIDI, teclado, guitarra) tem a sua própria fila, e o callback só
 * esvazia as filas no começo de cada buffer.
 */
#ifndef QUEUE_H
#define QUEUE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    EV_NOTE_ON,     /* note, velocity; value = nota fracionária (guitarra) */
    EV_NOTE_OFF,    /* note */
    EV_PITCH_BEND,  /* value = semitons (-2..+2) */
    EV_CC,          /* note = nº do controlador, velocity = valor 0-127 */
    EV_PITCH_TRACK, /* value = nota fracionária seguida continuamente */
    EV_ALL_OFF      /* solta todas as notas */
} EventType;

typedef enum { SRC_MIDI, SRC_KEYS, SRC_AUDIO, SRC_ARP, SRC_HARM } EventSource;

typedef struct {
    uint8_t type;
    uint8_t source;
    uint8_t note;
    uint8_t velocity;
    float   value;
} Event;

/* ---------- fila de eventos ---------- */
#define EVQ_SIZE 1024 /* potência de 2 */

typedef struct {
    Event buf[EVQ_SIZE];
    atomic_uint head; /* escrito pelo produtor */
    atomic_uint tail; /* escrito pelo consumidor */
} EventQueue;

static inline void evq_init(EventQueue *q)
{
    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);
}

static inline bool evq_push(EventQueue *q, Event e)
{
    unsigned h = atomic_load_explicit(&q->head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (h - t >= EVQ_SIZE) return false; /* cheia: descarta */
    q->buf[h & (EVQ_SIZE - 1)] = e;
    atomic_store_explicit(&q->head, h + 1, memory_order_release);
    return true;
}

static inline bool evq_pop(EventQueue *q, Event *e)
{
    unsigned t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&q->head, memory_order_acquire);
    if (t == h) return false;
    *e = q->buf[t & (EVQ_SIZE - 1)];
    atomic_store_explicit(&q->tail, t + 1, memory_order_release);
    return true;
}

/* ---------- fila de amostras (capacidade definida na criação) ---------- */
#include <stdlib.h>

typedef struct {
    float      *buf;
    unsigned    mask;
    atomic_uint head;
    atomic_uint tail;
} FloatRing;

/* capacity precisa ser potência de 2 */
static inline int fring_init(FloatRing *r, unsigned capacity)
{
    r->buf  = calloc(capacity, sizeof(float));
    r->mask = capacity - 1;
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    return r->buf ? 0 : -1;
}

static inline void fring_free(FloatRing *r) { free(r->buf); r->buf = NULL; }

static inline void fring_clear(FloatRing *r)
{
    atomic_store(&r->tail, atomic_load(&r->head));
}

static inline void fring_push(FloatRing *r, float x)
{
    unsigned h = atomic_load_explicit(&r->head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (h - t > r->mask) return; /* consumidor atrasado: descarta */
    r->buf[h & r->mask] = x;
    atomic_store_explicit(&r->head, h + 1, memory_order_release);
}

static inline unsigned fring_available(FloatRing *r)
{
    return atomic_load_explicit(&r->head, memory_order_acquire) -
           atomic_load_explicit(&r->tail, memory_order_relaxed);
}

static inline unsigned fring_read(FloatRing *r, float *dst, unsigned n)
{
    unsigned t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&r->head, memory_order_acquire);
    unsigned avail = h - t;
    if (n > avail) n = avail;
    for (unsigned i = 0; i < n; i++) dst[i] = r->buf[(t + i) & r->mask];
    atomic_store_explicit(&r->tail, t + n, memory_order_release);
    return n;
}

#endif
