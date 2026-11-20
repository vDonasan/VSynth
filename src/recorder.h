/* Grava a saída em WAV (16 bits, estéreo). Roda na thread da interface. */
#ifndef RECORDER_H
#define RECORDER_H

#include <stdio.h>

typedef struct {
    FILE    *f;
    unsigned frames;
    int      sr;
    char     path[512];
} Recorder;

int  recorder_start(Recorder *r, const char *path, int sr);
void recorder_write(Recorder *r, const float *interleaved, unsigned frames);
void recorder_stop(Recorder *r); /* corrige o cabeçalho e fecha */
int  recorder_active(const Recorder *r);

/* Escreve um WAV inteiro de uma vez (usado pelos testes). */
int  wav_write_stereo(const char *path, const float *interleaved, unsigned frames, int sr);

#endif
