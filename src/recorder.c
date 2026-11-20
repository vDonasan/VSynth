#include "recorder.h"

#include <stdint.h>
#include <string.h>

static void put32(FILE *f, uint32_t v) { unsigned char b[4] = {v, v >> 8, v >> 16, v >> 24}; fwrite(b, 1, 4, f); }
static void put16(FILE *f, uint16_t v) { unsigned char b[2] = {v, v >> 8}; fwrite(b, 1, 2, f); }

static void write_header(FILE *f, int sr, uint32_t frames)
{
    uint32_t data = frames * 4;
    fwrite("RIFF", 1, 4, f); put32(f, 36 + data); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, 2); put32(f, (uint32_t)sr); put32(f, (uint32_t)sr * 4);
    put16(f, 4); put16(f, 16); fwrite("data", 1, 4, f); put32(f, data);
}

int recorder_start(Recorder *r, const char *path, int sr)
{
    memset(r, 0, sizeof *r);
    r->f = fopen(path, "wb");
    if (!r->f) return -1;
    r->sr = sr;
    snprintf(r->path, sizeof r->path, "%s", path);
    write_header(r->f, sr, 0);
    return 0;
}

void recorder_write(Recorder *r, const float *x, unsigned frames)
{
    if (!r->f) return;
    for (unsigned i = 0; i < frames * 2; i++) {
        float v = x[i] > 1 ? 1 : (x[i] < -1 ? -1 : x[i]);
        put16(r->f, (uint16_t)(int16_t)(v * 32767.0f));
    }
    r->frames += frames;
}

void recorder_stop(Recorder *r)
{
    if (!r->f) return;
    fseek(r->f, 0, SEEK_SET);
    write_header(r->f, r->sr, r->frames);
    fclose(r->f);
    r->f = NULL;
}

int recorder_active(const Recorder *r) { return r->f != NULL; }

int wav_write_stereo(const char *path, const float *x, unsigned frames, int sr)
{
    Recorder r;
    if (recorder_start(&r, path, sr) < 0) return -1;
    recorder_write(&r, x, frames);
    recorder_stop(&r);
    return 0;
}
