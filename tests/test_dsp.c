/*
 * Testes sem placa de som: simula uma guitarra (Karplus-Strong), passa
 * pelo mesmo detector usado em tempo real e confere as notas. No fim,
 * testa efeitos, arpejador, looper e presets e gera demo.wav com o motor
 * completo (pad, arpejo e a "guitarra" simulada controlando o synth).
 *
 *   cmake --build build && ./build/test_dsp
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

#include "arp.h"
#include "engine.h"
#include "fx.h"
#include "guitar.h"
#include "looper.h"
#include "params.h"
#include "recorder.h"
#include "synth.h"

#define SR SAMPLE_RATE

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FALHOU: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned rng = 12345;
static float frand(void) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 8388608.0f - 1.0f; }

/* Corda dedilhada: ruído numa linha de atraso + média de 2 amostras */
typedef struct { float *buf; int len, pos; float decay; } Pluck;

static void pluck_init(Pluck *p, float hz, float amp)
{
    /* y[t+len] = (y[t] + y[t+1]) / 2  ->  período real = len - 0,5 amostra */
    p->len   = (int)lroundf(SR / hz + 0.5f);
    p->buf   = malloc(sizeof(float) * p->len);
    p->pos   = 0;
    p->decay = 0.996f;
    float mean = 0;
    for (int i = 0; i < p->len; i++) { p->buf[i] = frand() * amp; mean += p->buf[i]; }
    mean /= p->len; /* corda de verdade não tem nível contínuo (DC) */
    for (int i = 0; i < p->len; i++) p->buf[i] -= mean;
}

static float pluck_next(Pluck *p)
{
    int   nx  = (p->pos + 1) % p->len;
    float out = p->buf[p->pos];
    p->buf[p->pos] = p->decay * 0.5f * (p->buf[p->pos] + p->buf[nx]);
    p->pos = nx;
    return out;
}

static float midi_hz(int n) { return 440.0f * powf(2.0f, (n - 69) / 12.0f); }

/* Uma "partitura" para a guitarra simulada */
typedef struct { int note; float dur; float amp; } Pick; /* note < 0 = pausa */

static float *render_guitar(const Pick *picks, int count, int *out_len)
{
    float total = 0;
    for (int i = 0; i < count; i++) total += picks[i].dur;
    int    len = (int)(total * SR) + PITCH_HOP;
    float *x   = calloc(len, sizeof(float));
    int    pos = 0;
    for (int i = 0; i < count; i++) {
        int n = (int)(picks[i].dur * SR);
        if (picks[i].note >= 0) {
            Pluck p;
            pluck_init(&p, midi_hz(picks[i].note), picks[i].amp);
            for (int j = 0; j < n; j++) x[pos + j] = pluck_next(&p) + frand() * 0.0005f;
            free(p.buf);
        } else {
            for (int j = 0; j < n; j++) x[pos + j] = frand() * 0.0005f; /* chiado do amplificador */
        }
        pos += n;
    }
    *out_len = len;
    return x;
}

typedef struct { Event ev; int sample; } Timed;

/* Passa o sinal pelo detector e devolve os eventos de nota com o instante */
static int detect(const float *x, int len, Timed *out, int max)
{
    static Guitar g;
    static EventQueue q;
    evq_init(&q);
    if (guitar_init(&g, SR, &q) < 0) { printf("init falhou\n"); exit(1); }
    int n = 0;
    for (int pos = 0; pos + PITCH_HOP <= len; pos += PITCH_HOP) {
        guitar_process_hop(&g, x + pos);
        Event e;
        while (evq_pop(&q, &e))
            if (e.type != EV_PITCH_TRACK && e.type != EV_ONSET && n < max) out[n++] = (Timed){e, pos + PITCH_HOP};
    }
    guitar_free(&g);
    return n;
}

static void test_open_strings(void)
{
    printf("Teste 1: cada nota isolada é reconhecida\n");
    const int notes[] = {38, 40, 45, 50, 55, 59, 64, 69, 76, 81}; /* 38 = Ré grave (drop-D) */
    for (unsigned i = 0; i < sizeof notes / sizeof notes[0]; i++) {
        Pick  p[] = {{-1, 0.1f, 0}, {notes[i], 1.2f, 0.4f}, {-1, 0.5f, 0}};
        int   len;
        float *x = render_guitar(p, 3, &len);
        Timed ev[256];
        int   n = detect(x, len, ev, 256);
        int   ons = 0, first = -1, latency = -1, offs = 0;
        for (int k = 0; k < n; k++) {
            if (ev[k].ev.type == EV_NOTE_ON) {
                if (first < 0) { first = ev[k].ev.note; latency = ev[k].sample - (int)(0.1f * SR); }
                ons++;
            }
            if (ev[k].ev.type == EV_NOTE_OFF) offs++;
        }
        char nm[8];
        printf("  %-4s %7.1f Hz -> ", note_name(notes[i], nm, sizeof nm), midi_hz(notes[i]));
        if (first >= 0) printf("%-4s latência %4.1f ms, note-ons %d, note-offs %d\n",
                               note_name(first, nm, sizeof nm), latency * 1000.0f / SR, ons, offs);
        else printf("nada detectado\n");
        CHECK(first == notes[i], "esperado %d, veio %d", notes[i], first);
        CHECK(ons == 1, "esperado 1 note-on, vieram %d", ons);
        CHECK(offs >= 1, "a nota deveria terminar no silêncio");
        free(x);
    }
}

static void test_chromatic(void)
{
    printf("Teste 1b: escala cromática E2..E6, 49 notas x 5 ruídos diferentes\n");
    int total_ok = 0, total = 0;
    unsigned saved = rng;
    for (int seed = 1; seed <= 5; seed++) {
        rng = 1000u + 7919u * seed;
        Pick p[120];
        int k = 0, expected[60], ne = 0;
        p[k++] = (Pick){-1, 0.2f, 0};
        for (int n = 40; n <= 88; n++) {
            float amp = 0.15f + 0.35f * ((n * 7 + seed) % 5) / 4.0f; /* 0,15 a 0,5 */
            p[k++] = (Pick){n, 0.25f, amp};
            p[k++] = (Pick){-1, 0.08f, 0};
            expected[ne++] = n;
        }
        int len;
        float *x = render_guitar(p, k, &len);
        static Timed ev[4096];
        int n = detect(x, len, ev, 4096), got[120], ng = 0, ok = 0;
        for (int i = 0; i < n; i++) if (ev[i].ev.type == EV_NOTE_ON && ng < 120) got[ng++] = ev[i].ev.note;
        for (int i = 0; i < ne && i < ng; i++) ok += got[i] == expected[i];
        if (ng != ne || ok != ne) printf("  ruído %d: %d corretas, %d note-ons\n", seed, ok, ng);
        total_ok += (ng == ne) ? ok : 0;
        total += ne;
        free(x);
    }
    rng = saved;
    printf("  %d de %d notas corretas\n", total_ok, total);
    CHECK(total_ok == total, "escala cromática com erros");
}

static void test_silence(void)
{
    printf("Teste 2: só chiado não gera notas\n");
    Pick  p[] = {{-1, 2.0f, 0}};
    int   len;
    float *x = render_guitar(p, 1, &len);
    Timed ev[64];
    int   n = detect(x, len, ev, 64);
    CHECK(n == 0, "vieram %d eventos no silêncio", n);
    if (n == 0) printf("  ok\n");
    free(x);
}

static const Pick riff[] = {
    {-1, 0.2f, 0},
    {40, 0.5f, 0.45f}, {43, 0.5f, 0.40f}, {45, 0.7f, 0.45f},
    {-1, 0.3f, 0},
    {52, 0.4f, 0.35f}, {55, 0.4f, 0.35f}, {57, 0.4f, 0.35f}, {55, 0.4f, 0.30f}, {52, 1.0f, 0.40f},
    {-1, 0.6f, 0},
};
#define RIFF_LEN (int)(sizeof riff / sizeof riff[0])

static void test_riff(void)
{
    printf("Teste 3: sequência de notas (riff)\n");
    int   len;
    float *x = render_guitar(riff, RIFF_LEN, &len);
    Timed ev[2048];
    int   n = detect(x, len, ev, 2048);
    int   got[32], ng = 0, last_off = 0;
    for (int k = 0; k < n; k++) {
        if (ev[k].ev.type == EV_NOTE_ON && ng < 32) got[ng++] = ev[k].ev.note;
        last_off = ev[k].ev.type == EV_NOTE_OFF;
    }
    int expected[32], ne = 0;
    for (int i = 0; i < RIFF_LEN; i++) if (riff[i].note >= 0) expected[ne++] = riff[i].note;
    printf("  esperado:");
    for (int i = 0; i < ne; i++) printf(" %d", expected[i]);
    printf("\n  detectado:");
    for (int i = 0; i < ng; i++) printf(" %d", got[i]);
    printf("\n");
    CHECK(ng == ne && !memcmp(got, expected, sizeof(int) * ne), "sequência diferente");
    CHECK(last_off, "a última nota deveria ser solta");
    free(x);
}

static void test_octave_jumps(void)
{
    printf("Teste 3b: saltos de oitava tocados de propósito\n");
    const Pick p[] = {{-1, 0.2f, 0}, {45, 0.4f, 0.4f}, {57, 0.4f, 0.4f}, {45, 0.4f, 0.4f},
                      {57, 0.4f, 0.4f}, {69, 0.4f, 0.4f}, {64, 0.3f, 0.3f}, {76, 0.4f, 0.3f}, {-1, 0.4f, 0}};
    const int expected[] = {45, 57, 45, 57, 69, 64, 76};
    int len;
    float *x = render_guitar(p, 9, &len);
    Timed ev[512];
    int n = detect(x, len, ev, 512), got[32], ng = 0;
    for (int i = 0; i < n; i++) if (ev[i].ev.type == EV_NOTE_ON && ng < 32) got[ng++] = ev[i].ev.note;
    printf("  esperado: 45 57 45 57 69 64 76\n  detectado:");
    for (int i = 0; i < ng; i++) printf(" %d", got[i]);
    printf("\n");
    CHECK(ng == 7 && !memcmp(got, expected, sizeof expected), "salto de oitava perdido");
    free(x);
}

/* Hammer-on: o som continua e só a altura muda (sem palhetada nova) */
static void test_legato(void)
{
    printf("Teste 3c: legato (hammer-on, sem palhetada)\n");
    int len = (int)(1.3f * SR);
    float *x = calloc(len, sizeof(float));
    double ph = 0;
    for (int i = 0; i < len; i++) {
        float t = (float)i / SR;
        int note = t < 0.1f ? -1 : (t < 0.5f ? 52 : (t < 0.9f ? 55 : -1));
        float a = note < 0 ? 0 : 0.25f;
        if (note >= 0) ph += midi_hz(note) / SR;
        float v = 0;
        for (int h = 1; h <= 5; h++) v += sinf((float)(6.283185307 * h * ph)) / h; /* som "de corda": vários harmônicos */
        x[i] = a * v * 0.6f + frand() * 0.0005f;
    }
    Timed ev[512];
    int n = detect(x, len, ev, 512);
    char log[128] = "";
    for (int i = 0; i < n; i++) {
        char b[16];
        snprintf(b, sizeof b, " %s%d", ev[i].ev.type == EV_NOTE_ON ? "on" : "off", ev[i].ev.note);
        strncat(log, b, sizeof log - strlen(log) - 1);
    }
    printf("  eventos:%s\n  esperado: on52 on55 off55 (sem off entre as duas: o synth desliza)\n", log);
    CHECK(!strcmp(log, " on52 on55 off55"), "legato errado");
    free(x);
}

/* Ataque imediato: guitarra simulada passando pelo motor inteiro.
   Mede quando o som começa e quando a altura fica certa. */
static float zero_cross_hz(const float *x, int n, int *first_out)
{
    int first = -1, last = -1, count = 0;
    for (int i = 1; i < n; i++)
        if (x[i - 1] <= 0 && x[i] > 0) { if (first < 0) first = i; last = i; count++; }
    if (first_out) *first_out = first;
    return count > 1 ? (count - 1) * (float)SR / (last - first) : 0;
}

static void test_fast_attack(void)
{
    printf("Teste 3d: ataque imediato (som na palhetada, nota acertada depois)\n");
    static Engine e;
    const int notes[] = {40, 45, 50, 55, 64, 76};
    for (int fast = 0; fast <= 1; fast++) {
        double on_sum = 0, ok_sum = 0;
        int all_ok = 1;
        for (unsigned k = 0; k < sizeof notes / sizeof notes[0]; k++) {
            engine_init(&e, SR);
            params_init();
            param_set(P_INPUT_MODE, 2); param_set(P_GTR_BLEND, 0); param_set(P_REV_ON, 0);
            param_set(P_OSC1_WAVE, 0); param_set(P_OSC_MIX, 0); param_set(P_CUTOFF, 18000);
            param_set(P_GTR_FAST, (float)fast);
            params_smooth_snap();
            Pick p[] = {{-1, 0.1f, 0}, {notes[k], 0.6f, 0.4f}, {-1, 0.2f, 0}};
            int len;
            float *x = render_guitar(p, 3, &len);
            float *out = calloc(2 * len, sizeof(float)), *mono = calloc(len, sizeof(float));
            for (int pos = 0; pos + 64 <= len; pos += 64) engine_process(&e, x + pos, 1, out + 2 * pos, 64);
            for (int i = 0; i < len; i++) mono[i] = out[2 * i];
            int start = (int)(0.1f * SR), on = -1, ok = -1;
            for (int i = start; i < len; i++) if (fabsf(mono[i]) > 0.01f) { on = i; break; }
            /* altura certa: a partir de quando 3 ciclos seguidos da saída
               ficam a menos de 1/4 de semitom da nota tocada */
            float want = midi_hz(notes[k]);
            int win = (int)(3 * SR / want) + 2;
            for (int i = on > 0 ? on : start; i + 3 * win < start + (int)(0.5f * SR); i += 24) {
                int good = 1, first = 0; /* e continua certa nas 3 janelas seguintes */
                for (int r = 0; r < 3 && good; r++) {
                    int f;
                    float hz = zero_cross_hz(mono + i + r * win, win, &f);
                    if (r == 0) first = f;
                    good = hz > 0 && fabsf(12 * log2f(hz / want)) < 0.25f;
                }
                if (good) { ok = i + first; break; } /* conta do primeiro ciclo medido */
            }
            if (getenv("DBG3D")) printf("    nota %d: som %.1f ms, certa %.1f ms\n", notes[k], (on - start) * 1000.0 / SR, (ok - start) * 1000.0 / SR);
            if (on < 0 || ok < 0) all_ok = 0;
            else { on_sum += (on - start) * 1000.0 / SR; ok_sum += (ok - start) * 1000.0 / SR; }
            free(x); free(out); free(mono);
            engine_free(&e);
        }
        int cnt = sizeof notes / sizeof notes[0];
        printf("  ataque imediato %s: som em %.1f ms, nota certa em %.1f ms (médias)\n",
               fast ? "ligado " : "desligado", on_sum / cnt, ok_sum / cnt);
        CHECK(all_ok, "alguma nota não soou ou não chegou na altura certa");
        if (fast) CHECK(on_sum / cnt < 6.0, "o som deveria começar logo na palhetada");
    }
    params_init();
}

/* Batidas nas cordas abafadas (ruído, sem nota) com o ataque imediato
   ligado: o som provisório tem que sumir sozinho, sem ficar preso */
static void test_fast_attack_noise(void)
{
    printf("Teste 3e: batidas sem nota não deixam som preso\n");
    static Engine e;
    int len = (int)(3.2f * SR);
    float *x = calloc(len, sizeof(float)), *out = calloc(2 * len, sizeof(float));
    for (int b = 0; b < 10; b++) {
        int s0 = (int)(0.1f * SR) + b * (int)(0.3f * SR);
        for (int i = 0; i < (int)(0.02f * SR); i++) x[s0 + i] = frand() * 0.3f * expf(-i / 300.0f);
    }
    for (int i = 0; i < len; i++) x[i] += frand() * 0.0005f;
    float tails[2][10];
    for (int fast = 0; fast <= 1; fast++) {
        engine_init(&e, SR);
        params_init();
        param_set(P_INPUT_MODE, 2); param_set(P_GTR_BLEND, 0); param_set(P_REV_ON, 0);
        param_set(P_GTR_FAST, (float)fast);
        params_smooth_snap();
        memset(out, 0, 2 * len * sizeof(float));
        for (int pos = 0; pos + 64 <= len; pos += 64) engine_process(&e, x + pos, 1, out + 2 * pos, 64);
        for (int b = 0; b < 10; b++) {
            int s0 = (int)(0.1f * SR) + b * (int)(0.3f * SR);
            float t = 0;
            for (int i = s0 + (int)(0.1f * SR); i < s0 + (int)(0.29f * SR); i++) t = fmaxf(t, fabsf(out[2 * i]));
            tails[fast][b] = t;
        }
        engine_free(&e);
    }
    /* onde o modo normal ficou em silêncio, o ataque imediato também tem que ficar
       (às vezes uma batida vira nota de verdade nos dois modos: aí não conta) */
    int quiet = 0, stuck = 0;
    for (int b = 0; b < 10; b++)
        if (tails[0][b] < 0.001f) { quiet++; if (tails[1][b] >= 0.001f) stuck++; }
    printf("  %d batidas sem nota: %d deixaram som com o ataque imediato\n", quiet, stuck);
    CHECK(quiet >= 5 && stuck == 0, "ficou som preso depois de uma batida sem nota");
    free(x); free(out);
    params_init();
}


/* ------------------------------------------------------------------ */
/* Guitarra "suja", mais parecida com uma de verdade                   */
/* ------------------------------------------------------------------ */
/* Guitarra simulada "suja": harmônicos inarmônicos, posição da palheta,
   cada harmônico decai num ritmo, a corda começa um pouco aguda e assenta,
   ruído de palhetada, cordas soltas vibrando por simpatia, zumbido de 60 Hz
   e ruído. Acordes: cordas dedilhadas com alguns ms entre si. */
static unsigned rg_seed = 12345;
static float rg_rand(void) { rg_seed = rg_seed * 1664525u + 1013904223u; return ((rg_seed >> 9) / 4194304.0f) - 1.0f; }
static float rg_hz(int n) { return 440.0f * powf(2.0f, (n - 69) / 12.0f); }
/* soma uma corda em x[start...] */
static void rg_string(float *x, int len, int start, int note, float amp, float dur_s, float sr, float pick_pos)
{
    float f0 = rg_hz(note), B = 0.00012f;
    int n = (int)(dur_s * sr);
    if (start + n > len) n = len - start;
    for (int k = 1; k <= 30; k++) {
        float fk = k * f0 * sqrtf(1 + B * k * k);
        if (fk > sr * 0.45f) break;
        float a = amp * fabsf(sinf(k * 3.14159f * pick_pos)) / (k * 0.9f);
        float decay = 1.2f + 0.35f * k + f0 / 300.0f; /* agudos e harmônicos altos morrem antes */
        float ph = rg_rand() * 3.14159f;
        for (int i = 0; i < n; i++) {
            float t = i / sr;
            float bend = 1.0f + 0.006f * expf(-t / 0.08f); /* ataque ~10 cents agudo, assenta */
            float env = expf(-decay * t) * (i < 48 ? i / 48.0f : 1.0f);
            int left = n - i, fade = (int)(0.015f * sr); /* abafada pela mão: some em 15 ms */
            if (left < fade) env *= (float)left / fade;
            x[start + i] += a * env * sinf(ph + 6.2831853f * fk * bend * t);
        }
    }
    /* ruído da palhetada: 4 ms */
    for (int i = 0; i < (int)(0.004f * sr) && start + i < len; i++) x[start + i] += amp * 0.5f * rg_rand() * (1 - i / (0.004f * sr));
}
static void rg_background(float *x, int len, float sr, float noise, float sympathetic)
{
    /* cordas soltas que ficam vibrando baixinho (Mi, Lá, Ré graves) + zumbido */
    const int open[3] = {40, 45, 50};
    for (int s = 0; s < 3; s++) {
        float f = rg_hz(open[s]);
        for (int i = 0; i < len; i++) x[i] += sympathetic * sinf(6.2831853f * f * i / sr + s) * (0.6f + 0.4f * sinf(i / sr * 0.7f + s));
    }
    for (int i = 0; i < len; i++) x[i] += noise * rg_rand() + 0.002f * sinf(6.2831853f * 60 * i / sr);
}

/* Roda o detector e devolve as notas reconhecidas em texto (" 52 54 55") */
static int real_notes(const float *x, int len, char *seq, int cap)
{
    Guitar g; EventQueue q; evq_init(&q);
    guitar_init(&g, SR, &q); guitar_reset(&g);
    int ons = 0; seq[0] = 0;
    for (int pos = 0; pos + PITCH_HOP <= len; pos += PITCH_HOP) {
        guitar_process_hop(&g, x + pos);
        Event e;
        while (evq_pop(&q, &e)) if (e.type == EV_NOTE_ON) {
            char t[8]; snprintf(t, sizeof t, " %d", e.note);
            if ((int)(strlen(seq) + strlen(t)) < cap) strcat(seq, t);
            ons++;
        }
    }
    guitar_free(&g);
    return ons;
}

static void real_case(const char *label, const float *x, int len, const char *expected)
{
    char seq[400];
    real_notes(x, len, seq, sizeof seq);
    int ok = !strcmp(seq, expected);
    printf("  %-44s %s  notas:%s\n", label, ok ? "ok  " : "ERRO", seq);
    CHECK(ok, "notas diferentes do esperado");
}

static void test_real_guitar(void)
{
    printf("Teste 1c: guitarra realista (harmônicos desafinados, cordas vibrando por simpatia, zumbido)\n");
    params_init();
    rg_seed = 12345;
    int len = SR * 9;
    float *x = calloc(len, sizeof(float));
    char ex[400] = "";
    const int notes[] = {40, 43, 45, 47, 50, 52, 55, 57, 59, 62, 64, 67, 69, 72, 76, 79};
    for (int i = 0; i < 16; i++) {
        rg_string(x, len, (int)((0.2f + i * 0.5f) * SR), notes[i], 0.25f, 0.48f, SR, 0.13f + 0.05f * (i % 3));
        char t[8]; snprintf(t, sizeof t, " %d", notes[i]); strcat(ex, t);
    }
    rg_background(x, len, SR, 0.002f, 0.004f);
    real_case("16 notas soltas, uma por palhetada", x, len, ex);

    for (int note = 40; note <= 64; note += 12) {
        memset(x, 0, len * sizeof(float));
        ex[0] = 0;
        for (int k = 0; k < 6; k++) {
            rg_string(x, len, (int)((0.1f + k * 0.15f) * SR), note, 0.22f, k == 5 ? 0.5f : 0.165f, SR, 0.15f);
            char t[8]; snprintf(t, sizeof t, " %d", note); strcat(ex, t);
        }
        rg_background(x, len, SR, 0.002f, 0.002f);
        char lab[64]; snprintf(lab, sizeof lab, "nota %d palhetada 6x seguidas", note);
        real_case(lab, x, len, ex);
    }

    memset(x, 0, len * sizeof(float));
    const int alt[8] = {52, 55, 52, 55, 57, 55, 52, 50};
    ex[0] = 0;
    for (int k = 0; k < 8; k++) {
        rg_string(x, len, (int)((0.1f + k * 0.12f) * SR), alt[k], 0.22f, 0.12f, SR, 0.15f);
        char t[8]; snprintf(t, sizeof t, " %d", alt[k]); strcat(ex, t);
    }
    rg_background(x, len, SR, 0.002f, 0.002f);
    real_case("frase rápida (120 ms por nota)", x, len, ex);

    memset(x, 0, len * sizeof(float));
    {
        float f0 = rg_hz(57), ph = 0;
        int s0 = (int)(0.1f * SR);
        for (int i = 0; i < SR * 2; i++) {
            float t = i / (float)SR;
            float f = f0 * powf(2, 0.3f / 12 * sinf(6.2831853f * 5.5f * t) * fminf(1, t / 0.3f));
            ph += 6.2831853f * f / SR;
            x[s0 + i] = 0.2f * expf(-1.2f * t) * (sinf(ph) + 0.5f * sinf(2 * ph) + 0.3f * sinf(3 * ph) + 0.2f * sinf(4 * ph));
        }
        for (int i = 0; i < 200; i++) x[s0 + i] += 0.1f * rg_rand() * (1 - i / 200.0f);
    }
    rg_background(x, len, SR, 0.002f, 0.002f);
    real_case("vibrato largo (+-30 cents) e corda abafada", x, len, " 57");

    memset(x, 0, len * sizeof(float));
    {
        float ph = 0;
        int s0 = (int)(0.1f * SR);
        const int hn[3] = {52, 54, 55};
        for (int i = 0; i < (int)(1.2f * SR); i++) {
            float t = i / (float)SR;
            int k = t < 0.3f ? 0 : (t < 0.6f ? 1 : 2);
            ph += 6.2831853f * rg_hz(hn[k]) / SR;
            x[s0 + i] = 0.2f * expf(-1.0f * t) * (sinf(ph) + 0.6f * sinf(2 * ph) + 0.3f * sinf(3 * ph));
        }
        for (int i = 0; i < 200; i++) x[s0 + i] += 0.1f * rg_rand() * (1 - i / 200.0f);
    }
    rg_background(x, len, SR, 0.002f, 0.002f);
    real_case("hammer-on (52 -> 54 -> 55 sem palhetar)", x, len, " 52 54 55");

    /* zumbido forte acima do gate (gate baixo): sem ataque, não vira nota */
    memset(x, 0, len * sizeof(float));
    for (int i = 0; i < SR * 2; i++) {
        float t = i / (float)SR;
        x[i] = 0.006f * (sinf(6.2831853f * 60 * t) + 0.6f * sinf(6.2831853f * 120 * t + 1) + 0.4f * sinf(6.2831853f * 180 * t + 2)) +
               0.001f * rg_rand();
    }
    param_set(P_GTR_GATE, -66);
    real_case("zumbido forte, gate baixo: nenhuma nota", x, len, "");

    /* gate alto: a nota não pode cortar cedo enquanto ainda soa */
    {
        memset(x, 0, len * sizeof(float));
        rg_string(x, len, (int)(0.1f * SR), 52, 0.05f, 1.0f, SR, 0.15f);
        rg_background(x, len, SR, 0.0008f, 0.0006f);
        param_set(P_GTR_GATE, -30);
        Guitar g; EventQueue q; evq_init(&q);
        guitar_init(&g, SR, &q); guitar_reset(&g);
        int on_at = -1, off_at = -1;
        for (int pos = 0; pos + PITCH_HOP <= SR * 2; pos += PITCH_HOP) {
            guitar_process_hop(&g, x + pos);
            Event e;
            while (evq_pop(&q, &e)) {
                if (e.type == EV_NOTE_ON && on_at < 0) on_at = pos;
                if (e.type == EV_NOTE_OFF && on_at >= 0 && off_at < 0) off_at = pos;
            }
        }
        guitar_free(&g);
        float held = (off_at - (int)(0.1f * SR)) / (float)SR;
        printf("  %-44s %s  soou %.2f s de 1,00 s\n", "gate alto: nota não corta cedo", on_at >= 0 && held > 0.85f ? "ok  " : "ERRO", held);
        CHECK(on_at >= 0 && held > 0.85f, "nota cortada cedo com gate alto");
        params_init();
    }

    /* acordes: o detector toca uma nota só; ela não pode ficar trocando */
    memset(x, 0, len * sizeof(float));
    const int chords[6][6] = {{40, 47, 52, 56, 59, 64}, {45, 52, 57, 61, 64, -1}, {50, 57, 62, 66, -1, -1},
                              {43, 47, 50, 55, 59, 67}, {48, 52, 55, 60, 64, -1}, {40, 47, 52, 55, 59, 64}};
    for (int c = 0; c < 6; c++)
        for (int k = 0; k < 6 && chords[c][k] >= 0; k++)
            rg_string(x, len, (int)((0.2f + c * 1.2f) * SR) + k * (int)(0.012f * SR), chords[c][k], 0.15f, 1.15f, SR, 0.15f);
    rg_background(x, len, SR, 0.002f, 0.002f);
    char seq[400];
    int ons = real_notes(x, len, seq, sizeof seq);
    printf("  %-44s %s  notas:%s\n", "6 acordes dedilhados: no máximo 7 notas", ons <= 7 ? "ok  " : "ERRO", seq);
    CHECK(ons <= 7, "acordes trocando de nota");
    free(x);
    params_init();
}

/* Modo direto: o synth segue a onda da corda (direct.c) */
static void test_direct_mode(void)
{
    printf("Teste 1d: modo direto (synth segue a onda da corda)\n");
    static Engine e;
    int len = SR * 12;
    float *x = calloc(len, sizeof(float)), *out = calloc(2 * len, sizeof(float)), *mono = calloc(len, sizeof(float));
    const int notes[] = {40, 43, 45, 47, 50, 52, 55, 57, 59, 62, 64, 67, 69, 72, 76, 79};
    int starts[16];
    rg_seed = 100;
    for (int i = 0; i < 16; i++) {
        starts[i] = (int)((0.2f + i * 0.65f) * SR);
        rg_string(x, len, starts[i], notes[i], 0.25f, 0.4f, SR, 0.13f + 0.05f * (i % 3));
    }
    rg_background(x, len, SR, 0.002f, 0.012f); /* cordas ressoando forte */

    engine_init(&e, SR);
    params_init();
    param_set(P_INPUT_MODE, 2); param_set(P_GTR_DIRECT, 1); param_set(P_GTR_BLEND, 0); param_set(P_REV_ON, 0);
    param_set(P_OSC_MIX, 0); param_set(P_OSC1_WAVE, 2); param_set(P_CUTOFF, 5000); param_set(P_MOD0_AMT, 0);
    param_set(P_SUSTAIN, 1); param_set(P_RELEASE, 0.05f);
    params_smooth_snap();
    for (int pos = 0; pos + 64 <= len; pos += 64) engine_process(&e, x + pos, 1, out + 2 * pos, 64);
    for (int i = 0; i < len; i++) mono[i] = out[2 * i];
    engine_free(&e);

    /* altura ouvida a cada 1,3 ms (últimos 30 ms da saída) */
    Yin y;
    yin_init(&y, SR, 1440, 70, 2000, 0.15f);
    long wrong = 0, tot = 0;
    double sound_sum = 0;
    int sounded = 0, finite = 1;
    for (int k = 0; k < 16; k++) {
        int s0 = starts[k], sound = -1;
        for (int t = s0; t < s0 + (int)(0.05f * SR); t++) if (fabsf(mono[t]) > 0.01f) { sound = t; break; }
        if (sound >= 0) { sounded++; sound_sum += (sound - s0) * 1000.0 / SR; }
        for (int t = s0 + 64; t < s0 + (int)(0.4f * SR) - 1440; t += 64) {
            float c, hz = yin_detect(&y, mono + t - 1440 + 1, &c);
            int ok = hz > 0 && fabsf(69 + 12 * log2f(hz / 440) - notes[k]) < 0.5f;
            tot++;
            wrong += hz > 0 && c > 0.8f && !ok;
        }
    }
    for (int i = 0; i < len; i++) if (!isfinite(mono[i])) finite = 0;
    /* silêncio entre as notas: só ressonância e zumbido, sem palhetada */
    float quiet = 0;
    for (int k = 0; k < 15; k++)
        for (int t = starts[k] + (int)(0.5f * SR); t < starts[k + 1] - 200; t++) {
            if (getenv("DBG1D") && fabsf(mono[t]) > 0.01f && fabsf(mono[t]) > quiet) printf("    depois da nota %d, aos %.0f ms: %.3f\n", notes[k], (t - starts[k]) * 1000.0 / SR, fabsf(mono[t]));
            quiet = fmaxf(quiet, fabsf(mono[t]));
        }
    yin_free(&y);
    double wrong_pct = 100.0 * wrong / tot;
    printf("  som começa em %.1f ms (média, %d/16 notas) | altura errada audível %.1f%% do tempo | entre as notas: pico %.4f\n",
           sound_sum / (sounded ? sounded : 1), sounded, wrong_pct, quiet);
    CHECK(sounded == 16 && sound_sum / sounded < 3.0, "o som deveria começar logo na palhetada");
    CHECK(wrong_pct < 2.0, "altura errada audível demais no modo direto");
    CHECK(quiet < 0.01f, "ressonância sozinha não deveria fazer som");
    CHECK(finite, "saída inválida (NaN/inf)");

    /* acordes: viram fuzz, mas o som precisa sair e ser válido */
    memset(x, 0, len * sizeof(float));
    const int chord[6] = {40, 47, 52, 56, 59, 64};
    for (int k = 0; k < 6; k++) rg_string(x, len, (int)(0.1f * SR) + k * (int)(0.012f * SR), chord[k], 0.15f, 1.0f, SR, 0.15f);
    engine_init(&e, SR);
    params_init();
    param_set(P_INPUT_MODE, 2); param_set(P_GTR_DIRECT, 1); param_set(P_GTR_BLEND, 0);
    params_smooth_snap();
    float peak = 0;
    for (int pos = 0; pos + 64 <= SR * 2; pos += 64) {
        engine_process(&e, x + pos, 1, out, 64);
        for (int i = 0; i < 128; i++) { if (!isfinite(out[i])) finite = 0; peak = fmaxf(peak, fabsf(out[i])); }
    }
    engine_free(&e);
    printf("  acorde: pico %.2f, saída %s\n", peak, finite ? "válida" : "INVÁLIDA");
    CHECK(peak > 0.05f && finite, "acorde no modo direto sem som ou inválido");
    free(x); free(out); free(mono);
    params_init();
}

static void test_repick(void)
{
    printf("Teste 4: mesma nota palhetada de novo gera nova nota\n");
    Pick  p[] = {{-1, 0.2f, 0}, {50, 0.5f, 0.4f}, {50, 0.5f, 0.4f}, {50, 0.5f, 0.4f}, {-1, 0.5f, 0}};
    int   len;
    float *x = render_guitar(p, 5, &len);
    Timed ev[1024];
    int   n = detect(x, len, ev, 1024), ons = 0, offs = 0;
    for (int k = 0; k < n; k++) {
        ons  += ev[k].ev.type == EV_NOTE_ON && ev[k].ev.note == 50;
        offs += ev[k].ev.type == EV_NOTE_OFF;
    }
    printf("  note-ons %d (esperado 3), note-offs %d (esperado 3)\n", ons, offs);
    CHECK(ons == 3 && offs == 3, "redisparo incorreto");
    free(x);
}

static int finite_and_bounded(const float *x, int n, float lim)
{
    for (int i = 0; i < n; i++) if (!(x[i] == x[i]) || fabsf(x[i]) > lim) return 0;
    return 1;
}

static void test_fx(void)
{
    printf("Teste 5: efeitos estáveis (sem NaN, sem explodir) com os botões no máximo\n");
    static Fx fx;
    fx_init(&fx, SR);
    static float l[SR * 3], r[SR * 3];
    for (int t = 0; t < FX_COUNT; t++) {
        params_init();
        param_set(P_REV_ON, 0);
        param_set(fx_param_on(t), 1);
        /* pior caso: realimentação e mix no máximo */
        param_set(P_DLY_FB, 0.95f); param_set(P_DLY_MIX, 1); param_set(P_PHA_FB, 0.9f);
        param_set(P_REV_SIZE, 1); param_set(P_DIST_DRIVE, 40); param_set(P_CHO_DEPTH, 1);
        params_smooth_snap();
        fx_reset(&fx);
        for (int i = 0; i < SR * 3; i++) {
            float x = i < SR ? 0.8f * sinf(i * 0.05f) + 0.2f * frand() : 0.0f; /* som e depois silêncio */
            l[i] = r[i] = x;
        }
        for (int i = 0; i < SR * 3; i += 128) fx_process(&fx, l + i, r + i, 128, 120);
        float tail = 0;
        for (int i = SR * 3 - 4800; i < SR * 3; i++) tail = fmaxf(tail, fabsf(l[i]));
        int ok = finite_and_bounded(l, SR * 3, 8.0f) && finite_and_bounded(r, SR * 3, 8.0f);
        printf("  %-10s %s  (cauda após 2 s de silêncio: %.3f)\n", fx_name(t), ok ? "ok" : "INSTÁVEL", tail);
        CHECK(ok, "%s instável", fx_name(t));
    }
    fx_free(&fx);
    params_init();
}

static void test_smoothing(void)
{
    printf("Teste 5b: knobs suavizados (sem degraus)\n");
    params_init();
    params_smooth_snap();
    float coef = 1.0f - expf(-(float)ENGINE_CHUNK / (0.03f * SR));
    param_set(P_CUTOFF, 8000); /* salto de 900 Hz para 8 kHz de uma vez */
    float prev = param_s(P_CUTOFF), max_step = 0, at_30ms = 0;
    int steps_30ms = (int)(0.03f * SR / ENGINE_CHUNK);
    for (int i = 1; i <= 1000; i++) { /* ~0,7 s */
        params_smooth_step(coef);
        float v = param_s(P_CUTOFF);
        float oct = log2f(v / prev); /* passo em oitavas */
        if (oct > max_step) max_step = oct;
        prev = v;
        if (i == steps_30ms) at_30ms = v;
    }
    float frac = log2f(at_30ms / 900.0f) / log2f(8000.0f / 900.0f);
    printf("  maior passo entre pedaços de 32 amostras: %.3f oitava (o salto total é 3,15)\n", max_step);
    printf("  depois de 30 ms já percorreu %.0f%% do caminho; no fim: %.0f Hz\n", frac * 100, prev);
    CHECK(max_step < 0.1f, "passos grandes demais");
    CHECK(frac > 0.55f && frac < 0.75f && fabsf(prev - 8000) < 1, "tempo de suavização errado");
    params_init();
    params_smooth_snap();
}

static double time_reverb_tail(void)
{
    static Fx fx;
    fx_init(&fx, SR);
    params_init();
    param_set(P_REV_ON, 1); param_set(P_REV_SIZE, 1); param_set(P_REV_MIX, 1);
    param_set(P_DLY_ON, 1); param_set(P_DLY_FB, 0.6f);
    params_smooth_snap();
    static float l[256], r[256];
    for (int i = 0; i < 256; i++) l[i] = r[i] = (i % 32 == 0) ? 0.8f : 0.0f;
    fx_process(&fx, l, r, 256, 120);
    clock_t t0 = clock();
    for (int b = 0; b < SR * 40 / 256; b++) { /* 40 s de cauda sumindo no silêncio */
        memset(l, 0, sizeof l); memset(r, 0, sizeof r);
        fx_process(&fx, l, r, 256, 120);
    }
    double s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    fx_free(&fx);
    params_init();
    return s;
}

static void test_denormals(void)
{
    printf("Teste 5c: proteção contra denormais (cauda de reverb + delay, 40 s)\n");
#if defined(__x86_64__) || defined(_M_X64)
    _mm_setcsr(_mm_getcsr() & ~0x8040u); /* desliga FTZ/DAZ para comparar */
    double without = time_reverb_tail();
    engine_disable_denormals();
    double with = time_reverb_tail();
    printf("  sem proteção: %.2f s   com proteção: %.2f s   (%.1fx mais rápido)\n", without, with, without / with);
    CHECK(with <= without * 1.1, "a proteção deveria ajudar ou ao menos não atrapalhar");
#else
    engine_disable_denormals();
    printf("  (comparação só em x86; proteção ligada)\n");
#endif
}

static void synth_sink(void *user, const Event *e) { synth_handle_event((Synth *)user, e); }

static void test_arp(void)
{
    printf("Teste 6: arpejador no tempo certo\n");
    params_init();
    param_set(P_ARP_ON, 1); param_set(P_ARP_MODE, 0); param_set(P_ARP_RATE, 3); /* 1/16 */
    param_set(P_BPM, 120); param_set(P_ARP_OCT, 2);
    static Synth s;
    synth_init(&s, SR);
    Arp a;
    arp_init(&a);
    arp_note_on(&a, 60, 100); arp_note_on(&a, 64, 100); arp_note_on(&a, 67, 100);
    int seq[64], ns = 0, last_count = 0;
    float buf[32];
    for (int i = 0; i < SR; i += 32) { /* 1 segundo */
        arp_process(&a, synth_sink, &s, 32, SR, 120);
        synth_render(&s, buf, 32);
        if (a.step != last_count && ns < 64) { seq[ns++] = a.playing; last_count = a.step; }
    }
    /* 120 BPM, 1/16 = 8 notas por segundo; sobe 2 oitavas: 60 64 67 72 76 79 60 64 */
    const int expect[8] = {60, 64, 67, 72, 76, 79, 60, 64};
    printf("  notas em 1 s: %d (esperado 8):", ns);
    for (int i = 0; i < ns; i++) printf(" %d", seq[i]);
    printf("\n");
    CHECK(ns == 8 && !memcmp(seq, expect, sizeof expect), "sequência do arpejador errada");
    params_init();
}

static int sounding(Engine *e, int *notes, int max)
{
    int n = 0;
    for (int i = 0; i < MAX_VOICES; i++) {
        const Voice *v = &e->synth.voices[i];
        if (v->active && v->amp_env.stage != ENV_RELEASE && n < max) notes[n++] = v->note;
    }
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && notes[j - 1] > notes[j]; j--) { int t = notes[j]; notes[j] = notes[j - 1]; notes[j - 1] = t; }
    return n;
}

static void test_harmonizer(void)
{
    printf("Teste 6b: harmonizador\n");
    params_init();
    /* intervalos diatônicos em Dó maior: 3ª acima */
    param_set(P_HARM_KEY, 0); param_set(P_HARM_SCALE, 0); param_set(P_HARM_INTERVAL, 0);
    const int in[7]  = {60, 62, 64, 65, 67, 69, 71};
    const int exp3[7] = {64, 65, 67, 69, 71, 72, 74}; /* C->E, D->F, E->G, F->A, G->B, A->C, B->D */
    int ok = 1, h[2];
    for (int i = 0; i < 7; i++) { engine_harmony_notes(in[i], h); ok &= h[0] == exp3[i]; }
    printf("  3ª em Dó maior: %s\n", ok ? "ok (C->E, D->F, E->G, F->A, G->B, A->C, B->D)" : "ERRADO");
    CHECK(ok, "3ª diatônica errada");
    param_set(P_HARM_KEY, 9); param_set(P_HARM_SCALE, 1);  /* Lá menor */
    engine_harmony_notes(57, h); int a_min = h[0];          /* A -> C */
    param_set(P_HARM_INTERVAL, 5); engine_harmony_notes(60, h); int below = h[0]; /* 3ª abaixo de C em Lá menor = A */
    param_set(P_HARM_INTERVAL, 7); param_set(P_HARM_SCALE, 0); param_set(P_HARM_KEY, 0);
    int nc = engine_harmony_notes(60, h);                   /* acorde: C -> E + G */
    printf("  Lá menor: A->%d (60), 3ª abaixo de C -> %d (57); acorde de C -> %d e %d (64, 67)\n", a_min, below, h[0], h[1]);
    CHECK(a_min == 60 && below == 57 && nc == 2 && h[0] == 64 && h[1] == 67, "harmonia errada");

    /* no motor: guitarra toca E (52), harmonia de 3ª em Dó maior = G (55) */
    static Engine e;
    engine_init(&e, SR);
    params_init();
    param_set(P_HARM_ON, 1); param_set(P_HARM_INTERVAL, 0); param_set(P_HARM_SCALE, 0);
    static float out[2 * 512];
    Event on = {EV_NOTE_ON, SRC_AUDIO, 52, 100, 52.0f};
    evq_push(&e.q_gtr, on);
    engine_process(&e, NULL, 0, out, 256);
    int notes[8], n = sounding(&e, notes, 8);
    printf("  guitarra E3 -> soando:");
    for (int i = 0; i < n; i++) printf(" %d", notes[i]);
    printf(" (esperado 52 55)\n");
    CHECK(n == 2 && notes[0] == 52 && notes[1] == 55, "harmonia da guitarra");
    /* bend de meio tom: a voz de harmonia acompanha */
    Event bend = {EV_PITCH_TRACK, SRC_AUDIO, 52, 0, 52.5f};
    evq_push(&e.q_gtr, bend);
    engine_process(&e, NULL, 0, out, 256);
    float target = 0;
    for (int i = 0; i < MAX_VOICES; i++)
        if (e.synth.voices[i].active && e.synth.voices[i].source == SRC_HARM) target = e.synth.voices[i].target_freq;
    float semis = 12 * log2f(target / 440.0f) + 69;
    printf("  bend de +0,5 st na guitarra -> harmonia vai para %.2f (esperado 55,50)\n", semis);
    CHECK(fabsf(semis - 55.5f) < 0.01f, "harmonia não acompanhou o bend");
    /* legato para A (57): harmonia troca para C (60) */
    Event leg = {EV_NOTE_ON, SRC_AUDIO, 57, 100, 57.0f};
    evq_push(&e.q_gtr, leg);
    engine_process(&e, NULL, 0, out, 256);
    n = sounding(&e, notes, 8);
    printf("  legato para A3 -> soando:");
    for (int i = 0; i < n; i++) printf(" %d", notes[i]);
    printf(" (esperado 57 60)\n");
    CHECK(n == 2 && notes[0] == 57 && notes[1] == 60, "legato da harmonia");
    Event off = {EV_NOTE_OFF, SRC_AUDIO, 57, 0, 0};
    evq_push(&e.q_gtr, off);
    engine_process(&e, NULL, 0, out, 256);
    n = sounding(&e, notes, 8);
    CHECK(n == 0, "notas presas depois do note-off (%d)", n);
    printf("  note-off solta guitarra e harmonia: %s\n", n == 0 ? "ok" : "NOTAS PRESAS");

    /* teclado: só harmoniza com "Aplicar em: Tudo" */
    engine_ui_event(&e, EV_NOTE_ON, 60, 100);
    engine_process(&e, NULL, 0, out, 256);
    int only = sounding(&e, notes, 8);
    engine_ui_event(&e, EV_NOTE_OFF, 60, 0);
    param_set(P_HARM_SRC, 1);
    param_set(P_HARM_INTERVAL, 7); /* acorde */
    engine_ui_event(&e, EV_NOTE_ON, 62, 100); /* D -> F + A em Dó maior */
    engine_process(&e, NULL, 0, out, 256);
    n = sounding(&e, notes, 8);
    printf("  teclado com \"Só guitarra\": %d nota; com \"Tudo\" e acorde, D4 ->", only);
    for (int i = 0; i < n; i++) printf(" %d", notes[i]);
    printf(" (esperado 62 65 69)\n");
    CHECK(only == 1 && n == 3 && notes[0] == 62 && notes[1] == 65 && notes[2] == 69, "harmonia do teclado");
    engine_ui_event(&e, EV_NOTE_OFF, 62, 0);
    engine_process(&e, NULL, 0, out, 256);
    CHECK(sounding(&e, notes, 8) == 0, "teclado deixou harmonia presa");
    engine_free(&e);
    params_init();
}

static void test_midi_out(void)
{
    printf("Teste 6c: guitarra -> MIDI (o que vai para a DAW)\n");
    static Engine e;
    engine_init(&e, SR);
    params_init();
    param_set(P_MOUT_SRC, 1);
    param_set(P_HARM_ON, 1); /* harmonia e arpejo NÃO saem no modo "Guitarra" */
    param_set(P_ARP_ON, 1);
    static float out[2 * 256];
    Event in[] = {
        {EV_NOTE_ON, SRC_AUDIO, 52, 90, 52.1f},
        {EV_PITCH_TRACK, SRC_AUDIO, 52, 0, 52.8f},
        {EV_NOTE_ON, SRC_AUDIO, 55, 80, 55.0f}, /* legato: precisa de note-off do 52 */
        {EV_NOTE_OFF, SRC_AUDIO, 55, 0, 0},
    };
    char log[256] = "";
    Event ev;
    engine_process(&e, NULL, 0, out, 256);
    while (evq_pop(&e.q_mout, &ev)) {} /* ignora o "solta tudo" inicial */
    for (unsigned k = 0; k < sizeof in / sizeof in[0]; k++) {
        evq_push(&e.q_gtr, in[k]);
        engine_process(&e, NULL, 0, out, 256);
        while (evq_pop(&e.q_mout, &ev)) {
            char b[32];
            if (ev.type == EV_NOTE_ON) snprintf(b, sizeof b, " on%d", ev.note);
            else if (ev.type == EV_NOTE_OFF) snprintf(b, sizeof b, " off%d", ev.note);
            else if (ev.type == EV_PITCH_TRACK) snprintf(b, sizeof b, " bend%+.1f", ev.value - ev.note);
            else snprintf(b, sizeof b, " ?%d", ev.type);
            strncat(log, b, sizeof log - strlen(log) - 1);
        }
    }
    printf("  enviado:%s\n  esperado: on52 bend+0.8 off52 on55 off55\n", log);
    CHECK(!strcmp(log, " on52 bend+0.8 off52 on55 off55"), "sequência MIDI errada");
    engine_free(&e);
    params_init();
}

/* Simula um cabo da saída na entrada, com atraso conhecido */
static void test_latency_meter(void)
{
    printf("Teste 6d: medidor de latência (cabo simulado da saída na entrada)\n");
    static Engine e;
    engine_init(&e, SR);
    params_init();
    const int delays[3] = {300, 731, 2048}; /* 6,3 / 15,2 / 42,7 ms */
    for (int k = 0; k < 3; k++) {
        int D = delays[k];
        static float line[SR * 2];
        memset(line, 0, sizeof line);
        static float out[2 * 128], in[128];
        long long t = 0;
        engine_latency_start(&e);
        for (int b = 0; b < 600 && atomic_load(&e.lat_state) != LAT_DONE && atomic_load(&e.lat_state) != LAT_FAIL; b++) {
            for (int i = 0; i < 128; i++) { /* entrada = saída atrasada D amostras + chiado */
                long long src = t + i - D;
                in[i] = (src >= 0 ? line[src % (SR * 2)] : 0) * 0.5f + frand() * 0.002f;
            }
            engine_process(&e, in, 1, out, 128);
            for (int i = 0; i < 128; i++) line[(t + i) % (SR * 2)] = out[2 * i];
            t += 128;
        }
        int r = atomic_load(&e.lat_result);
        printf("  atraso real %4d amostras (%.1f ms) -> medido %4d (%.1f ms)\n", D, D * 1000.0f / SR, r, r * 1000.0f / SR);
        CHECK(atomic_load(&e.lat_state) == LAT_DONE && abs(r - D) <= 2, "medição errada");
        atomic_store(&e.lat_state, LAT_IDLE);
    }
    /* sem nada ligado: tem que desistir, não inventar um número */
    engine_latency_start(&e);
    static float out2[2 * 128], silence[128];
    for (int b = 0; b < 600 && atomic_load(&e.lat_state) != LAT_FAIL && atomic_load(&e.lat_state) != LAT_DONE; b++) {
        for (int i = 0; i < 128; i++) silence[i] = frand() * 0.002f;
        engine_process(&e, silence, 1, out2, 128);
    }
    printf("  sem cabo: %s\n", atomic_load(&e.lat_state) == LAT_FAIL ? "desistiu (certo)" : "MEDIU ALGO ERRADO");
    CHECK(atomic_load(&e.lat_state) == LAT_FAIL, "deveria desistir sem sinal de volta");
    engine_free(&e);
    params_init();
}

static void test_looper(void)
{
    printf("Teste 7: looper grava, repete e desfaz o overdub\n");
    static Looper lp;
    looper_init(&lp, SR, 5);
    float l[100], r[100];
    looper_command(&lp, LCMD_REC);
    for (int k = 0; k < 10; k++) { /* grava 1000 amostras de rampa */
        for (int i = 0; i < 100; i++) l[i] = r[i] = (k * 100 + i) / 1000.0f;
        looper_process(&lp, l, r, 100, 1);
    }
    looper_command(&lp, LCMD_REC); /* fecha o loop: passa a tocar */
    for (int i = 0; i < 100; i++) l[i] = r[i] = 0;
    looper_process(&lp, l, r, 100, 1);
    CHECK(lp.len == 1000 && fabsf(l[50] - 0.05f) < 1e-6f, "reprodução errada (len %d, l[50]=%f)", lp.len, l[50]);
    looper_command(&lp, LCMD_REC); /* overdub de 1 em tudo */
    for (int k = 0; k < 10; k++) { for (int i = 0; i < 100; i++) l[i] = r[i] = 1; looper_process(&lp, l, r, 100, 1); }
    float after = lp.l[150];
    looper_command(&lp, LCMD_UNDO);
    for (int i = 0; i < 100; i++) l[i] = r[i] = 0;
    looper_process(&lp, l, r, 100, 1);
    printf("  comprimento %d, amostra 150: %.3f -> overdub %.3f -> desfeito %.3f\n", lp.len, 0.15f, after, lp.l[150]);
    CHECK(fabsf(after - 1.15f) < 1e-5f && fabsf(lp.l[150] - 0.15f) < 1e-6f, "overdub/desfazer errado");
    looper_free(&lp);
}

static void test_presets(void)
{
    printf("Teste 8: presets salvam e carregam\n");
    params_init();
    for (int i = 0; i < factory_preset_count(); i++) factory_preset_apply(i); /* todos aplicam sem erro */
    factory_preset_apply(1);
    float cut = param_get(P_CUTOFF), drive = param_get(P_DIST_DRIVE);
    param_set(P_MASTER, 0.33f); /* não faz parte do preset */
    CHECK(preset_save("teste_preset.txt", 2) == 0, "não salvou");
    params_init();
    param_set(P_MASTER, 0.66f);
    CHECK(preset_load("teste_preset.txt") == 0, "não carregou");
    printf("  corte %.0f/%.0f, drive %.0f/%.0f, volume geral preservado %.2f\n",
           param_get(P_CUTOFF), cut, param_get(P_DIST_DRIVE), drive, param_get(P_MASTER));
    CHECK(fabsf(param_get(P_CUTOFF) - cut) < 0.5f && param_get(P_DIST_DRIVE) == drive, "valores diferentes");
    CHECK(fabsf(param_get(P_MASTER) - 0.66f) < 1e-6f, "preset não deveria mudar o volume geral");
    params_init();
    remove("teste_preset.txt");
}

/* Toca o motor completo (sem placa de som) e grava demo.wav */
static void render_demo(void)
{
    printf("Teste 9: motor completo -> demo.wav\n");
    params_init();
    static Engine e;
    if (engine_init(&e, SR) < 0) { CHECK(0, "engine_init"); return; }
    int total = SR * 20, pos = 0, glen;
    float *out = calloc((size_t)total * 2, sizeof(float));
    float *gtr = render_guitar(riff, RIFF_LEN, &glen);
    float in[PITCH_HOP];

#define RUN(seconds) do { int n_ = (int)((seconds) * SR); \
        for (int k_ = 0; k_ < n_ && pos + 256 <= total; k_ += 256, pos += 256) \
            engine_process(&e, NULL, 0, out + 2 * pos, 256); } while (0)

    /* 1) acorde com o preset "Pad espacial" */
    factory_preset_apply(2);
    engine_ui_event(&e, EV_NOTE_ON, 48, 90); engine_ui_event(&e, EV_NOTE_ON, 55, 90);
    engine_ui_event(&e, EV_NOTE_ON, 60, 90); engine_ui_event(&e, EV_NOTE_ON, 64, 90);
    RUN(2.5);
    engine_ui_event(&e, EV_ALL_OFF, 0, 0);
    RUN(1.5);
    /* 2) arpejador */
    factory_preset_apply(6);
    engine_ui_event(&e, EV_NOTE_ON, 57, 100); engine_ui_event(&e, EV_NOTE_ON, 60, 100); engine_ui_event(&e, EV_NOTE_ON, 64, 100);
    RUN(3.5);
    engine_ui_event(&e, EV_NOTE_OFF, 57, 0); engine_ui_event(&e, EV_NOTE_OFF, 60, 0); engine_ui_event(&e, EV_NOTE_OFF, 64, 0);
    RUN(1.0);
    /* 3) guitarra simulada controlando o preset "Lead de guitarra" */
    factory_preset_apply(1);
    param_set(P_INPUT_MODE, 2);
    for (int p = 0; p + PITCH_HOP <= glen && pos + PITCH_HOP <= total; p += PITCH_HOP, pos += PITCH_HOP) {
        memcpy(in, gtr + p, sizeof in);
        engine_process(&e, in, 1, out + 2 * pos, PITCH_HOP);
    }
    RUN(1.0);

    int ok = finite_and_bounded(out, pos * 2, 1.0f);
    float peak = 0;
    for (int i = 0; i < pos * 2; i++) peak = fmaxf(peak, fabsf(out[i]));
    CHECK(ok && peak > 0.05f, "saída inválida (pico %f)", peak);
    wav_write_stereo("demo.wav", out, (unsigned)pos, SR);
    printf("  demo.wav: %.1f s, pico %.2f\n", (float)pos / SR, peak);
    free(out);
    free(gtr);
    engine_free(&e);
}

int main(void)
{
    params_init();
    test_open_strings();
    test_chromatic();
    test_silence();
    test_riff();
    test_octave_jumps();
    test_legato();
    test_real_guitar();
    test_direct_mode();
    test_fast_attack();
    test_fast_attack_noise();
    test_repick();
    test_fx();
    test_smoothing();
    test_denormals();
    test_arp();
    test_harmonizer();
    test_midi_out();
    test_latency_meter();
    test_looper();
    test_presets();
    render_demo();
    printf(failures ? "\n%d falha(s)\n" : "\nTodos os testes passaram.\n", failures);
    return failures ? 1 : 0;
}
