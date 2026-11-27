/* Um "host" mínimo que carrega o VirtualSynth.clap como uma DAW faria e
   confere o básico: som com MIDI e com a guitarra, notas da guitarra saindo
   como MIDI, duas instâncias independentes, estado salvo e carregado,
   automação de parâmetro. Uso: test_clap_host caminho/VirtualSynth.clap */
#include <clap/clap.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void *load_lib(const char *p) { return (void *)LoadLibraryA(p); }
static void *get_sym(void *h, const char *s) { return (void *)GetProcAddress((HMODULE)h, s); }
#else
#include <dlfcn.h>
static void *load_lib(const char *p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static void *get_sym(void *h, const char *s) { return dlsym(h, s); }
#endif

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FALHOU: %s\n", msg); failures++; } } while (0)

/* ---------- host ---------- */
static const void *h_ext(const clap_host_t *h, const char *id) { (void)h; (void)id; return NULL; }
static void h_req(const clap_host_t *h) { (void)h; }
static const clap_host_t HOST = {CLAP_VERSION_INIT, NULL, "teste", "vDonasan", "", "1.0", h_ext, h_req, h_req, h_req};

/* ---------- listas de eventos ---------- */
typedef struct { clap_input_events_t in; const clap_event_header_t *ev[64]; uint32_t n; } InList;
static uint32_t il_size(const clap_input_events_t *l) { return ((const InList *)l->ctx)->n; }
static const clap_event_header_t *il_get(const clap_input_events_t *l, uint32_t i) { return ((const InList *)l->ctx)->ev[i]; }
static void il_init(InList *l) { l->n = 0; l->in.ctx = l; l->in.size = il_size; l->in.get = il_get; }

typedef struct { clap_output_events_t out; int note_on[128], note_off, bends; } OutList;
static bool ol_push(const clap_output_events_t *l, const clap_event_header_t *h)
{
    OutList *o = l->ctx;
    if (h->type == CLAP_EVENT_MIDI) {
        const clap_event_midi_t *m = (const void *)h;
        int st = m->data[0] & 0xF0;
        if (st == 0x90 && m->data[2]) o->note_on[m->data[1]]++;
        else if (st == 0x80) o->note_off++;
        else if (st == 0xE0) o->bends++;
    }
    return true;
}
static void ol_init(OutList *o) { memset(o, 0, sizeof *o); o->out.ctx = o; o->out.try_push = ol_push; }

/* ---------- streams do estado ---------- */
typedef struct { char buf[65536]; size_t len, pos; } Mem;
static int64_t ms_write(const clap_ostream_t *s, const void *b, uint64_t n)
{
    Mem *m = s->ctx;
    if (m->len + n > sizeof m->buf) return -1;
    memcpy(m->buf + m->len, b, n);
    m->len += n;
    return (int64_t)n;
}
static int64_t ms_read(const clap_istream_t *s, void *b, uint64_t n)
{
    Mem *m = s->ctx;
    uint64_t left = m->len - m->pos;
    if (n > left) n = left;
    memcpy(b, m->buf + m->pos, n);
    m->pos += n;
    return (int64_t)n;
}

/* ---------- helpers ---------- */
static double SRATE = 48000.0;
static const clap_plugin_t *make(const clap_plugin_factory_t *f)
{
    const clap_plugin_descriptor_t *d = f->get_plugin_descriptor(f, 0);
    const clap_plugin_t *p = f->create_plugin(f, &HOST, d->id);
    if (!p || !p->init(p)) return NULL;
    if (!p->activate(p, SRATE, 32, 512)) return NULL;
    p->start_processing(p);
    return p;
}

static clap_id find_param(const clap_plugin_t *p, const char *name, const char *module)
{
    const clap_plugin_params_t *pa = p->get_extension(p, CLAP_EXT_PARAMS);
    for (uint32_t i = 0; i < pa->count(p); i++) {
        clap_param_info_t inf;
        pa->get_info(p, i, &inf);
        if (!strcmp(inf.name, name) && (!module || !strcmp(inf.module, module))) return inf.id;
    }
    return CLAP_INVALID_ID;
}

static clap_event_param_value_t pv(clap_id id, double v)
{
    clap_event_param_value_t e;
    memset(&e, 0, sizeof e);
    e.header.size = sizeof e;
    e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e.header.type = CLAP_EVENT_PARAM_VALUE;
    e.param_id = id;
    e.note_id = -1; e.port_index = -1; e.channel = -1; e.key = -1;
    e.value = v;
    return e;
}

/* roda 'blocks' blocos de 512; devolve o RMS da saída esquerda */
static double run(const clap_plugin_t *p, int blocks, InList *first, OutList *out, const float *gtr, long *gtr_pos)
{
    static float inL[512], inR[512], oL[512], oR[512];
    float *ins[2] = {inL, inR}, *outs[2] = {oL, oR};
    clap_audio_buffer_t ib = {ins, NULL, 2, 0, 0}, ob = {outs, NULL, 2, 0, 0};
    InList empty;
    il_init(&empty);
    double acc = 0;
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < 512; i++) { float v = gtr ? gtr[(*gtr_pos)++] : 0.0f; inL[i] = inR[i] = v; }
        clap_process_t pr;
        memset(&pr, 0, sizeof pr);
        pr.frames_count = 512;
        pr.audio_inputs = &ib; pr.audio_inputs_count = 1;
        pr.audio_outputs = &ob; pr.audio_outputs_count = 1;
        pr.in_events = b == 0 && first ? &first->in : &empty.in;
        pr.out_events = &out->out;
        p->process(p, &pr);
        for (int i = 0; i < 512; i++) acc += (double)oL[i] * oL[i];
    }
    return sqrt(acc / (blocks * 512.0));
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("uso: %s VirtualSynth.clap\n", argv[0]); return 2; }
    void *lib = load_lib(argv[1]);
    if (!lib) { printf("não abriu %s\n", argv[1]); return 1; }
    const clap_plugin_entry_t *entry = get_sym(lib, "clap_entry");
    if (!entry || !entry->init(argv[1])) { printf("sem clap_entry\n"); return 1; }
    const clap_plugin_factory_t *f = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    printf("Plugin: %s (%s), %u plugin(s) na fábrica\n", f->get_plugin_descriptor(f, 0)->name,
           f->get_plugin_descriptor(f, 0)->version, f->get_plugin_count(f));

    const clap_plugin_t *a = make(f), *b = make(f);
    CHECK(a && b, "não criou/ativou duas instâncias");
    if (!a || !b) return 1;
    const clap_plugin_params_t *pa = a->get_extension(a, CLAP_EXT_PARAMS);
    printf("  %u parâmetros\n", pa->count(a));
    CHECK(pa->count(a) > 50, "poucos parâmetros");

    /* 1. MIDI faz som */
    InList il; OutList ol; il_init(&il); ol_init(&ol);
    clap_event_note_t on = {{sizeof on, 10, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0}, -1, 0, 0, 57, 0.8};
    il.ev[il.n++] = &on.header;
    double rms = run(a, 40, &il, &ol, NULL, NULL);
    printf("  nota MIDI: saída %.1f dB\n", 20 * log10(rms + 1e-12));
    CHECK(rms > 0.003, "nota MIDI não fez som");

    /* 2. instâncias independentes: corte diferente em cada uma */
    clap_id cut = find_param(a, "Corte", "Filtro");
    CHECK(cut != CLAP_INVALID_ID, "não achou o parâmetro Corte");
    clap_event_param_value_t c1 = pv(cut, 300.0), c2 = pv(cut, 9000.0);
    il_init(&il); il.ev[il.n++] = &c1.header; run(a, 1, &il, &ol, NULL, NULL);
    il_init(&il); il.ev[il.n++] = &c2.header; run(b, 1, &il, &ol, NULL, NULL);
    double va, vb;
    pa->get_value(a, cut, &va);
    pa->get_value(b, cut, &vb);
    printf("  instâncias: corte A = %.0f Hz, B = %.0f Hz\n", va, vb);
    CHECK(fabs(va - 300) < 1 && fabs(vb - 9000) < 1, "as instâncias dividem os parâmetros");

    /* 3. estado: salva A, carrega em B */
    const clap_plugin_state_t *st = a->get_extension(a, CLAP_EXT_STATE);
    static Mem mem;
    clap_ostream_t os = {&mem, ms_write};
    clap_istream_t is = {&mem, ms_read};
    CHECK(st->save(a, &os), "não salvou o estado");
    CHECK(st->load(b, &is), "não carregou o estado");
    pa->get_value(b, cut, &vb);
    printf("  estado: %zu bytes, corte em B depois de carregar = %.0f Hz\n", mem.len, vb);
    CHECK(fabs(vb - 300) < 1, "estado não restaurou o parâmetro");

    /* 4. texto dos parâmetros */
    char txt[64];
    pa->value_to_text(a, cut, 1200.0, txt, sizeof txt);
    printf("  texto: 1200 -> \"%s\"\n", txt);
    CHECK(strlen(txt) > 0, "value_to_text vazio");

    /* 5. guitarra: nota reconhecida vira som e MIDI de saída, em várias taxas */
    const double rates[4] = {48000.0, 44100.0, 96000.0, 192000.0};
    for (int r = 0; r < 4; r++) {
        SRATE = rates[r];
        const clap_plugin_t *g = make(f);
        CHECK(g != NULL, "não ativou nessa taxa");
        if (!g) continue;
        clap_id msrc = find_param(g, "Enviar", "Saída MIDI");
        CHECK(msrc != CLAP_INVALID_ID, "não achou Saída MIDI > Enviar");
        clap_event_param_value_t m1 = pv(msrc, 1.0);
        il_init(&il); il.ev[il.n++] = &m1.header;
        OutList go; ol_init(&go);
        long n = (long)(SRATE * 2), pos = 0;
        float *gtr = calloc(n + 4096, sizeof(float));
        srand(1);
        for (long i = 0; i < n; i++) {
            double t = (i - SRATE * 0.1) / SRATE;
            if (t < 0) continue;
            double f0 = 110.0, env = exp(-2.0 * t), v = 0;
            for (int k = 1; k <= 8; k++) v += sin(2 * M_PI * f0 * k * t) / k * exp(-0.4 * k * t);
            gtr[i] = (float)(0.2 * env * v);
            if (t < 0.004) gtr[i] += (float)(0.1 * ((rand() / (double)RAND_MAX) * 2 - 1));
        }
        double grms = run(g, (int)(n / 512), &il, &go, gtr, &pos);
        int others = 0;
        char ol_txt[128] = "";
        for (int k = 0; k < 128; k++) if (k != 45 && go.note_on[k]) {
            others += go.note_on[k];
            char t[16]; snprintf(t, sizeof t, " %d", k);
            strncat(ol_txt, t, sizeof ol_txt - strlen(ol_txt) - 1);
        }
        printf("  guitarra a %6.0f Hz (Lá 110 Hz): saída %.1f dB, MIDI de saída: nota 45 x%d, outras:%s\n",
               SRATE, 20 * log10(grms + 1e-12), go.note_on[45], others ? ol_txt : " nenhuma");
        CHECK(grms > 0.003, "a guitarra não fez som");
        /* aceita só a vizinha corrigida logo no começo (palhetada chegando
           de meio tom abaixo): nada além disso */
        int bad = others - go.note_on[44] - go.note_on[46];
        CHECK(go.note_on[45] == 1 && bad == 0 && go.note_on[44] + go.note_on[46] <= 1, "a nota da guitarra não saiu certa como MIDI");
        g->destroy(g);
        free(gtr);
    }

    a->destroy(a);
    b->destroy(b);
    entry->deinit();
    printf(failures ? "\n%d falha(s)\n" : "\nPlugin: todos os testes passaram.\n", failures);
    return failures ? 1 : 0;
}
