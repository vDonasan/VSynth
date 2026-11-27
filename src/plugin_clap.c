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
} Inst;

static void apply_param(Inst *p, const clap_event_param_value_t *ev)
{
    if (ev->param_id >= P_COUNT) return;
    ParamSet *old = params_bind(&p->ps);
    param_set((int)ev->param_id, (float)ev->value);
    params_bind(old);
}

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
    params_bind(old);
    return true;
}

static void p_destroy(const clap_plugin_t *pl)
{
    Inst *p = pl->plugin_data;
    if (p->e) { engine_free(p->e); free(p->e); }
    free(p->in);
    free(p->out);
    free(p);
}

static bool p_activate(const clap_plugin_t *pl, double sr, uint32_t min_frames, uint32_t max_frames)
{
    (void)min_frames;
    Inst *p = pl->plugin_data;
    if (p->e) { engine_free(p->e); free(p->e); p->e = NULL; }
    free(p->in);
    free(p->out);
    p->sr = sr;
    p->max_frames = max_frames ? max_frames : 4096;
    p->in  = calloc(p->max_frames, sizeof(float));
    p->out = calloc((size_t)p->max_frames * 2, sizeof(float));
    p->e   = calloc(1, sizeof(Engine));
    if (!p->in || !p->out || !p->e) return false;
    ParamSet *old = params_bind(&p->ps);
    int ok = engine_init(p->e, (float)sr) == 0;
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
static bool pa_from_text(const clap_plugin_t *pl, clap_id id, const char *text, double *v)
{
    (void)pl;
    if (id >= P_COUNT) return false;
    const ParamDef *d = param_def((int)id);
    if (d->kind == K_CHOICE && d->choices) {
        const char *c = d->choices;
        for (int i = 0; *c; i++) {
            size_t len = strcspn(c, ";");
            if (strncmp(c, text, len) == 0 && strlen(text) == len) { *v = d->min + i; return true; }
            c += len;
            if (*c == ';') c++;
        }
    }
    char *end;
    double x = strtod(text, &end);
    if (end == text) return false;
    if (d->unit && strcmp(d->unit, "%") == 0) x /= 100.0;
    *v = x;
    return true;
}
static void pa_flush(const clap_plugin_t *pl, const clap_input_events_t *in, const clap_output_events_t *out)
{
    (void)out;
    Inst *p = pl->plugin_data;
    uint32_t n = in ? in->size(in) : 0;
    for (uint32_t i = 0; i < n; i++) {
        const clap_event_header_t *h = in->get(in, i);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
            apply_param(p, (const clap_event_param_value_t *)h);
    }
}
static const clap_plugin_params_t s_params = {pa_count, pa_info, pa_value, pa_to_text, pa_from_text, pa_flush};

/* estado do projeto: "chave = valor" de todos os parâmetros */
static bool st_save(const clap_plugin_t *pl, const clap_ostream_t *s)
{
    Inst *p = pl->plugin_data;
    char line[160];
    for (int i = 0; i < P_COUNT; i++) {
        int n = snprintf(line, sizeof line, "%s = %.9g\n", param_def(i)->key,
                         (double)atomic_load_explicit(&p->ps.values[i], memory_order_relaxed));
        const char *b = line;
        while (n > 0) {
            int64_t w = s->write(s, b, (uint64_t)n);
            if (w <= 0) return false;
            b += w;
            n -= (int)w;
        }
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
    ParamSet *old = params_bind(&p->ps);
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *e = eq;
        while (e > k && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
        while (*k == ' ') k++;
        params_read_line(k, eq + 1);
    }
    params_bind(old);
    free(buf);
    return true;
}
static const clap_plugin_state_t s_state = {st_save, st_load};

static uint32_t lat_get(const clap_plugin_t *pl) { (void)pl; return 0; }
static const clap_plugin_latency_t s_latency = {lat_get};

static const void *p_ext(const clap_plugin_t *pl, const char *id)
{
    (void)pl;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &s_audio_ports;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &s_note_ports;
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &s_params;
    if (!strcmp(id, CLAP_EXT_STATE)) return &s_state;
    if (!strcmp(id, CLAP_EXT_LATENCY)) return &s_latency;
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
