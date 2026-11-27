/*
 * Virtual Synth como plugin CLAP (REAPER, Bitwig e outros). O mesmo motor do
 * app: entra a guitarra (áudio) e/ou MIDI, sai o som em estéreo e, se pedido,
 * as notas reconhecidas da guitarra como MIDI para gravar numa trilha.
 *
 * Cada instância tem o seu motor e os seus parâmetros (ParamSet ligado à
 * thread durante o processamento), então dá para ter várias no projeto.
 * Sem interface própria: o host mostra os parâmetros (no REAPER, os
 * controles genéricos do FX), com automação de todos eles.
 */
#include <clap/clap.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "params.h"
#include "queue.h"

#ifdef VSYNTH_GUI
#include <sys/stat.h>
#include "gui.h"
#include "plugin_gui.h"
#ifdef _WIN32
#include <direct.h>
#define mkdir_one(p) _mkdir(p)
#else
#define mkdir_one(p) mkdir(p, 0755)
#endif
#endif

#define UI_W_DEFAULT 1280
#define UI_H_DEFAULT 800

#ifndef VSYNTH_VERSION
#define VSYNTH_VERSION "1.0.7"
#endif

/* ------------------------------------------------------------------ */
/* Parâmetros que o plugin mostra                                      */
/* ------------------------------------------------------------------ */
/* coisas do app que no plugin não fazem sentido: teclado do computador,
   metrônomo e looper (a DAW tem os dela), andamento (vem do host) */
static int param_visible(int id)
{
    switch (id) {
    case P_KEYS_OCTAVE: case P_METRO_ON: case P_METRO_VOL: case P_LOOP_VOL: case P_BPM:
    case P_FX_ORDER0: case P_FX_ORDER1: case P_FX_ORDER2: case P_FX_ORDER3: case P_FX_ORDER4:
        return 0;
    default:
        return 1;
    }
}

static int g_map_ready = 0, g_nvis = 0, g_vis[P_COUNT];
static void build_map(void)
{
    if (g_map_ready) return;
    g_nvis = 0;
    for (int i = 0; i < P_COUNT; i++) if (param_visible(i)) g_vis[g_nvis++] = i;
    g_map_ready = 1;
}

/* seção de cada parâmetro, pelo começo da chave */
static const char *module_of(const char *key)
{
    static const struct { const char *pre, *mod; } M[] = {
        {"osc", "Osciladores"}, {"corte", "Filtro"}, {"ressonancia", "Filtro"}, {"filtro_", "Filtro"},
        {"ataque", "Envelope"}, {"decaimento", "Envelope"}, {"sustentacao", "Envelope"}, {"release", "Envelope"},
        {"lfo", "LFO"}, {"roda_mod", "LFO"}, {"mod", "Modulação"}, {"glide", "Osciladores"},
        {"nivel_synth", "Geral"}, {"entrada", "Guitarra"}, {"gtr_", "Guitarra"},
        {"dist_", "Distorção"}, {"chorus_", "Chorus"}, {"phaser_", "Phaser"}, {"delay_", "Delay"},
        {"reverb_", "Reverb"}, {"arp_", "Arpejador"}, {"harm_", "Harmonizador"}, {"midi_saida", "Saída MIDI"},
    };
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++)
        if (strncmp(key, M[i].pre, strlen(M[i].pre)) == 0) return M[i].mod;
    return "Geral";
}

/* ------------------------------------------------------------------ */
/* Instância                                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    clap_plugin_t       plugin;
    const clap_host_t  *host;
    Engine             *e;
    ParamSet            ps;
    double              sr;
    int                 active;
    uint32_t            max_frames;
    float              *in;   /* guitarra (mono) */
    float              *out;  /* estéreo intercalado */
    int                 last_bend; /* último pitch bend mandado (não repete o mesmo) */
#ifdef VSYNTH_GUI
    /* interface */
    Gui                *gui;
    RlContext          *rl;
    PlugWin            *win;
    uint32_t            gw, gh;     /* tamanho no padrão do host (pixels; pontos no Mac) */
    double              gscale;
    clap_id             timer;
    int                 has_timer;
    /* mudanças feitas na tela, para avisar o host (automação, controles) */
    _Atomic float       seen[P_COUNT];
    struct { int id; float v; } pend[256];
    atomic_uint         pend_w, pend_r;
#endif
} Inst;

static void apply_param(Inst *p, const clap_event_param_value_t *ev)
{
    if (ev->param_id >= P_COUNT) return;
    ParamSet *old = params_bind(&p->ps);
    param_set((int)ev->param_id, (float)ev->value);
    params_bind(old);
#ifdef VSYNTH_GUI
    /* veio do host: a tela não deve devolvê-lo como mudança dela */
    atomic_store(&p->seen[ev->param_id], atomic_load(&p->ps.values[ev->param_id]));
#endif
}

#ifdef VSYNTH_GUI
/* manda para o host o que mudou pela tela (thread de áudio ou flush) */
static void push_gui_changes(Inst *p, const clap_output_events_t *out)
{
    unsigned r = atomic_load_explicit(&p->pend_r, memory_order_relaxed);
    unsigned w = atomic_load_explicit(&p->pend_w, memory_order_acquire);
    while (r != w) {
        int id = p->pend[r & 255].id;
        float v = p->pend[r & 255].v;
        r++;
        if (!out) continue;
        clap_event_param_value_t e;
        memset(&e, 0, sizeof e);
        e.header.size = sizeof e;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type = CLAP_EVENT_PARAM_VALUE;
        e.param_id = (clap_id)id;
        e.note_id = -1; e.port_index = -1; e.channel = -1; e.key = -1;
        e.value = v;
        out->try_push(out, &e.header);
    }
    atomic_store_explicit(&p->pend_r, r, memory_order_release);
}
#endif

/* CC que chegam do host: os do app (roda de mod., volume, ressonância,
   corte); sustain e "all notes off" vão como evento */
static void handle_cc(Inst *p, int cc, int val)
{
    if (cc == 64 || cc == 120 || cc == 123) {
        Event e = {EV_CC, SRC_MIDI, (uint8_t)cc, (uint8_t)val, 0};
        evq_push(&p->e->q_midi, e);
        return;
    }
    int id = cc == 1 ? P_MODWHEEL : cc == 7 ? P_MASTER : cc == 71 ? P_RESONANCE : cc == 74 ? P_CUTOFF : -1;
    if (id >= 0) param_set_norm(id, val / 127.0f);
}

static void handle_event(Inst *p, const clap_event_header_t *h)
{
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
    switch (h->type) {
    case CLAP_EVENT_NOTE_ON: {
        const clap_event_note_t *n = (const void *)h;
        if (n->key < 0 || n->key > 127) break;
        int vel = (int)lrint(n->velocity * 127.0);
        Event e = {EV_NOTE_ON, SRC_MIDI, (uint8_t)n->key, (uint8_t)(vel < 1 ? 1 : vel > 127 ? 127 : vel), (float)n->key};
        evq_push(&p->e->q_midi, e);
        break;
    }
    case CLAP_EVENT_NOTE_OFF:
    case CLAP_EVENT_NOTE_CHOKE: {
        const clap_event_note_t *n = (const void *)h;
        if (n->key < 0) { Event e = {EV_ALL_OFF, SRC_MIDI, 0, 0, 0}; evq_push(&p->e->q_midi, e); break; }
        Event e = {EV_NOTE_OFF, SRC_MIDI, (uint8_t)n->key, 0, 0};
        evq_push(&p->e->q_midi, e);
        break;
    }
    case CLAP_EVENT_MIDI: {
        const clap_event_midi_t *m = (const void *)h;
        int st = m->data[0] & 0xF0, d1 = m->data[1] & 0x7F, d2 = m->data[2] & 0x7F;
        if (st == 0x90 && d2 > 0) { Event e = {EV_NOTE_ON, SRC_MIDI, (uint8_t)d1, (uint8_t)d2, (float)d1}; evq_push(&p->e->q_midi, e); }
        else if (st == 0x80 || (st == 0x90 && d2 == 0)) { Event e = {EV_NOTE_OFF, SRC_MIDI, (uint8_t)d1, 0, 0}; evq_push(&p->e->q_midi, e); }
        else if (st == 0xB0) handle_cc(p, d1, d2);
        else if (st == 0xE0) { Event e = {EV_PITCH_BEND, SRC_MIDI, 0, 0, (((d2 << 7) | d1) - 8192) / 8192.0f * 2.0f}; evq_push(&p->e->q_midi, e); }
        break;
    }
    case CLAP_EVENT_PARAM_VALUE:
        apply_param(p, (const clap_event_param_value_t *)h);
        break;
    default:
        break;
    }
}

/* notas reconhecidas da guitarra (e harmonia/arpejo, se pedido) -> MIDI */
static void push_midi(const clap_output_events_t *out, uint32_t time, int b0, int b1, int b2)
{
    clap_event_midi_t m;
    m.header.size     = sizeof m;
    m.header.time     = time;
    m.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    m.header.type     = CLAP_EVENT_MIDI;
    m.header.flags    = 0;
    m.port_index      = 0;
    m.data[0] = (uint8_t)b0;
    m.data[1] = (uint8_t)(b1 & 0x7F);
    m.data[2] = (uint8_t)(b2 & 0x7F);
    out->try_push(out, &m.header);
}

/* com ±2 semitons, 1 cent = 41 unidades: muda só a partir de meio cent
   (force: começo de nota, sempre manda) */
static void push_bend_f(Inst *p, const clap_output_events_t *out, uint32_t time, int ch, float semis, int force)
{
    float range = (float)param_geti(P_MOUT_BEND);
    int v = (int)lroundf(8192.0f + semis / range * 8192.0f);
    if (v < 0) v = 0;
    if (v > 16383) v = 16383;
    float unit = 8192.0f / (range * 100.0f); /* unidades por cent */
    if (!force && p->last_bend >= 0 && fabsf((float)(v - p->last_bend)) < 0.5f * unit) return;
    if (v == p->last_bend) return;
    p->last_bend = v;
    push_midi(out, time, 0xE0 | ch, v & 0x7F, v >> 7);
}
#define push_bend(p, out, time, ch, semis) push_bend_f(p, out, time, ch, semis, 1)

static void drain_midi_out(Inst *p, const clap_output_events_t *out, uint32_t time)
{
    Event e;
    int ch = param_geti(P_MOUT_CH) - 1;
    float track = 0.0f;
    int has_track = 0;
    while (evq_pop(&p->e->q_mout, &e)) {
        if (!out) continue;
        /* bends seguidos da guitarra: só o último do bloco */
        if (e.type == EV_PITCH_TRACK) { track = e.value - (float)e.note; has_track = 1; continue; }
        if (has_track) { push_bend_f(p, out, time, ch, track, 0); has_track = 0; }
        switch (e.type) {
        case EV_NOTE_ON:
            if (e.source == SRC_AUDIO) push_bend(p, out, time, ch, e.value - (float)e.note);
            push_midi(out, time, 0x90 | ch, e.note, e.velocity ? e.velocity : 1);
            break;
        case EV_NOTE_OFF:    push_midi(out, time, 0x80 | ch, e.note, 0); break;
        case EV_PITCH_BEND:  push_bend(p, out, time, ch, e.value); break;
        case EV_CC:          push_midi(out, time, 0xB0 | ch, e.note, e.velocity); break;
        case EV_ALL_OFF:
            push_midi(out, time, 0xB0 | ch, 64, 0);
            push_midi(out, time, 0xB0 | ch, 123, 0);
            push_bend(p, out, time, ch, 0);
            break;
        default: break;
        }
    }
    if (has_track) push_bend_f(p, out, time, ch, track, 0);
}

/* ------------------------------------------------------------------ */
/* clap_plugin                                                         */
/* ------------------------------------------------------------------ */
static bool p_init(const clap_plugin_t *pl)
{
    Inst *p = pl->plugin_data;
    build_map();
    paramset_init(&p->ps);
    ParamSet *old = params_bind(&p->ps);
    params_init();
    /* no plugin a guitarra é a entrada natural (MIDI funciona junto) e o
       gate fica num valor seguro; o resto como no app */
    param_set(P_INPUT_MODE, 2);
    /* o motor existe desde já (a tela pode abrir antes de ativar);
       activate o refaz se a taxa for outra */
    p->e = calloc(1, sizeof(Engine));
    int ok = p->e && engine_init(p->e, 48000.0f) == 0;
    params_bind(old);
#ifdef VSYNTH_GUI
    for (int i = 0; i < P_COUNT; i++) atomic_init(&p->seen[i], atomic_load(&p->ps.values[i]));
#endif
    return ok;
}

#ifdef VSYNTH_GUI
static void g_destroy(const clap_plugin_t *pl);
#endif
static void p_destroy(const clap_plugin_t *pl)
{
    Inst *p = pl->plugin_data;
#ifdef VSYNTH_GUI
    g_destroy(pl);
#endif
    if (p->e) { engine_free(p->e); free(p->e); }
    free(p->in);
    free(p->out);
    free(p);
}

static bool p_activate(const clap_plugin_t *pl, double sr, uint32_t min_frames, uint32_t max_frames)
{
    (void)min_frames;
    Inst *p = pl->plugin_data;
    free(p->in);
    free(p->out);
    p->sr = sr;
    p->max_frames = max_frames ? max_frames : 4096;
    p->in  = calloc(p->max_frames, sizeof(float));
    p->out = calloc((size_t)p->max_frames * 2, sizeof(float));
    if (!p->in || !p->out || !p->e) return false;
    ParamSet *old = params_bind(&p->ps);
    int ok = 1;
    if (p->e->sr != (float)sr) { /* mesma struct: a tela continua apontando para ela */
        engine_free(p->e);
        ok = engine_init(p->e, (float)sr) == 0;
    }
    if (ok) {
        guitar_start(&p->e->guitar);
        atomic_store(&p->e->cap_channel, 0);
        params_smooth_snap();
    }
    params_bind(old);
    p->active = ok;
    return ok;
}

static void p_deactivate(const clap_plugin_t *pl)
{
    Inst *p = pl->plugin_data;
    p->active = 0;
}

static bool p_start(const clap_plugin_t *pl) { (void)pl; return true; }
static void p_stop(const clap_plugin_t *pl) { (void)pl; }

static void p_reset(const clap_plugin_t *pl)
{
    Inst *p = pl->plugin_data;
    if (!p->e) return;
    Event off = {EV_ALL_OFF, SRC_MIDI, 0, 0, 0};
    evq_push(&p->e->q_midi, off);
}

static void run_block(Inst *p, const clap_process_t *pr, uint32_t from, uint32_t to)
{
    uint32_t n = to - from;
    if (n == 0) return;
    const float *src = NULL;
    if (pr->audio_inputs_count > 0 && pr->audio_inputs[0].channel_count > 0 && pr->audio_inputs[0].data32)
        src = pr->audio_inputs[0].data32[0] + from; /* guitarra no canal esquerdo (ou trilha mono) */
    if (src) memcpy(p->in, src, n * sizeof(float));
    else memset(p->in, 0, n * sizeof(float));
    engine_process(p->e, p->in, 1, p->out, (int)n);
    if (pr->audio_outputs_count > 0 && pr->audio_outputs[0].data32) {
        const clap_audio_buffer_t *o = &pr->audio_outputs[0];
        for (uint32_t c = 0; c < o->channel_count; c++) {
            float *d = o->data32[c] + from;
            int k = c < 2 ? (int)c : 1;
            for (uint32_t i = 0; i < n; i++) d[i] = p->out[2 * i + k];
        }
    }
    drain_midi_out(p, pr->out_events, from);
}

static clap_process_status p_process(const clap_plugin_t *pl, const clap_process_t *pr)
{
    Inst *p = pl->plugin_data;
    if (!p->active || !p->e) return CLAP_PROCESS_ERROR;
    ParamSet *old = params_bind(&p->ps);

    if (pr->transport && (pr->transport->flags & CLAP_TRANSPORT_HAS_TEMPO) && pr->transport->tempo > 0)
        param_set(P_BPM, (float)pr->transport->tempo);

    /* eventos no ponto exato do bloco em que chegaram */
    uint32_t nev = pr->in_events ? pr->in_events->size(pr->in_events) : 0, ei = 0, pos = 0;
    while (pos < pr->frames_count) {
        while (ei < nev) {
            const clap_event_header_t *h = pr->in_events->get(pr->in_events, ei);
            if (h->time > pos) break;
            handle_event(p, h);
            ei++;
        }
        uint32_t next = pr->frames_count;
        if (ei < nev) {
            const clap_event_header_t *h = pr->in_events->get(pr->in_events, ei);
            if (h->time < next) next = h->time;
        }
        if (next - pos > p->max_frames) next = pos + p->max_frames;
        run_block(p, pr, pos, next);
        pos = next;
    }
    while (ei < nev) handle_event(p, pr->in_events->get(pr->in_events, ei++));
#ifdef VSYNTH_GUI
    push_gui_changes(p, pr->out_events);
#endif

    params_bind(old);
    return CLAP_PROCESS_CONTINUE;
}

/* ---------- extensões ---------- */
static uint32_t ap_count(const clap_plugin_t *pl, bool is_input) { (void)pl; (void)is_input; return 1; }
static bool ap_get(const clap_plugin_t *pl, uint32_t index, bool is_input, clap_audio_port_info_t *info)
{
    (void)pl;
    if (index != 0) return false;
    info->id = is_input ? 0 : 1;
    snprintf(info->name, sizeof info->name, "%s", is_input ? "Guitarra" : "Saída");
    info->flags         = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type     = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
static const clap_plugin_audio_ports_t s_audio_ports = {ap_count, ap_get};

static uint32_t np_count(const clap_plugin_t *pl, bool is_input) { (void)pl; (void)is_input; return 1; }
static bool np_get(const clap_plugin_t *pl, uint32_t index, bool is_input, clap_note_port_info_t *info)
{
    (void)pl;
    if (index != 0) return false;
    info->id = is_input ? 0 : 1;
    info->supported_dialects = is_input ? (CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI) : CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect  = is_input ? CLAP_NOTE_DIALECT_CLAP : CLAP_NOTE_DIALECT_MIDI;
    snprintf(info->name, sizeof info->name, "%s", is_input ? "MIDI" : "Notas da guitarra");
    return true;
}
static const clap_plugin_note_ports_t s_note_ports = {np_count, np_get};

static uint32_t pa_count(const clap_plugin_t *pl) { (void)pl; build_map(); return (uint32_t)g_nvis; }
static bool pa_info(const clap_plugin_t *pl, uint32_t index, clap_param_info_t *info)
{
    (void)pl;
    build_map();
    if (index >= (uint32_t)g_nvis) return false;
    int id = g_vis[index];
    const ParamDef *d = param_def(id);
    memset(info, 0, sizeof *info);
    info->id = (clap_id)id;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (d->kind == K_INT || d->kind == K_BOOL || d->kind == K_CHOICE) info->flags |= CLAP_PARAM_IS_STEPPED;
    if (d->kind == K_CHOICE || d->kind == K_BOOL) info->flags |= CLAP_PARAM_IS_ENUM;
    snprintf(info->name, sizeof info->name, "%s", d->label);
    snprintf(info->module, sizeof info->module, "%s", module_of(d->key));
    info->min_value     = d->min;
    info->max_value     = d->max;
    info->default_value = id == P_INPUT_MODE ? 2 : d->def;
    return true;
}
static bool pa_value(const clap_plugin_t *pl, clap_id id, double *v)
{
    Inst *p = pl->plugin_data;
    if (id >= P_COUNT) return false;
    *v = atomic_load_explicit(&p->ps.values[id], memory_order_relaxed);
    return true;
}
static bool pa_to_text(const clap_plugin_t *pl, clap_id id, double value, char *buf, uint32_t size)
{
    (void)pl;
    if (id >= P_COUNT) return false;
    /* formata num conjunto de rascunho, sem mexer na instância */
    static _Thread_local ParamSet scratch;
    ParamSet *old = params_bind(&scratch);
    param_set((int)id, (float)value);
    char tmp[64];
    param_format((int)id, tmp, sizeof tmp);
    params_bind(old);
    snprintf(buf, size, "%s", tmp);
    return true;
}
/* o contrário de param_format: entende todas as unidades que ele escreve
   ("1.2 kHz", "350 ms", "1.50 s", "35%", "40% gtr", "só synth", "ligado") */
static bool pa_from_text(const clap_plugin_t *pl, clap_id id, const char *text, double *v)
{
    (void)pl;
    if (id >= P_COUNT || !text) return false;
    const ParamDef *d = param_def((int)id);
    while (*text == ' ') text++;
    if (d->kind == K_CHOICE && d->choices) {
        const char *c = d->choices;
        for (int i = 0; *c; i++) {
            size_t len = strcspn(c, ";");
            if (strncmp(c, text, len) == 0 && strlen(text) == len) { *v = d->min + i; return true; }
            c += len;
            if (*c == ';') c++;
        }
    }
    if (d->kind == K_BOOL) {
        if (!strcmp(text, "ligado") || !strcmp(text, "on")) { *v = 1; return true; }
        if (!strcmp(text, "desligado") || !strcmp(text, "off")) { *v = 0; return true; }
    }
    if (!strcmp(d->unit, "mix")) {
        if (!strncmp(text, "só synth", 9)) { *v = 0; return true; }
        if (!strncmp(text, "só gtr", 7)) { *v = 1; return true; }
    }
    char *end;
    double x = strtod(text, &end);
    if (end == text) return false;
    while (*end == ' ') end++;
    if (!strncmp(end, "kHz", 3)) x *= 1000.0;
    else if (!strncmp(end, "ms", 2) && !strcmp(d->unit, "s")) x /= 1000.0;
    else if (*end == '%') x /= 100.0;
    if (x < d->min) x = d->min;
    if (x > d->max) x = d->max;
    *v = x;
    return true;
}
static void pa_flush(const clap_plugin_t *pl, const clap_input_events_t *in, const clap_output_events_t *out)
{
    Inst *p = pl->plugin_data;
#ifdef VSYNTH_GUI
    push_gui_changes(p, out);
#else
    (void)out;
#endif
    uint32_t n = in ? in->size(in) : 0;
    for (uint32_t i = 0; i < n; i++) {
        const clap_event_header_t *h = in->get(in, i);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
            apply_param(p, (const clap_event_param_value_t *)h);
    }
}
static const clap_plugin_params_t s_params = {pa_count, pa_info, pa_value, pa_to_text, pa_from_text, pa_flush};

/* estado do projeto: uma linha de identificação e "chave = valor" de todos
   os parâmetros */
#define STATE_HEADER "# VirtualSynth state 1\n"
static bool write_all(const clap_ostream_t *s, const char *b, int n)
{
    while (n > 0) {
        int64_t w = s->write(s, b, (uint64_t)n);
        if (w <= 0) return false;
        b += w;
        n -= (int)w;
    }
    return true;
}
static bool st_save(const clap_plugin_t *pl, const clap_ostream_t *s)
{
    Inst *p = pl->plugin_data;
    char line[160];
    if (!write_all(s, STATE_HEADER, (int)strlen(STATE_HEADER))) return false;
    for (int i = 0; i < P_COUNT; i++) {
        int n = snprintf(line, sizeof line, "%s = %.9g\n", param_def(i)->key,
                         (double)atomic_load_explicit(&p->ps.values[i], memory_order_relaxed));
        if (!write_all(s, line, n)) return false;
    }
    return true;
}
static bool st_load(const clap_plugin_t *pl, const clap_istream_t *s)
{
    Inst *p = pl->plugin_data;
    size_t cap = 16384, len = 0;
    char *buf = malloc(cap);
    if (!buf) return false;
    for (;;) {
        if (len + 4096 + 1 > cap) { cap *= 2; char *nb = realloc(buf, cap); if (!nb) { free(buf); return false; } buf = nb; }
        int64_t r = s->read(s, buf + len, 4096);
        if (r < 0) { free(buf); return false; }
        if (r == 0) break;
        len += (size_t)r;
    }
    buf[len] = 0;
    /* vazio ou não é um estado nosso: recusa sem mexer em nada */
    if (strncmp(buf, STATE_HEADER, strlen(STATE_HEADER)) != 0) { free(buf); return false; }
    ParamSet *old = params_bind(&p->ps);
    int got = 0;
    for (char *line = strtok(buf + strlen(STATE_HEADER), "\n"); line; line = strtok(NULL, "\n")) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *e = eq;
        while (e > k && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
        while (*k == ' ') k++;
        got += params_read_line(k, eq + 1);
    }
    params_bind(old);
    free(buf);
    /* os valores mudaram: o host atualiza os controles */
    const clap_host_params_t *hp = p->host ? p->host->get_extension(p->host, CLAP_EXT_PARAMS) : NULL;
    if (hp && hp->rescan) hp->rescan(p->host, CLAP_PARAM_RESCAN_VALUES);
    return got > 0;
}
static const clap_plugin_state_t s_state = {st_save, st_load};

static uint32_t lat_get(const clap_plugin_t *pl) { (void)pl; return 0; }
static const clap_plugin_latency_t s_latency = {lat_get};

#ifdef VSYNTH_GUI
/* ---------- interface ---------- */
static int param_visible(int id);

static void preset_dir(char *buf, size_t cap)
{
    char base[900] = "";
#if defined(_WIN32)
    const char *a = getenv("APPDATA");
    snprintf(base, sizeof base, "%s\\VirtualSynth", a ? a : ".");
    mkdir_one(base);
    snprintf(buf, cap, "%s\\presets", base);
#elif defined(__APPLE__)
    const char *h = getenv("HOME");
    snprintf(base, sizeof base, "%s/Library/Application Support/Virtual Synth", h ? h : ".");
    mkdir_one(base);
    snprintf(buf, cap, "%s/presets", base);
#else
    const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
    if (x && x[0]) snprintf(base, sizeof base, "%s/VirtualSynth", x);
    else {
        char tmp[900];
        snprintf(tmp, sizeof tmp, "%s/.local", h ? h : ".");
        mkdir_one(tmp);
        snprintf(tmp, sizeof tmp, "%s/.local/share", h ? h : ".");
        mkdir_one(tmp);
        snprintf(base, sizeof base, "%s/.local/share/VirtualSynth", h ? h : ".");
    }
    mkdir_one(base);
    snprintf(buf, cap, "%s/presets", base);
#endif
    mkdir_one(buf);
}

/* um quadro da tela: desenha e manda ao host o que mudou */
static void gui_frame_cb(void *user)
{
    Inst *p = user;
    ParamSet *old = params_bind(&p->ps);
    gui_plugin_frame(p->gui);
    params_bind(old);
    if (p->win) plugwin_keyboard(p->win, gui_plugin_typing(p->gui));
    int any = 0;
    for (int i = 0; i < P_COUNT; i++) {
        if (!param_visible(i)) continue;
        float v = atomic_load(&p->ps.values[i]);
        if (v == atomic_load(&p->seen[i])) continue;
        atomic_store(&p->seen[i], v);
        unsigned w = atomic_load_explicit(&p->pend_w, memory_order_relaxed);
        if (w - atomic_load_explicit(&p->pend_r, memory_order_acquire) >= 256) break; /* cheio: manda no próximo */
        p->pend[w & 255].id = i;
        p->pend[w & 255].v = v;
        atomic_store_explicit(&p->pend_w, w + 1, memory_order_release);
        any = 1;
    }
    if (any) {
        const clap_host_params_t *hp = p->host->get_extension(p->host, CLAP_EXT_PARAMS);
        if (hp && hp->request_flush) hp->request_flush(p->host);
    }
}

static bool g_supported(const clap_plugin_t *pl, const char *api, bool floating)
{
    (void)pl;
    return !floating && !strcmp(api, plugwin_api());
}
static bool g_preferred(const clap_plugin_t *pl, const char **api, bool *floating)
{
    (void)pl;
    *api = plugwin_api();
    *floating = false;
    return true;
}
static bool g_create(const clap_plugin_t *pl, const char *api, bool floating)
{
    Inst *p = pl->plugin_data;
    if (floating || strcmp(api, plugwin_api()) || p->gui) return false;
    char dir[1024];
    preset_dir(dir, sizeof dir);
    p->rl = rls_create();
    if (!p->rl) return false;
    RlContext *o = rls_bind(p->rl);
    ParamSet *old = params_bind(&p->ps);
    p->gui = gui_plugin_create(p->e, dir);
    params_bind(old);
    rls_bind(o);
    if (!p->gui) { rls_destroy(p->rl); p->rl = NULL; return false; }
    if (!p->gw) { p->gw = UI_W_DEFAULT; p->gh = UI_H_DEFAULT; }
    if (p->gscale <= 0) p->gscale = 1.0;
    return true;
}
static void g_destroy(const clap_plugin_t *pl)
{
    Inst *p = pl->plugin_data;
    if (p->has_timer) {
        const clap_host_timer_support_t *ht = p->host->get_extension(p->host, CLAP_EXT_TIMER_SUPPORT);
        if (ht) ht->unregister_timer(p->host, p->timer);
        p->has_timer = 0;
    }
    if (p->win) { plugwin_destroy(p->win); p->win = NULL; }
    if (p->gui) {
        RlContext *o = rls_bind(p->rl);
        gui_plugin_destroy(p->gui);
        rls_bind(o);
        p->gui = NULL;
    }
    if (p->rl) { rls_destroy(p->rl); p->rl = NULL; }
}
static bool g_set_scale(const clap_plugin_t *pl, double scale)
{
    Inst *p = pl->plugin_data;
    if (scale <= 0) return false;
    if (!strcmp(plugwin_api(), "cocoa")) return false; /* no Mac o sistema cuida */
    /* o tamanho acompanha a escala da tela (125%, 150%...) */
    p->gw = (uint32_t)(p->gw / p->gscale * scale + 0.5);
    p->gh = (uint32_t)(p->gh / p->gscale * scale + 0.5);
    p->gscale = scale;
    if (p->win) plugwin_set_size(p->win, (int)p->gw, (int)p->gh, scale);
    return true;
}
static bool g_get_size(const clap_plugin_t *pl, uint32_t *w, uint32_t *h)
{
    Inst *p = pl->plugin_data;
    *w = p->gw ? p->gw : UI_W_DEFAULT;
    *h = p->gh ? p->gh : UI_H_DEFAULT;
    return true;
}
static bool g_can_resize(const clap_plugin_t *pl) { (void)pl; return true; }
static bool g_hints(const clap_plugin_t *pl, clap_gui_resize_hints_t *h)
{
    (void)pl;
    h->can_resize_horizontally = true;
    h->can_resize_vertically = true;
    h->preserve_aspect_ratio = true;
    h->aspect_ratio_width = 1280;
    h->aspect_ratio_height = 800;
    return true;
}
static bool g_adjust(const clap_plugin_t *pl, uint32_t *w, uint32_t *h)
{
    Inst *p = pl->plugin_data;
    double s = p->gscale > 0 ? p->gscale : 1.0;
    uint32_t minw = (uint32_t)(640 * s);
    if (*w < minw) *w = minw;
    /* mantém 16:10, pelo lado que couber */
    uint32_t hw = (uint32_t)(*w * 800.0 / 1280.0 + 0.5);
    if (hw > *h && *h >= (uint32_t)(400 * s)) *w = (uint32_t)(*h * 1280.0 / 800.0 + 0.5);
    *h = (uint32_t)(*w * 800.0 / 1280.0 + 0.5);
    return true;
}
static bool g_set_size(const clap_plugin_t *pl, uint32_t w, uint32_t h)
{
    Inst *p = pl->plugin_data;
    p->gw = w;
    p->gh = h;
    if (p->win) plugwin_set_size(p->win, (int)w, (int)h, p->gscale);
    return true;
}
static bool g_set_parent(const clap_plugin_t *pl, const clap_window_t *win)
{
    Inst *p = pl->plugin_data;
    if (!p->gui || p->win) return false;
    uintptr_t parent = 0;
    if (!strcmp(plugwin_api(), "x11")) parent = (uintptr_t)win->x11;
    else parent = (uintptr_t)win->ptr;
    p->win = plugwin_create(parent, (int)p->gw, (int)p->gh, p->gscale, p->rl, gui_frame_cb, p);
    if (!p->win) return false;
    /* ~30 quadros por segundo pelo timer do host; sem ele, a janela usa o seu */
    const clap_host_timer_support_t *ht = p->host->get_extension(p->host, CLAP_EXT_TIMER_SUPPORT);
    if (ht && ht->register_timer && ht->register_timer(p->host, 33, &p->timer)) p->has_timer = 1;
    else plugwin_own_timer(p->win, true);
    plugwin_tick(p->win);
    return true;
}
static bool g_set_transient(const clap_plugin_t *pl, const clap_window_t *w) { (void)pl; (void)w; return false; }
static void g_title(const clap_plugin_t *pl, const char *t) { (void)pl; (void)t; }
static bool g_show(const clap_plugin_t *pl)
{
    Inst *p = pl->plugin_data;
    if (p->win) plugwin_show(p->win, true);
    return p->win != NULL;
}
static bool g_hide(const clap_plugin_t *pl)
{
    Inst *p = pl->plugin_data;
    if (p->win) plugwin_show(p->win, false);
    return p->win != NULL;
}
static const clap_plugin_gui_t s_gui = {g_supported, g_preferred, g_create, g_destroy, g_set_scale, g_get_size,
                                        g_can_resize, g_hints, g_adjust, g_set_size, g_set_parent, g_set_transient,
                                        g_title, g_show, g_hide};

static void on_timer(const clap_plugin_t *pl, clap_id id)
{
    Inst *p = pl->plugin_data;
    if (p->has_timer && id == p->timer && p->win) plugwin_tick(p->win);
}
static const clap_plugin_timer_support_t s_timer = {on_timer};
#endif

static const void *p_ext(const clap_plugin_t *pl, const char *id)
{
    (void)pl;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &s_audio_ports;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &s_note_ports;
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &s_params;
    if (!strcmp(id, CLAP_EXT_STATE)) return &s_state;
    if (!strcmp(id, CLAP_EXT_LATENCY)) return &s_latency;
#ifdef VSYNTH_GUI
    if (!strcmp(id, CLAP_EXT_GUI)) return &s_gui;
    if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &s_timer;
#endif
    return NULL;
}

static void p_main(const clap_plugin_t *pl) { (void)pl; }

/* ------------------------------------------------------------------ */
/* Descritor, fábrica e entrada                                        */
/* ------------------------------------------------------------------ */
static const char *const s_features[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                         CLAP_PLUGIN_FEATURE_STEREO, NULL};
static const clap_plugin_descriptor_t s_desc = {
    .clap_version = CLAP_VERSION_INIT,
    .id           = "com.vdonasan.virtualsynth",
    .name         = "Virtual Synth",
    .vendor       = "vDonasan",
    .url          = "https://github.com/vDonasan/VSynth",
    .manual_url   = "https://github.com/vDonasan/VSynth#readme",
    .support_url  = "https://github.com/vDonasan/VSynth/issues",
    .version      = VSYNTH_VERSION,
    .description  = "Synth for guitar (audio in) and MIDI: recognizes the guitar's notes or uses the string itself as the sound source.",
    .features     = s_features,
};

static uint32_t f_count(const clap_plugin_factory_t *f) { (void)f; return 1; }
static const clap_plugin_descriptor_t *f_desc(const clap_plugin_factory_t *f, uint32_t i) { (void)f; return i == 0 ? &s_desc : NULL; }
static const clap_plugin_t *f_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id)
{
    (void)f;
    if (!clap_version_is_compatible(host->clap_version) || strcmp(id, s_desc.id) != 0) return NULL;
    Inst *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->host = host;
    p->last_bend = -1;
    p->plugin.desc             = &s_desc;
    p->plugin.plugin_data      = p;
    p->plugin.init             = p_init;
    p->plugin.destroy          = p_destroy;
    p->plugin.activate         = p_activate;
    p->plugin.deactivate       = p_deactivate;
    p->plugin.start_processing = p_start;
    p->plugin.stop_processing  = p_stop;
    p->plugin.reset            = p_reset;
    p->plugin.process          = p_process;
    p->plugin.get_extension    = p_ext;
    p->plugin.on_main_thread   = p_main;
    return &p->plugin;
}
static const clap_plugin_factory_t s_factory = {f_count, f_desc, f_create};

static bool e_init(const char *path) { (void)path; return true; }
static void e_deinit(void) {}
static const void *e_factory(const char *id) { return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &s_factory : NULL; }

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,
    .init         = e_init,
    .deinit       = e_deinit,
    .get_factory  = e_factory,
};
