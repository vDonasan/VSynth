#include "midi.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "params.h"
#include "platform.h"
#if !defined(_WIN32)
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/* Mapa de controladores (MIDI learn) — vale com ou sem PortMidi       */
/* ------------------------------------------------------------------ */
static atomic_int g_cc_map[128];
static atomic_int g_last_cc = -1, g_cc_counter = 0;
static int g_map_ready = 0;

static void map_defaults(void)
{
    for (int i = 0; i < 128; i++) atomic_store(&g_cc_map[i], -1);
    atomic_store(&g_cc_map[1], P_MODWHEEL);   /* roda de modulação */
    atomic_store(&g_cc_map[7], P_MASTER);     /* volume */
    atomic_store(&g_cc_map[71], P_RESONANCE); /* padrão "ressonância" */
    atomic_store(&g_cc_map[74], P_CUTOFF);    /* padrão "brilho/corte" */
    g_map_ready = 1;
}

int midi_cc_param(int cc)
{
    if (!g_map_ready) map_defaults();
    return (cc >= 0 && cc < 128) ? atomic_load(&g_cc_map[cc]) : -1;
}

int midi_param_cc(int param)
{
    if (!g_map_ready) map_defaults();
    for (int i = 0; i < 128; i++) if (atomic_load(&g_cc_map[i]) == param) return i;
    return -1;
}

void midi_map(int cc, int param)
{
    if (!g_map_ready) map_defaults();
    if (param >= 0) /* um parâmetro só tem um CC */
        for (int i = 0; i < 128; i++) if (atomic_load(&g_cc_map[i]) == param) atomic_store(&g_cc_map[i], -1);
    if (cc >= 0 && cc < 128) atomic_store(&g_cc_map[cc], param);
}

int midi_last_cc(int *counter)
{
    if (counter) *counter = atomic_load(&g_cc_counter);
    return atomic_load(&g_last_cc);
}

int midi_map_save(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# MIDI learn: cc = parâmetro\n");
    for (int i = 0; i < 128; i++) {
        int p = midi_cc_param(i);
        if (p >= 0) fprintf(f, "%d = %s\n", i, param_def(p)->key);
    }
    fclose(f);
    return 0;
}

int midi_map_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    for (int i = 0; i < 128; i++) atomic_store(&g_cc_map[i], -1);
    g_map_ready = 1;
    char line[256], key[128];
    int cc;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        if (sscanf(line, "%d = %127s", &cc, key) == 2) {
            int p = param_find(key);
            if (p >= 0 && cc >= 0 && cc < 128) atomic_store(&g_cc_map[cc], p);
        }
    }
    fclose(f);
    return 0;
}

static void handle_cc(EventQueue *q, int cc, int val)
{
    atomic_store(&g_last_cc, cc);
    atomic_fetch_add(&g_cc_counter, 1);
    if (cc == 64 || cc == 120 || cc == 123) { /* sustain e "all notes off" */
        Event e = {EV_CC, SRC_MIDI, (uint8_t)cc, (uint8_t)val, 0};
        evq_push(q, e);
        return;
    }
    int p = midi_cc_param(cc);
    if (p >= 0) param_set_norm(p, val / 127.0f);
}

#ifdef HAVE_PORTMIDI
#include <portmidi.h>

static int          g_init = 0;
static int          g_ids[MIDI_MAX_DEVS];
static char         g_names[MIDI_MAX_DEVS][128];
static int          g_count = 0;
static PmStream    *g_stream = NULL;
static pthread_t    g_thread;
static atomic_bool  g_running;
static EventQueue  *g_q;
static char         g_open_name[128];

/* saída */
static int          g_out_ids[MIDI_MAX_DEVS];
static char         g_out_names[MIDI_MAX_DEVS][128];
static int          g_out_count = 0;
static PmStream    *g_out = NULL;
static int          g_virtual_id = -1;
static pthread_t    g_out_thread;
static atomic_bool  g_out_running;
static EventQueue  *g_out_q;
static char         g_out_name[128];
static atomic_int   g_out_activity = 0;
static atomic_bool  g_out_test = false;

int midi_available(void) { return 1; }

int midi_refresh(void)
{
    /* o PortMidi só relê a lista com nada aberto */
    if (g_stream || g_out) return g_count;
    if (g_init) Pm_Terminate();
    Pm_Initialize();
    g_init  = 1;
    g_count = 0;
    g_out_count = 0;
    for (int i = 0; i < Pm_CountDevices(); i++) {
        const PmDeviceInfo *info = Pm_GetDeviceInfo(i);
        if (!info || info->is_virtual) continue;
        if (info->input && g_count < MIDI_MAX_DEVS) {
            g_ids[g_count] = i;
            snprintf(g_names[g_count], sizeof g_names[0], "%s", info->name);
            g_count++;
        }
        if (info->output && g_out_count < MIDI_MAX_DEVS) {
            g_out_ids[g_out_count] = i;
            snprintf(g_out_names[g_out_count], sizeof g_out_names[0], "%s", info->name);
            g_out_count++;
        }
    }
    return g_count;
}

const char *midi_device_name(int i) { return (i >= 0 && i < g_count) ? g_names[i] : ""; }
int         midi_is_open(void) { return g_stream != NULL; }
const char *midi_open_name(void) { return g_stream ? g_open_name : ""; }

static void push(EventType type, int d1, int d2, float value)
{
    Event e = {(uint8_t)type, SRC_MIDI, (uint8_t)d1, (uint8_t)d2, value};
    evq_push(g_q, e);
}

static void *midi_thread(void *arg)
{
    (void)arg;
    PmEvent buf[64];
    while (atomic_load(&g_running)) {
        int n = Pm_Read(g_stream, buf, 64);
        for (int i = 0; i < n; i++) {
            PmMessage msg = buf[i].message;
            int status = Pm_MessageStatus(msg) & 0xF0; /* qualquer canal */
            int d1 = Pm_MessageData1(msg) & 0x7F, d2 = Pm_MessageData2(msg) & 0x7F;
            switch (status) {
            case 0x90: push(d2 > 0 ? EV_NOTE_ON : EV_NOTE_OFF, d1, d2, (float)d1); break;
            case 0x80: push(EV_NOTE_OFF, d1, 0, 0); break;
            case 0xB0: handle_cc(g_q, d1, d2); break;
            case 0xE0: push(EV_PITCH_BEND, 0, 0, (((d2 << 7) | d1) - 8192) / 8192.0f * 2.0f); break;
            default: break;
            }
        }
        if (n <= 0) sleep_ms(1);
    }
    return NULL;
}

int midi_open(int i, EventQueue *q)
{
    midi_close();
    if (!g_init) midi_refresh();
    if (i < 0 || i >= g_count) return -1;
    PmStream *s = NULL;
    if (Pm_OpenInput(&s, g_ids[i], NULL, 512, NULL, NULL) != pmNoError) return -1;
    Pm_SetFilter(s, PM_FILT_ACTIVE | PM_FILT_CLOCK | PM_FILT_SYSEX);
    g_stream = s;
    g_q = q;
    snprintf(g_open_name, sizeof g_open_name, "%s", g_names[i]);
    atomic_store(&g_running, true);
    if (pthread_create(&g_thread, NULL, midi_thread, NULL) != 0) {
        Pm_Close(s);
        g_stream = NULL;
        return -1;
    }
    return 0;
}

void midi_close(void)
{
    if (!g_stream) return;
    atomic_store(&g_running, false);
    pthread_join(g_thread, NULL);
    Pm_Close(g_stream);
    g_stream = NULL;
    Event off = {EV_CC, SRC_MIDI, 123, 0, 0}; /* não deixa nota presa */
    evq_push(g_q, off);
}

/* ------------------------------------------------------------------ */
/* Saída MIDI                                                          */
/* ------------------------------------------------------------------ */
int         midi_out_count(void) { return g_out_count; }
const char *midi_out_name(int i) { return (i >= 0 && i < g_out_count) ? g_out_names[i] : ""; }
int         midi_out_is_open(void) { return g_out != NULL; }
const char *midi_out_open_name(void) { return g_out ? g_out_name : ""; }
void        midi_out_test(void) { atomic_store(&g_out_test, true); }
int         midi_out_activity(void) { return atomic_load(&g_out_activity); }

int midi_virtual_supported(void)
{
#if defined(_WIN32)
    return 0;
#elif defined(__APPLE__)
    return 1;
#else
    /* Sem o sequenciador ALSA o PortMidi aborta o programa ao criar a
       porta virtual (em vez de devolver erro); então confere antes. */
    return access("/dev/snd/seq", R_OK | W_OK) == 0;
#endif
}

static void out3(int status, int d1, int d2)
{
    Pm_WriteShort(g_out, 0, Pm_Message(status, d1 & 0x7F, d2 & 0x7F));
    atomic_fetch_add(&g_out_activity, 1);
}

/* semitons -> pitch bend de 14 bits, com a faixa escolhida (padrão ±2) */
static void out_bend(int ch, float semis)
{
    float b = semis / (float)param_geti(P_MOUT_BEND);
    if (b > 1) b = 1;
    if (b < -1) b = -1;
    int v = 8192 + (int)lroundf(b * 8191.0f);
    out3(0xE0 | ch, v & 0x7F, v >> 7);
}

static void *midi_out_thread(void *arg)
{
    (void)arg;
    Event e;
    while (atomic_load(&g_out_running)) {
        int ch = param_geti(P_MOUT_CH) - 1;
        if (atomic_exchange(&g_out_test, false)) { /* Dó central por 0,3 s */
            out_bend(ch, 0);
            out3(0x90 | ch, 60, 100);
            sleep_ms(300);
            out3(0x80 | ch, 60, 0);
        }
        int any = 0;
        while (evq_pop(g_out_q, &e)) {
            any = 1;
            switch (e.type) {
            case EV_NOTE_ON:
                if (e.source == SRC_AUDIO) out_bend(ch, e.value - (float)e.note); /* afinação fina */
                out3(0x90 | ch, e.note, e.velocity ? e.velocity : 1);
                break;
            case EV_NOTE_OFF:    out3(0x80 | ch, e.note, 0); break;
            case EV_PITCH_TRACK: out_bend(ch, e.value - (float)e.note); break;
            case EV_PITCH_BEND:  out_bend(ch, e.value); break;
            case EV_CC:          out3(0xB0 | ch, e.note, e.velocity); break;
            case EV_ALL_OFF:
                out3(0xB0 | ch, 64, 0);  /* solta o sustain */
                out3(0xB0 | ch, 123, 0); /* all notes off */
                out_bend(ch, 0);
                break;
            default: break;
            }
        }
        if (!any) sleep_ms(1);
    }
    return NULL;
}

int midi_out_open(int i, EventQueue *q)
{
    midi_out_close();
    if (!g_init) midi_refresh();
    int dev = -1;
    if (i < 0) {
        if (!midi_virtual_supported()) return -1;
#ifdef __APPLE__
        dev = Pm_CreateVirtualOutput("Sintetizador", "CoreMIDI", NULL);
#else
        dev = Pm_CreateVirtualOutput("Sintetizador", "ALSA", NULL);
#endif
        if (dev < 0) return -1;
        g_virtual_id = dev;
        snprintf(g_out_name, sizeof g_out_name, "porta virtual \"Sintetizador\"");
    } else {
        if (i >= g_out_count) return -1;
        dev = g_out_ids[i];
        snprintf(g_out_name, sizeof g_out_name, "%s", g_out_names[i]);
    }
    PmStream *s = NULL;
    if (Pm_OpenOutput(&s, dev, NULL, 1024, NULL, NULL, 0) != pmNoError) {
        if (g_virtual_id >= 0) { Pm_DeleteVirtualDevice(g_virtual_id); g_virtual_id = -1; }
        return -1;
    }
    g_out   = s;
    g_out_q = q;
    Event e;
    while (evq_pop(q, &e)) {} /* descarta o que acumulou com a saída fechada */
    atomic_store(&g_out_running, true);
    if (pthread_create(&g_out_thread, NULL, midi_out_thread, NULL) != 0) {
        midi_out_close();
        return -1;
    }
    return 0;
}

void midi_out_close(void)
{
    if (!g_out) return;
    if (atomic_load(&g_out_running)) {
        atomic_store(&g_out_running, false);
        pthread_join(g_out_thread, NULL);
    }
    int ch = param_geti(P_MOUT_CH) - 1;
    out3(0xB0 | ch, 123, 0); /* não deixa nota presa na DAW */
    out_bend(ch, 0);
    Pm_Close(g_out);
    g_out = NULL;
    if (g_virtual_id >= 0) { Pm_DeleteVirtualDevice(g_virtual_id); g_virtual_id = -1; }
}

#else /* compilado sem PortMidi */

int         midi_available(void) { return 0; }
int         midi_refresh(void) { return 0; }
const char *midi_device_name(int i) { (void)i; return ""; }
int         midi_open(int i, EventQueue *q) { (void)i; (void)q; (void)handle_cc; return -1; }
void        midi_close(void) {}
int         midi_is_open(void) { return 0; }
const char *midi_open_name(void) { return ""; }
int         midi_out_count(void) { return 0; }
const char *midi_out_name(int i) { (void)i; return ""; }
int         midi_virtual_supported(void) { return 0; }
int         midi_out_open(int i, EventQueue *q) { (void)i; (void)q; return -1; }
void        midi_out_close(void) {}
int         midi_out_is_open(void) { return 0; }
const char *midi_out_open_name(void) { return ""; }
void        midi_out_test(void) {}
int         midi_out_activity(void) { return 0; }

#endif
