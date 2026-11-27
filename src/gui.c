/*
 * Interface gráfica — layout do mockup "Sintetizador — interface nova".
 *
 *  ┌ barra: logo · preset ‹ › · salvar · ⚙ áudio · MIDI learn · gravar · pânico · volume ┐
 *  ├ ENTRADA (teclado / MIDI / guitarra) ┬ abas: Som · Efeitos · Modulação · Arp/Harmonia · Looper · Saída MIDI ┤
 *  ├ osciloscópio ───────────────────────┴───────────────────────────────────────────────┤
 *  ├ piano clicável ─────────────────────────────────────────────────────────────────────┤
 *  └ barra de status ────────────────────────────────────────────────────────────────────┘
 *
 * Os widgets (knobs, botões, listas...) estão em ui.c.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "raylib.h"

#include "engine.h"
#include "fx.h"
#include "gui.h"
#include "midi.h"
#include "params.h"
#include "pitch.h"
#include "recorder.h"
#include "ui.h"

#define PRESET_DIR "presets"
#define SETTINGS_FILE "configuracoes.ini"
#define REC_DIR    "gravacoes"
#define MAX_USER_PRESETS 64

enum { DD_PRESET = 1, DD_OUT, DD_IN, DD_MIDI, DD_MOUT, DD_CH, DD_MOD = 20 };
enum { TB_SAVE = 1, TB_SEARCH };

typedef struct {
    Engine *e;
    int     tab;

    /* dispositivos */
    char out_items[4096], in_items[4096], midi_items[4096], mout_items[4096], ch_items[256];
    int  out_active, in_active, midi_active, mout_active, in_ch;
    char audio_error[256];

    /* presets */
    char preset_items[8192];
    char user_presets[MAX_USER_PRESETS][128];
    int  n_user, preset_active;
    char preset_name[64];
    int  user_cat[MAX_USER_PRESETS];
    bool save_open;
    char save_name[64];
    int  save_cat;

    /* navegador de presets */
    bool   browser_open;
    int    br_cat;            /* -1 = todas */
    char   br_search[48];
    int    br_first;          /* primeira linha visível */
    int    br_last_click;
    double br_click_time;

    /* janela de áudio */
    bool audio_open;

    /* MIDI learn */
    int  learn_seen;

    /* notas tocadas pela interface */
    int  key_note[64];
    int  mouse_note;

    /* gravação */
    Recorder rec;
    Recorder rec_gtr;  /* entrada crua da guitarra + synth, para diagnóstico */
    double   rec_start;
    float    rec_buf[16384];

    /* status e medidores */
    char   status[640];
    double status_time;
    float  out_level;
    int    tuner_note;
    float  tuner_cents, tuner_hz;
    double tuner_time;
    double taps[4];
    int    ntaps;
    int    last_mode, mout_last_src, mout_seen;
    double mout_blink;

    /* medição de latência: 5 rodadas, mostra a mediana */
    int    lat_runs;
    bool   lat_running;
    float  lat_vals[5];
    float  lat_ms;      /* resultado (0 = ainda não mediu, <0 = falhou) */
    double lat_next;

    /* calibração do gate */
    int    calibrating;   /* esperando o resultado da medida na thread de áudio */
    double calib_start;

    /* arrastar pedais */
    int   drag_fx;   /* posição (slot) sendo arrastada, -1 = nenhuma */
    float drag_off;
} Gui;

/* ------------------------------------------------------------------ */
/* Utilidades                                                          */
/* ------------------------------------------------------------------ */
static void set_status(Gui *g, const char *msg)
{
    snprintf(g->status, sizeof g->status, "%s", msg);
    g->status_time = GetTime();
}

static void append_item(char *list, size_t cap, const char *item)
{
    if (list[0]) strncat(list, ";", cap - strlen(list) - 1);
    /* ';' separa itens: troca por ',' se aparecer no nome */
    size_t l = strlen(list);
    for (const char *p = item; *p && l + 1 < cap; p++) list[l++] = *p == ';' ? ',' : *p;
    list[l] = 0;
}

static float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }

/* ------------------------------------------------------------------ */
/* Dispositivos                                                        */
/* ------------------------------------------------------------------ */
static void build_channel_list(Gui *g)
{
    int n = engine_capture_channels(g->e);
    if (n < 1) n = 2;
    g->ch_items[0] = 0;
    for (int i = 0; i < n && i < 32; i++) {
        char t[8];
        snprintf(t, sizeof t, "%d", i + 1);
        append_item(g->ch_items, sizeof g->ch_items, t);
    }
    if (g->in_ch >= n) g->in_ch = 0;
}

static void build_device_lists(Gui *g)
{
    Engine *e = g->e;
    engine_refresh_devices(e);
    g->out_items[0] = g->in_items[0] = 0;
    append_item(g->out_items, sizeof g->out_items, "Padrão do sistema");
    for (int i = 0; i < e->nplay; i++) append_item(g->out_items, sizeof g->out_items, e->play[i].name);
    append_item(g->in_items, sizeof g->in_items, "Padrão do sistema");
    for (int i = 0; i < e->ncap; i++) append_item(g->in_items, sizeof g->in_items, e->cap[i].name);

    int n = midi_refresh();
    g->midi_items[0] = 0;
    if (n == 0) append_item(g->midi_items, sizeof g->midi_items, midi_available() ? "(nenhum dispositivo)" : "(sem suporte a MIDI)");
    for (int i = 0; i < n; i++) append_item(g->midi_items, sizeof g->midi_items, midi_device_name(i));
    if (g->midi_active >= n) g->midi_active = 0;

    g->mout_items[0] = 0;
    if (midi_virtual_supported()) append_item(g->mout_items, sizeof g->mout_items, "Porta virtual \"Sintetizador\"");
    for (int i = 0; i < midi_out_count(); i++) append_item(g->mout_items, sizeof g->mout_items, midi_out_name(i));
    if (!g->mout_items[0])
        append_item(g->mout_items, sizeof g->mout_items, midi_available() ? "(nenhuma saída MIDI)" : "(sem suporte a MIDI)");
    build_channel_list(g);
}

static bool out_is_asio(Gui *g)
{
    int i = g->out_active - 1;
    return i >= 0 && i < g->e->nplay && g->e->play[i].asio >= 0;
}

static void apply_audio(Gui *g)
{
    int mode = param_geti(P_INPUT_MODE);
    int cap  = mode == 2 ? g->in_active - 1 : -2;
    g->audio_error[0] = 0;
    if (engine_open(g->e, g->out_active - 1, cap, g->in_ch) < 0) {
        if (cap != -2 && engine_open(g->e, g->out_active - 1, -2, 0) == 0)
            snprintf(g->audio_error, sizeof g->audio_error, "Não abriu a entrada de áudio. Tente outro dispositivo.");
        else if (g->e->asio_error[0])
            snprintf(g->audio_error, sizeof g->audio_error, "%s", g->e->asio_error);
        else
            snprintf(g->audio_error, sizeof g->audio_error, "Não abriu a saída de áudio!");
        set_status(g, g->audio_error);
    }
    build_channel_list(g);
    atomic_store(&g->e->cap_channel, g->in_ch);
}

static void apply_midi(Gui *g)
{
    if (param_geti(P_INPUT_MODE) != 1) { midi_close(); return; }
    if (midi_refresh() == 0) { midi_close(); return; }
    if (midi_open(g->midi_active, &g->e->q_midi) == 0) {
        char m[200];
        snprintf(m, sizeof m, "MIDI conectado: %s", midi_open_name());
        set_status(g, m);
    } else {
        set_status(g, "Não foi possível abrir o dispositivo MIDI");
    }
}

static void apply_midi_out(Gui *g)
{
    if (param_geti(P_MOUT_SRC) == 0) { midi_out_close(); return; }
    int idx = midi_virtual_supported() ? g->mout_active - 1 : g->mout_active; /* -1 = porta virtual */
    if (!midi_virtual_supported() && midi_out_count() == 0) {
#ifdef _WIN32
        set_status(g, "Nenhuma saída MIDI. Instale o loopMIDI, crie uma porta e clique em Atualizar.");
#else
        set_status(g, "Nenhuma saída MIDI: o sistema MIDI (ALSA sequencer) não está disponível.");
#endif
        return;
    }
    if (midi_out_open(idx, &g->e->q_mout) == 0) {
        char m[200];
        snprintf(m, sizeof m, "Saída MIDI: enviando para %s", midi_out_open_name());
        set_status(g, m);
    } else {
        set_status(g, "Não foi possível abrir a saída MIDI");
    }
}

static void refresh_all_devices(Gui *g)
{
    midi_close();
    midi_out_close();
    build_device_lists(g);
    apply_midi(g);
    apply_midi_out(g);
}

/* ------------------------------------------------------------------ */
/* Presets                                                             */
/* ------------------------------------------------------------------ */
static int cmp_str(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void build_preset_list(Gui *g)
{
    g->n_user = 0;
    if (DirectoryExists(PRESET_DIR)) {
        FilePathList fl = LoadDirectoryFilesEx(PRESET_DIR, ".txt", false);
        for (unsigned i = 0; i < fl.count && g->n_user < MAX_USER_PRESETS; i++)
            snprintf(g->user_presets[g->n_user++], 128, "%s", GetFileNameWithoutExt(fl.paths[i]));
        UnloadDirectoryFiles(fl);
    }
    qsort(g->user_presets, g->n_user, sizeof g->user_presets[0], cmp_str);
    for (int i = 0; i < g->n_user; i++) {
        char path[300];
        snprintf(path, sizeof path, PRESET_DIR "/%s.txt", g->user_presets[i]);
        g->user_cat[i] = preset_file_category(path);
    }
    g->preset_items[0] = 0;
    for (int i = 0; i < factory_preset_count(); i++) append_item(g->preset_items, sizeof g->preset_items, factory_preset_name(i));
    for (int i = 0; i < g->n_user; i++) {
        char t[140];
        snprintf(t, sizeof t, "★ %s", g->user_presets[i]);
        append_item(g->preset_items, sizeof g->preset_items, t);
    }
}

static int preset_total(Gui *g) { return factory_preset_count() + g->n_user; }

static int preset_cat_of(Gui *g, int idx)
{
    int nf = factory_preset_count();
    if (idx < 0) return 0;
    return idx < nf ? factory_preset_category(idx) : g->user_cat[idx - nf];
}

static const char *preset_name_of(Gui *g, int idx)
{
    int nf = factory_preset_count();
    return idx < nf ? factory_preset_name(idx) : g->user_presets[idx - nf];
}

static const char *preset_desc_of(Gui *g, int idx)
{
    (void)g;
    int nf = factory_preset_count();
    return idx < nf ? factory_preset_desc(idx) : "Preset seu, salvo na pasta presets/";
}

static void load_preset(Gui *g, int idx)
{
    Event off = {EV_ALL_OFF, SRC_KEYS, 0, 0, 0};
    evq_push(&g->e->q_ui, off);
    int nf = factory_preset_count();
    if (idx < nf) {
        factory_preset_apply(idx);
        snprintf(g->preset_name, sizeof g->preset_name, "%s", factory_preset_name(idx));
    } else {
        char path[300];
        snprintf(path, sizeof path, PRESET_DIR "/%s.txt", g->user_presets[idx - nf]);
        preset_load(path);
        snprintf(g->preset_name, sizeof g->preset_name, "%s", g->user_presets[idx - nf]);
    }
    g->preset_active = idx;
    char m[160];
    snprintf(m, sizeof m, "Preset carregado: %s", g->preset_name);
    set_status(g, m);
}

static void save_preset(Gui *g, const char *name)
{
    char clean[64];
    int j = 0;
    for (int i = 0; name[i] && j < 60; i++) {
        unsigned char c = (unsigned char)name[i];
        if (strchr("/\\:*?\"<>|;", c)) continue;
        clean[j++] = (char)c;
    }
    while (j > 0 && clean[j - 1] == ' ') j--;
    clean[j] = 0;
    if (!j) { set_status(g, "Digite um nome para o preset"); return; }
    MakeDirectory(PRESET_DIR);
    char path[300];
    snprintf(path, sizeof path, PRESET_DIR "/%s.txt", clean);
    if (preset_save(path, g->save_cat) == 0) {
        build_preset_list(g);
        for (int i = 0; i < g->n_user; i++)
            if (!strcmp(g->user_presets[i], clean)) g->preset_active = factory_preset_count() + i;
        snprintf(g->preset_name, sizeof g->preset_name, "%s", clean);
        char m[360];
        snprintf(m, sizeof m, "Preset salvo em %s", path);
        set_status(g, m);
    } else {
        set_status(g, "Não foi possível salvar o preset");
    }
}

/* ------------------------------------------------------------------ */
/* Configurações lembradas (configuracoes.ini, ao lado dos presets)    */
/* ------------------------------------------------------------------ */
static int find_item(const char *items, const char *name)
{
    int idx = 0;
    const char *p = items;
    size_t n = strlen(name);
    while (p && *p) {
        const char *e = strchr(p, ';');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == n && !strncmp(p, name, n)) return idx;
        p = e ? e + 1 : NULL;
        idx++;
    }
    return -1;
}

static void item_name(const char *items, int idx, char *buf, int len)
{
    const char *p = items;
    for (int i = 0; i < idx && p; i++) { p = strchr(p, ';'); if (p) p++; }
    if (!p) { buf[0] = 0; return; }
    const char *e = strchr(p, ';');
    snprintf(buf, len, "%.*s", e ? (int)(e - p) : (int)strlen(p), p);
}

static void save_settings(Gui *g)
{
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) return;
    char t[300];
    fprintf(f, "# Configurações do Sintetizador (gravadas ao fechar o programa)\n");
    item_name(g->out_items, g->out_active, t, sizeof t);   fprintf(f, "saida_audio = %s\n", t);
    item_name(g->in_items, g->in_active, t, sizeof t);     fprintf(f, "entrada_audio = %s\n", t);
    fprintf(f, "canal_entrada = %d\n", g->in_ch);
    item_name(g->midi_items, g->midi_active, t, sizeof t); fprintf(f, "midi_entrada = %s\n", t);
    item_name(g->mout_items, g->mout_active, t, sizeof t); fprintf(f, "midi_saida = %s\n", t);
    fprintf(f, "buffer = %d\n", g->e->period_frames);
    fprintf(f, "modo_exclusivo = %d\n", g->e->exclusive);
    fprintf(f, "preset_nome = %s\n", g->preset_name);
    fprintf(f, "preset_indice = %d\n", g->preset_active);
    fprintf(f, "aba = %d\n", g->tab);
    fprintf(f, "\n# Som atual e ajustes (mesmo formato dos presets)\n");
    params_write_all(f);
    fclose(f);
}

static void load_settings(Gui *g)
{
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ') k++;
        for (char *e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        while (*v == ' ') v++;
        for (char *e = v + strlen(v); e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); ) *--e = 0;
        int idx;
        if (!strcmp(k, "saida_audio"))        { if ((idx = find_item(g->out_items, v)) >= 0) g->out_active = idx; }
        else if (!strcmp(k, "entrada_audio")) { if ((idx = find_item(g->in_items, v)) >= 0) g->in_active = idx; }
        else if (!strcmp(k, "canal_entrada")) g->in_ch = atoi(v);
        else if (!strcmp(k, "midi_entrada"))  { if ((idx = find_item(g->midi_items, v)) >= 0) g->midi_active = idx; }
        else if (!strcmp(k, "midi_saida"))    { if ((idx = find_item(g->mout_items, v)) >= 0) g->mout_active = idx; }
        else if (!strcmp(k, "buffer"))        g->e->period_frames = atoi(v);
        else if (!strcmp(k, "modo_exclusivo")) g->e->exclusive = atoi(v);
        else if (!strcmp(k, "preset_nome"))   snprintf(g->preset_name, sizeof g->preset_name, "%s", v);
        else if (!strcmp(k, "preset_indice")) g->preset_active = atoi(v);
        else if (!strcmp(k, "aba"))           g->tab = atoi(v) % 6;
        else params_read_line(k, v);
    }
    fclose(f);
    /* o índice muda quando entram presets novos: o nome é quem manda */
    for (int i = 0; i < preset_total(g); i++)
        if (!strcmp(preset_name_of(g, i), g->preset_name)) { g->preset_active = i; return; }
    if (g->preset_active < 0 || g->preset_active >= preset_total(g)) g->preset_active = 0;
}

/* ------------------------------------------------------------------ */
/* Gravação                                                            */
/* ------------------------------------------------------------------ */
static void drain_recording(Gui *g)
{
    unsigned avail;
    while ((avail = fring_available(&g->e->rec_ring) & ~1u) > 0) {
        unsigned n = avail < 16384 ? avail : 16384;
        fring_read(&g->e->rec_ring, g->rec_buf, n);
        recorder_write(&g->rec, g->rec_buf, n / 2);
    }
    /* entrada da guitarra (diagnóstico): só existe no modo Guitarra */
    while ((avail = fring_available(&g->e->rec_gtr_ring) & ~1u) > 0) {
        unsigned n = avail < 16384 ? avail : 16384;
        fring_read(&g->e->rec_gtr_ring, g->rec_buf, n);
        if (!recorder_active(&g->rec_gtr)) {
            char path[600];
            snprintf(path, sizeof path, "%.*s-guitarra.wav", (int)strlen(g->rec.path) - 4, g->rec.path);
            if (recorder_start(&g->rec_gtr, path, (int)g->e->sr) < 0) continue;
            /* e os ajustes do momento, para reproduzir exatamente */
            snprintf(path, sizeof path, "%.*s-ajustes.txt", (int)strlen(g->rec.path) - 4, g->rec.path);
            FILE *f = fopen(path, "w");
            if (f) {
                fprintf(f, "# Ajustes usados na gravação (para diagnóstico)\n");
                fprintf(f, "entrada = %s\ncanal = %d\nbuffer = %d\n", g->e->cap_name, atomic_load(&g->e->cap_channel),
                        g->e->period_frames);
                params_write_all(f);
                fclose(f);
            }
        }
        recorder_write(&g->rec_gtr, g->rec_buf, n / 2);
    }
}

static void toggle_recording(Gui *g)
{
    if (!recorder_active(&g->rec)) {
        MakeDirectory(REC_DIR);
        char path[256], stamp[32];
        time_t t = time(NULL);
        strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&t));
        snprintf(path, sizeof path, REC_DIR "/gravacao-%s.wav", stamp);
        if (recorder_start(&g->rec, path, (int)g->e->sr) < 0) { set_status(g, "Não foi possível criar o arquivo WAV"); return; }
        fring_clear(&g->e->rec_ring);
        fring_clear(&g->e->rec_gtr_ring);
        atomic_store(&g->e->rec_on, true);
        g->rec_start = GetTime();
        set_status(g, "Gravando...");
    } else {
        atomic_store(&g->e->rec_on, false);
        drain_recording(g);
        char m[600];
        if (recorder_active(&g->rec_gtr))
            snprintf(m, sizeof m, "Gravação salva: %s (%.1f s) + entrada da guitarra em ...-guitarra.wav", g->rec.path,
                     g->rec.frames / g->e->sr);
        else snprintf(m, sizeof m, "Gravação salva: %s (%.1f s)", g->rec.path, g->rec.frames / g->e->sr);
        recorder_stop(&g->rec);
        recorder_stop(&g->rec_gtr);
        set_status(g, m);
    }
}

/* ------------------------------------------------------------------ */
/* Teclado do computador                                               */
/* ------------------------------------------------------------------ */
static const struct { int key, semi; } KEYMAP[] = {
    {KEY_Z, 0}, {KEY_S, 1}, {KEY_X, 2}, {KEY_D, 3}, {KEY_C, 4}, {KEY_V, 5}, {KEY_G, 6}, {KEY_B, 7},
    {KEY_H, 8}, {KEY_N, 9}, {KEY_J, 10}, {KEY_M, 11}, {KEY_COMMA, 12}, {KEY_L, 13}, {KEY_PERIOD, 14},
    {KEY_SEMICOLON, 15}, {KEY_SLASH, 16},
    {KEY_Q, 12}, {KEY_TWO, 13}, {KEY_W, 14}, {KEY_THREE, 15}, {KEY_E, 16}, {KEY_R, 17}, {KEY_FIVE, 18},
    {KEY_T, 19}, {KEY_SIX, 20}, {KEY_Y, 21}, {KEY_SEVEN, 22}, {KEY_U, 23}, {KEY_I, 24}, {KEY_NINE, 25},
    {KEY_O, 26}, {KEY_ZERO, 27}, {KEY_P, 28},
};
#define NKEYS (int)(sizeof KEYMAP / sizeof KEYMAP[0])

static void handle_keyboard(Gui *g)
{
    Engine *e = g->e;
    if (ui_typing() || g->save_open || g->audio_open || g->browser_open) return; /* digitando ou numa janela: as teclas não tocam */
    int base = 12 * (param_geti(P_KEYS_OCTAVE) + 1);
    for (int i = 0; i < NKEYS; i++) {
        if (IsKeyPressed(KEYMAP[i].key)) {
            int note = base + KEYMAP[i].semi;
            if (note <= 127) { g->key_note[i] = note; engine_ui_event(e, EV_NOTE_ON, note, 100); }
        }
        if (IsKeyReleased(KEYMAP[i].key) && g->key_note[i] >= 0) {
            engine_ui_event(e, EV_NOTE_OFF, g->key_note[i], 0);
            g->key_note[i] = -1;
        }
    }
    if (IsKeyPressed(KEY_LEFT))  param_set(P_KEYS_OCTAVE, param_get(P_KEYS_OCTAVE) - 1);
    if (IsKeyPressed(KEY_RIGHT)) param_set(P_KEYS_OCTAVE, param_get(P_KEYS_OCTAVE) + 1);
    if (IsKeyPressed(KEY_SPACE))  engine_ui_event(e, EV_CC, 64, 127);
    if (IsKeyReleased(KEY_SPACE)) engine_ui_event(e, EV_CC, 64, 0);
    if (IsKeyPressed(KEY_ENTER) && !g->save_open) looper_command(&e->looper, LCMD_REC);
}

/* ------------------------------------------------------------------ */
/* Piano                                                               */
/* ------------------------------------------------------------------ */
#define PIANO_LOW  36 /* C2 */
#define PIANO_HIGH 96 /* C7 */

static bool is_black(int n) { int k = n % 12; return k == 1 || k == 3 || k == 6 || k == 8 || k == 10; }

static Rectangle key_rect(Rectangle area, int note, float ww)
{
    int whites = 0;
    for (int n = PIANO_LOW; n < note; n++) if (!is_black(n)) whites++;
    if (!is_black(note)) return (Rectangle){area.x + whites * ww, area.y, ww - 2, area.height};
    float bw = ww * 0.62f;
    return (Rectangle){area.x + whites * ww - bw / 2 - 1, area.y, bw, area.height * 0.6f};
}

static int piano_hit(Rectangle area, Vector2 m, float ww)
{
    if (!CheckCollisionPointRec(m, area)) return -1;
    for (int n = PIANO_LOW; n <= PIANO_HIGH; n++)
        if (is_black(n) && CheckCollisionPointRec(m, key_rect(area, n, ww))) return n;
    for (int n = PIANO_LOW; n <= PIANO_HIGH; n++)
        if (!is_black(n) && CheckCollisionPointRec(m, key_rect(area, n, ww))) return n;
    return -1;
}

static void draw_piano(Gui *g, Rectangle area)
{
    Engine *e = g->e;
    int whites = 0;
    for (int n = PIANO_LOW; n <= PIANO_HIGH; n++) if (!is_black(n)) whites++;
    float ww = (area.width + 2) / whites;

    /* faixa azul: onde está o teclado do computador */
    int base = 12 * (param_geti(P_KEYS_OCTAVE) + 1);
    int lo = base < PIANO_LOW ? PIANO_LOW : base, hi = base + 28 > PIANO_HIGH ? PIANO_HIGH : base + 28;
    if (lo <= PIANO_HIGH) {
        Rectangle a = key_rect(area, lo, ww), b = key_rect(area, hi, ww);
        ui_rect((Rectangle){a.x, area.y - 9, b.x + b.width - a.x, 4}, 2, Fade(C_CYAN, 0.8f));
    }

    /* mouse: clica e arrasta (glissando); mais embaixo na tecla = mais forte */
    Vector2 m = ui_mouse();
    bool can = ui_hover(area) || g->mouse_note >= 0;
    if (can && IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !ui_blocked()) {
        int n = (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || g->mouse_note >= 0) ? piano_hit(area, m, ww) : -1;
        if (n != g->mouse_note) {
            if (g->mouse_note >= 0) engine_ui_event(e, EV_NOTE_OFF, g->mouse_note, 0);
            if (n >= 0) {
                Rectangle kr = key_rect(area, n, ww);
                int vel = 50 + (int)(77 * (m.y - kr.y) / kr.height);
                engine_ui_event(e, EV_NOTE_ON, n, vel > 127 ? 127 : vel);
            }
            g->mouse_note = n;
        }
    } else if (g->mouse_note >= 0) {
        engine_ui_event(e, EV_NOTE_OFF, g->mouse_note, 0);
        g->mouse_note = -1;
    }

    unsigned char lit[128] = {0};
    for (int i = 0; i < 128; i++) lit[i] = atomic_load(&e->notes_on[i]);
    for (int i = 0; i < NKEYS; i++) if (g->key_note[i] >= 0) lit[g->key_note[i]] = 1;
    if (g->mouse_note >= 0) lit[g->mouse_note] = 1;

    for (int pass = 0; pass < 2; pass++) {
        for (int n = PIANO_LOW; n <= PIANO_HIGH; n++) {
            if (is_black(n) != (pass == 1)) continue;
            Rectangle r = key_rect(area, n, ww);
            if (!pass) {
                Color c = lit[n] ? C_AMBER : (Color){233, 235, 238, 255};
                DrawRectangleRec((Rectangle){r.x, r.y, r.width, r.height - 7}, c);
                ui_rect((Rectangle){r.x, r.y + r.height - 14, r.width, 14}, 6, c);
                if (n % 12 == 0) {
                    char nm[8];
                    note_name(n, nm, sizeof nm);
                    ui_text_center(nm, (Rectangle){r.x, r.y + r.height - 24, r.width, 14}, 10.5f, UI_SEMIBOLD,
                                   lit[n] ? (Color){42, 26, 2, 255} : (Color){107, 115, 128, 255});
                }
            } else {
                Color c = lit[n] ? (Color){196, 127, 15, 255} : (Color){26, 29, 34, 255};
                DrawRectangleRec((Rectangle){r.x, r.y + 2, r.width, r.height - 2}, BLACK);
                DrawRectangleRec((Rectangle){r.x, r.y, r.width, r.height - 6}, c);
                ui_rect((Rectangle){r.x, r.y + r.height - 14, r.width, 10}, 5, c);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Desenhos de apoio (curvas e medidores)                              */
/* ------------------------------------------------------------------ */
static void well(Rectangle r) { ui_rect(r, 8, C_WELL); }

static void draw_polyline(const Vector2 *p, int n, float thick, Color c)
{
    for (int i = 1; i < n; i++) DrawLineEx(p[i - 1], p[i], thick, c);
}

/* Área sob a curva: dois triângulos por trecho (desenhados nos dois
   sentidos, porque o raylib descarta os que estão "de costas") */
static void fill_under(const Vector2 *p, int n, float bottom, Color c)
{
    for (int i = 1; i < n; i++) {
        Vector2 a = p[i - 1], b = p[i], bb = {b.x, bottom}, ab = {a.x, bottom};
        DrawTriangle(a, ab, bb, c); DrawTriangle(a, bb, ab, c);
        DrawTriangle(a, bb, b, c);  DrawTriangle(a, b, bb, c);
    }
}

static void draw_filter_curve(Rectangle r)
{
    well(r);
    float fc = param_get(P_CUTOFF), k = 2.0f - 1.95f * param_get(P_RESONANCE);
    Vector2 p[96];
    for (int i = 0; i < 96; i++) {
        float f = 20.0f * powf(1000.0f, i / 95.0f), x = f / fc;
        float mag = 1.0f / sqrtf((1 - x * x) * (1 - x * x) + (k * x) * (k * x));
        float db = 20.0f * log10f(mag);
        p[i] = (Vector2){r.x + r.width * i / 95.0f, clampf(r.y + r.height * 0.36f - db * 1.3f, r.y + 4, r.y + r.height - 2)};
    }
    fill_under(p, 96, r.y + r.height, Fade(C_AMBER, 0.12f));
    draw_polyline(p, 96, 2, C_AMBER);
    float cx = r.x + r.width * logf(fc / 20.0f) / logf(1000.0f);
    for (float y = r.y + 6; y < r.y + r.height - 6; y += 6) DrawLineEx((Vector2){cx, y}, (Vector2){cx, y + 3}, 1, Fade(C_AMBER, 0.5f));
}

static void draw_adsr(Rectangle r, int pa, int pd, int ps, int pr)
{
    well(r);
    float a = param_get(pa), d = param_get(pd), s = param_get(ps), rel = param_get(pr), hold = 0.25f;
    /* escala com raiz: tempos curtos e longos aparecem bem */
    float wa = sqrtf(a), wd = sqrtf(d), wr = sqrtf(rel), wh = sqrtf(hold);
    float k = (r.width - 12) / (wa + wd + wh + wr);
    float top = r.y + 8, bot = r.y + r.height - 6, ys = bot - (bot - top) * s;
    Vector2 p[5] = {{r.x + 6, bot}, {r.x + 6 + wa * k, top}, {r.x + 6 + (wa + wd) * k, ys},
                    {r.x + 6 + (wa + wd + wh) * k, ys}, {r.x + 6 + (wa + wd + wh + wr) * k, bot}};
    fill_under(p, 5, bot, Fade(C_AMBER, 0.12f));
    draw_polyline(p, 5, 2, C_AMBER);
}

static void draw_lfo(Rectangle r)
{
    well(r);
    DrawLineEx((Vector2){r.x, r.y + r.height / 2}, (Vector2){r.x + r.width, r.y + r.height / 2}, 1, (Color){30, 35, 43, 255});
    float rate = param_get(P_LFO_RATE), ph = (float)GetTime() * rate;
    float cycles = clampf(rate * 0.6f, 1.5f, 8.0f);
    Vector2 p[120];
    for (int i = 0; i < 120; i++) {
        float t = i / 119.0f;
        p[i] = (Vector2){r.x + 4 + (r.width - 8) * t, r.y + r.height / 2 - sinf((t * cycles - ph) * 2 * PI) * r.height * 0.32f};
    }
    draw_polyline(p, 120, 2, C_AMBER);
}

static void draw_scope(Gui *g, Rectangle r)
{
    ui_panel(r, 12, C_WELL, C_BORDER);
    DrawLineEx((Vector2){r.x + 8, r.y + r.height / 2}, (Vector2){r.x + r.width - 8, r.y + r.height / 2}, 1, (Color){28, 33, 40, 255});
    static float buf[SCOPE_SIZE];
    unsigned end = atomic_load(&g->e->scope_pos);
    for (int i = 0; i < SCOPE_SIZE; i++) buf[i] = atomic_load_explicit(&g->e->scope[(end + i) & (SCOPE_SIZE - 1)], memory_order_relaxed);
    int start = 0;
    for (int i = 1; i < SCOPE_SIZE / 2; i++) if (buf[i - 1] <= 0 && buf[i] > 0) { start = i; break; }
    int n = SCOPE_SIZE / 2;
    float gain = 0;
    for (int i = 0; i < n; i++) gain = fmaxf(gain, fabsf(buf[start + i]));
    gain = gain > 0.02f ? 0.42f / gain : 0.42f / 0.02f;
    Vector2 prev = {0};
    for (int i = 0; i < n; i += 2) {
        Vector2 p = {r.x + 8 + (r.width - 16) * i / (n - 1), r.y + r.height / 2 - buf[start + i] * gain * r.height};
        if (i) DrawLineEx(prev, p, 1.6f, C_AMBER);
        prev = p;
    }
    ui_caption("OSCILOSCÓPIO", r.x + 12, r.y + 7, C_TEXT2);
}

/* ------------------------------------------------------------------ */
/* Barra superior                                                      */
/* ------------------------------------------------------------------ */
static void draw_logo(float x, float y)
{
    ui_panel((Rectangle){x, y, 28, 28}, 8, (Color){30, 34, 41, 255}, (Color){46, 52, 64, 255});
    Vector2 p[24];
    for (int i = 0; i < 24; i++) {
        float t = i / 23.0f;
        p[i] = (Vector2){x + 6 + 16 * t, y + 14 - sinf(t * 2 * PI) * 6 * (1 - 0.3f * t)};
    }
    draw_polyline(p, 24, 2.2f, C_AMBER);
}

static void open_browser(Gui *g);

static void draw_top_bar(Gui *g)
{
    DrawRectangle(0, 0, UI_W, 56, C_BAR);
    DrawRectangle(0, 56, UI_W, 1, (Color){31, 36, 44, 255});
    draw_logo(16, 14);
    ui_text("Virtual Synth", 54, 18, 15, UI_BOLD, C_TEXT);

    /* seletor de preset: ‹ [TAG Nome ▾] › */
    Rectangle grp = {212, 10, 320, 36};
    ui_panel(grp, 10, (Color){26, 30, 37, 255}, C_BORDER2);
    int total = preset_total(g);
    if (ui_button_icon((Rectangle){grp.x + 1, grp.y + 1, 34, 34}, "chev-left", "Preset anterior"))
        load_preset(g, (g->preset_active - 1 + total) % total);
    if (ui_button_icon((Rectangle){grp.x + grp.width - 35, grp.y + 1, 34, 34}, "chev-right", "Próximo preset"))
        load_preset(g, (g->preset_active + 1) % total);
    ui_tooltip((Rectangle){grp.x + 1, grp.y + 1, 34, 34}, "Preset anterior");
    ui_tooltip((Rectangle){grp.x + grp.width - 35, grp.y + 1, 34, 34}, "Próximo preset");
    Rectangle mid = {grp.x + 36, grp.y + 1, grp.width - 72, 34};
    const char *cat = preset_cat_tag(preset_cat_of(g, g->preset_active));
    float cw = ui_text_width(cat, 10, UI_BOLD) + 12;
    if (ui_clicked(mid)) {
        g->browser_open = !g->browser_open;
        if (g->browser_open) open_browser(g);
    }
    bool hov = ui_hover(mid) || g->browser_open;
    ui_rect((Rectangle){mid.x + 1, mid.y + 1, mid.width - 2, mid.height - 2}, 8, hov ? C_CTRL_HOV : C_CTRL);
    Rectangle tag = {mid.x + 10, mid.y + 9, cw, 17};
    ui_rect(tag, 5, C_AMBER_BG);
    ui_text_center(cat, tag, 10, UI_BOLD, C_AMBER);
    ui_text(g->preset_name, tag.x + cw + 8, mid.y + 9, 13, UI_SEMIBOLD, C_TEXT);
    ui_icon(g->browser_open ? "chev-up" : "chev-down", mid.x + mid.width - 24, mid.y + 10, 14, C_TEXT2);
    ui_tooltip(mid, "Abre a lista de presets, separada por categoria.");

    if (ui_button((Rectangle){542, 10, 92, 36}, "@save Salvar", UI_BTN_NORMAL)) {
        g->save_open = !g->save_open;
        g->save_name[0] = 0; /* vazio: Enter salva com o nome atual */
        g->save_cat = preset_cat_of(g, g->preset_active);
        if (g->save_cat == 0) g->save_cat = PRESET_CAT_USER;
        g->browser_open = false;
        if (g->save_open) ui_focus(TB_SAVE);
    }
    ui_tooltip((Rectangle){542, 10, 92, 36}, "Salva o som atual como um preset seu (pasta presets/).");

    /* lado direito */
    float x = 1268;
    /* volume geral + medidor */
    float peak = atomic_load(&g->e->peak);
    g->out_level = fmaxf(peak, g->out_level * 0.9f);
    float db = 20 * log10f(g->out_level + 1e-6f);
    ui_meter((Rectangle){x - 82, 22, 82, 12}, 16, clampf((db + 48) / 48, 0, 1), 0.62f, 0.9f);
    ui_tooltip((Rectangle){x - 82, 18, 82, 20}, "Nível de saída. Se acender o vermelho, baixe o volume.");
    x -= 82 + 12;
    ui_knob_draw(x - 17, 28, 34, param_get_norm(P_MASTER), C_TEXT, false);
    {
        /* knob pequeno sem rótulo: interação própria */
        Rectangle hit = {x - 36, 8, 38, 40};
        static float drag_y, drag_v;
        static bool dragging;
        if (ui_learn()) ui_learn_zone(hit, P_MASTER);
        else {
            if (ui_pressed(hit)) { dragging = true; drag_y = ui_mouse().y; drag_v = param_get_norm(P_MASTER); }
            if (dragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) param_set_norm(P_MASTER, drag_v + (drag_y - ui_mouse().y) * 0.006f);
            else dragging = false;
            if (ui_hover(hit)) param_set_norm(P_MASTER, param_get_norm(P_MASTER) + GetMouseWheelMove() * 0.025f);
        }
        char t[128];
        snprintf(t, sizeof t, "Volume geral: %.0f%%. Arraste para cima/baixo ou use a roda do mouse.", param_get(P_MASTER) * 100);
        ui_tooltip(hit, t);
    }
    x -= 36 + 14;
    DrawRectangle((int)x + 6, 14, 1, 28, (Color){31, 36, 44, 255});
    x -= 6;

    Rectangle pan = {x - 84, 10, 84, 36};
    if (ui_button(pan, "Pânico", UI_BTN_NORMAL)) {
        Event off = {EV_ALL_OFF, SRC_KEYS, 0, 0, 0};
        evq_push(&g->e->q_ui, off);
        looper_command(&g->e->looper, LCMD_STOP);
        set_status(g, "Todas as notas foram silenciadas");
    }
    ui_tooltip(pan, "Silencia todas as notas presas e para o looper.");
    x -= 84 + 8;

    bool rec = recorder_active(&g->rec);
    char rl[48];
    if (rec) {
        double s = GetTime() - g->rec_start;
        snprintf(rl, sizeof rl, "Gravando %02d:%02d", (int)s / 60, (int)s % 60);
    } else snprintf(rl, sizeof rl, "@rec Gravar");
    Rectangle rb = {x - 138, 10, 138, 36};
    if (ui_button(rb, rl, rec ? UI_BTN_DANGER : UI_BTN_NORMAL)) toggle_recording(g);
    if (rec) ui_led(rb.x + 16, rb.y + 18, 5, (int)(GetTime() * 2) % 2 == 0, C_RED);
    ui_tooltip(rb, rec ? "Clique para parar e salvar o WAV." : "Grava tudo o que sai (sem o metrônomo) em gravacoes/. No modo Guitarra, grava também o som cru da guitarra (...-guitarra.wav).");
    x -= 138 + 8;

    Rectangle lb = {x - 124, 10, 124, 36};
    if (ui_button(lb, "@midi MIDI Learn", ui_learn() ? UI_BTN_AMBER : UI_BTN_NORMAL)) {
        ui_set_learn(!ui_learn());
        midi_last_cc(&g->learn_seen);
        set_status(g, ui_learn() ? "MIDI learn: clique num controle e gire um knob do seu controlador (botão direito desfaz)"
                                 : "MIDI learn desligado");
    }
    ui_tooltip(lb, "Liga um knob do seu controlador MIDI a qualquer controle da tela.");
    x -= 124 + 8;

    Rectangle ab = {x - 36, 10, 36, 36};
    if (ui_button(ab, "@gear", g->audio_open ? UI_BTN_CYAN : UI_BTN_NORMAL)) {
        g->audio_open = !g->audio_open;
        g->browser_open = false;
    }
    ui_tooltip(ab, "Configurações de áudio: saída, buffer e latência.");
}

/* ------------------------------------------------------------------ */
/* Painel de entrada                                                   */
/* ------------------------------------------------------------------ */
static void draw_tuner(Gui *g, Rectangle r)
{
    Engine *e = g->e;
    float hz = atomic_load(&e->guitar.st_hz);
    double now = GetTime();
    if (hz > 0) {
        float midi = hz_to_midi(hz);
        int n = (int)lroundf(midi);
        if (n != g->tuner_note) g->tuner_cents = (midi - n) * 100.0f;
        g->tuner_note  = n;
        g->tuner_cents += ((midi - n) * 100.0f - g->tuner_cents) * 0.3f;
        g->tuner_hz    = hz;
        g->tuner_time  = now;
    }
    bool fresh = g->tuner_time > 0 && now - g->tuner_time < 0.6;
    bool tuned = fresh && fabsf(g->tuner_cents) < 5;
    ui_panel(r, 12, C_WELL, C_BORDER);
    ui_caption("AFINADOR", r.x + 12, r.y + 12, C_TEXT2);
    if (fresh) {
        const char *chip = tuned ? "afinado" : (g->tuner_cents < 0 ? "baixo" : "alto");
        float cw = ui_text_width(chip, 11, UI_SEMIBOLD) + 16;
        Rectangle c = {r.x + r.width - cw - 10, r.y + 9, cw, 20};
        ui_rect(c, 6, tuned ? (Color){18, 41, 31, 255} : (Color){42, 34, 19, 255});
        ui_text_center(chip, c, 11, UI_SEMIBOLD, tuned ? C_GREEN : C_AMBER);
    }
    char nm[8] = "--", oct[12] = "";
    if (fresh) {
        static const char *names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        snprintf(nm, sizeof nm, "%s", names[((g->tuner_note % 12) + 12) % 12]);
        snprintf(oct, sizeof oct, "%d", g->tuner_note / 12 - 1);
    }
    Color nc = !fresh ? C_TEXT2 : (tuned ? C_GREEN : C_TEXT);
    ui_text(nm, r.x + 12, r.y + 26, 30, UI_BOLD, nc);
    float nw = ui_text_width(nm, 30, UI_BOLD);
    ui_text(oct, r.x + 14 + nw, r.y + 40, 15, UI_SEMIBOLD, nc);
    if (fresh) {
        char t[48];
        snprintf(t, sizeof t, "%.1f Hz · %+.0f cents", g->tuner_hz, g->tuner_cents);
        ui_text(t, r.x + 72, r.y + 41, 12.5f, UI_REGULAR, C_TEXT2);
    }
    /* régua de cents com ponteiro */
    Rectangle bar = {r.x + 14, r.y + r.height - 26, r.width - 28, 18};
    for (int i = 0; i <= 20; i++) {
        float x = bar.x + bar.width * i / 20.0f;
        float h = i == 10 ? 10 : (i % 5 == 0 ? 8 : 5);
        DrawRectangleRec((Rectangle){x - 1, bar.y + 12 - h, 2, h}, (i >= 9 && i <= 11) ? C_GREEN : (Color){58, 65, 77, 255});
    }
    if (fresh) {
        float c = clampf(g->tuner_cents, -50, 50);
        float x = bar.x + bar.width * (0.5f + c / 100.0f);
        Color pc = tuned ? C_GREEN : C_AMBER;
        DrawRectangleRec((Rectangle){x - 6, bar.y - 4, 12, 22}, Fade(pc, 0.15f));
        ui_rect((Rectangle){x - 2, bar.y - 4, 4, 22}, 2, pc);
    }
}

static void input_guitar(Gui *g, Rectangle p)
{
    Engine *e = g->e;
    float x = p.x + 14, w = p.width - 28, y = p.y + 92;
    ui_text("Interface de áudio", x, y, 11, UI_REGULAR, C_TEXT2);
    ui_text("Canal", x + w - 64, y, 11, UI_REGULAR, C_TEXT2);
    if (out_is_asio(g)) {
        /* no ASIO a entrada é a do mesmo driver da saída */
        Rectangle lr = {x, y + 17, w - 72, 34};
        ui_rect(lr, 9, C_WELL);
        char nm[256];
        snprintf(nm, sizeof nm, "%s", g->e->play[g->out_active - 1].name);
        while (strlen(nm) > 4 && ui_text_width(nm, 12.5f, UI_REGULAR) > w - 92) strcpy(nm + strlen(nm) - 4, "...");
        ui_text(nm, x + 10, y + 26, 12.5f, UI_REGULAR, C_TEXT2);
        ui_tooltip(lr, "Com ASIO, a entrada é a do mesmo driver escolhido como saída (engrenagem lá em cima).");
    } else if (ui_dropdown((Rectangle){x, y + 17, w - 72, 34}, g->in_items, &g->in_active, DD_IN)) apply_audio(g);
    if (ui_dropdown((Rectangle){x + w - 64, y + 17, 64, 34}, g->ch_items, &g->in_ch, DD_CH)) atomic_store(&e->cap_channel, g->in_ch);
    ui_tooltip((Rectangle){x + w - 64, y + 17, 64, 34}, "Em qual entrada da interface a guitarra está ligada.");

    y += 64;
    float db = atomic_load(&e->guitar.st_db), gate = param_get(P_GTR_GATE);
    char t[32];
    ui_text("Nível", x, y, 11, UI_REGULAR, C_TEXT2);
    snprintf(t, sizeof t, db < -90 ? "--" : "%.0f dB", db);
    ui_text(t, x + 36, y, 11.5f, UI_SEMIBOLD, C_TEXT);

    /* calibrar: mede 2 s da guitarra parada (dentro do áudio, a cada hop)
       e põe o gate 10 dB acima do ruído típico */
    double now = GetTime();
    Rectangle cb = {x + w - 112, y - 6, 112, 24};
    char cl[32];
    if (g->calibrating) snprintf(cl, sizeof cl, "Medindo... %d", (int)ceil(2.0 - (now - g->calib_start)));
    else snprintf(cl, sizeof cl, "Calibrar gate");
    if (ui_button(cb, cl, g->calibrating ? UI_BTN_AMBER : UI_BTN_NORMAL) && !g->calibrating) {
        if (engine_capture_channels(e) == 0)
            set_status(g, "Sem entrada de áudio aberta: escolha a entrada da guitarra antes de calibrar.");
        else {
            guitar_calib_start(&e->guitar, 2.0f);
            g->calibrating = 1;
            g->calib_start = now;
            set_status(g, "Calibrando: não toque e deixe as cordas paradas por 2 segundos...");
        }
    }
    ui_tooltip(cb, "Mede o ruído da sua interface com a guitarra parada e põe o gate 10 dB acima dele.");
    if (g->calibrating) {
        float noise, ng;
        if (guitar_calib_result(&e->guitar, &noise, &ng)) {
            g->calibrating = 0;
            char msg[200];
            param_set(P_GTR_GATE, ng);
            if (noise > -25.0f)
                snprintf(msg, sizeof msg, "Calibração: entrada muito alta (%.0f dB). Tocou durante a medida? Gate em %.0f dB; tente de novo.", noise, ng);
            else if (noise + 10.0f < -70.0f)
                snprintf(msg, sizeof msg, "Calibração: entrada quase sem ruído (%.0f dB). Gate no mínimo, %.0f dB: as notas soam até o fim.", noise, ng);
            else
                snprintf(msg, sizeof msg, "Calibração: ruído de %.0f dB, gate ajustado para %.0f dB.", noise, ng);
            set_status(g, msg);
        } else if (now - g->calib_start > 4.0) { /* o áudio parou no meio */
            g->calibrating = 0;
            set_status(g, "Calibração não terminou: a entrada de áudio parou. Confira o dispositivo.");
        }
    }
    Rectangle m = {x, y + 18, w, 12};
    ui_meter(m, 30, clampf((db + 70) / 70, 0, 1), 0.76f, 0.9f);
    float gx = m.x + m.width * clampf((gate + 70) / 70, 0, 1);
    ui_rect((Rectangle){gx - 1, m.y - 4, 3, 20}, 1, C_AMBER);
    ui_text("−70", x, y + 34, 10, UI_REGULAR, C_TEXT2);
    ui_text_center("gate", (Rectangle){gx - 30, y + 34, 60, 12}, 10, UI_SEMIBOLD, C_AMBER);
    ui_text_right("0 dB", x + w, y + 34, 10, UI_REGULAR, C_TEXT2);
    ui_tooltip((Rectangle){x, y, w, 46}, "Nível da guitarra. O traço laranja é o gate: notas só começam acima dele.");

    y += 80;
    ui_knob(x + w * 0.17f, y, 44, P_GTR_GATE, C_CYAN, "Gate");
    ui_knob(x + w * 0.5f, y, 44, P_GTR_GAIN, C_CYAN, "Ganho");
    ui_knob_bipolar(x + w * 0.83f, y, 44, P_GTR_BLEND, C_CYAN, "Mistura");
    ui_tooltip((Rectangle){x + w * 0.83f - 30, y - 26, 60, 90}, "Mistura o som limpo da guitarra com o synth: à esquerda só synth, à direita só guitarra.");

    y += 60;
    /* Como o synth segue a guitarra: reconhecendo notas ou direto na onda */
    ui_text("Synth", x, y + 9, 11, UI_REGULAR, C_TEXT2);
    Rectangle ms = {x + 46, y, w - 46, 30};
    int dm = param_geti(P_GTR_DIRECT);
    if (ui_segmented(ms, "Notas;Direto", &dm, C_CYAN, C_CYAN_BG, C_CYAN_HI)) {
        param_set(P_GTR_DIRECT, (float)dm);
        set_status(g, dm ? "Modo direto: o som sai da própria corda, sem atraso. MIDI e harmonizador funcionam; com o arpejador, ele toca no lugar da corda."
                         : "Modo notas: reconhece cada nota e o synth a toca (MIDI, harmonizador e arpejador funcionam).");
    }
    ui_tooltip(ms, param_help(P_GTR_DIRECT));

    y += 38;
    ui_text("Oitava", x, y + 8, 11, UI_REGULAR, C_TEXT2);
    Rectangle st = {x + 46, y, 86, 30};
    ui_panel(st, 9, C_CTRL, C_BORDER2);
    if (ui_button_icon((Rectangle){st.x + 2, st.y + 2, 26, 26}, "minus", "Oitava abaixo")) param_set(P_GTR_OCTAVE, param_get(P_GTR_OCTAVE) - 1);
    if (ui_button_icon((Rectangle){st.x + st.width - 28, st.y + 2, 26, 26}, "plus", "Oitava acima")) param_set(P_GTR_OCTAVE, param_get(P_GTR_OCTAVE) + 1);
    snprintf(t, sizeof t, "%+d", param_geti(P_GTR_OCTAVE));
    ui_text_center(param_geti(P_GTR_OCTAVE) ? t : "0", st, 12.5f, UI_SEMIBOLD, C_TEXT);
    ui_tooltip(st, dm ? "Toca o synth uma ou duas oitavas acima ou abaixo da guitarra (no modo direto, entra assim que a nota é confirmada)."
                      : "Toca o synth uma ou duas oitavas acima ou abaixo da guitarra.");
    if (!dm) {
        ui_text_right("Bends", x + w - 52, y + 8, 12, UI_REGULAR, C_TEXT);
        ui_param_switch((Rectangle){x + w - 44, y + 3, 44, 24}, P_GTR_BENDS, C_CYAN);
        ui_tooltip((Rectangle){x + w - 110, y, 110, 30}, "O synth acompanha bends e vibrato da corda.");

        y += 36;
        ui_text("Ataque imediato", x, y + 4, 12, UI_REGULAR, C_TEXT);
        ui_text(param_geti(P_GTR_FAST) ? "som já na palhetada" : "som quando a nota é reconhecida", x, y + 20, 10.5f,
                UI_REGULAR, C_TEXT2);
        ui_param_switch((Rectangle){x + w - 44, y + 5, 44, 24}, P_GTR_FAST, C_CYAN);
        ui_tooltip((Rectangle){x, y, w - 54, 34}, param_help(P_GTR_FAST));
    } else {
        y += 36;
        ui_text("Segue a onda da corda na hora:", x, y + 4, 11, UI_REGULAR, C_TEXT2);
        ui_text("bends e vibrato vêm junto; acorde vira fuzz.", x, y + 19, 11, UI_REGULAR, C_TEXT2);
    }

    draw_tuner(g, (Rectangle){x, p.y + p.height - 84, w, 74});
}

static void input_keys(Gui *g, Rectangle p)
{
    (void)g;
    float x = p.x + 14, y = p.y + 96;
    ui_text("Toque pelo teclado do computador:", x, y, 12.5f, UI_SEMIBOLD, C_TEXT);
    y += 26;
    static const char *whites[2] = {"QWERTYUIOP", "ZXCVBNM,./"};
    static const char *blacks[2] = {"23 567 90", "SD GHJ L;"};
    for (int rr = 0; rr < 2; rr++) {
        float ky = y + rr * 64;
        for (int i = 0; blacks[rr][i]; i++) {
            if (blacks[rr][i] == ' ') continue;
            Rectangle k = {x + 4 + i * 26.0f + 13, ky, 23, 24};
            ui_panel(k, 5, (Color){26, 29, 34, 255}, C_BORDER2);
            char c[2] = {blacks[rr][i], 0};
            ui_text_center(c, k, 11, UI_SEMIBOLD, C_TEXT);
        }
        for (int i = 0; whites[rr][i]; i++) {
            Rectangle k = {x + 4 + i * 26.0f, ky + 28, 23, 26};
            ui_rect(k, 5, (Color){233, 235, 238, 255});
            char c[2] = {whites[rr][i], 0};
            ui_text_center(c, k, 11, UI_SEMIBOLD, (Color){30, 30, 30, 255});
        }
    }
    y += 140;
    static const char *tips[][2] = {{"← →", "muda a oitava"}, {"Espaço", "pedal de sustain"}, {"Enter", "botão do looper"}};
    for (int i = 0; i < 3; i++) {
        float tw = ui_text_width(tips[i][0], 11, UI_SEMIBOLD) + 14;
        Rectangle k = {x, y + i * 28.0f, tw, 21};
        ui_panel(k, 5, C_CTRL, C_BORDER2);
        ui_text_center(tips[i][0], k, 11, UI_SEMIBOLD, C_TEXT);
        ui_text(tips[i][1], x + tw + 10, y + i * 28.0f + 3, 12, UI_REGULAR, C_TEXT2);
    }
    y += 96;
    ui_text("Oitava do teclado", x, y + 8, 12, UI_REGULAR, C_TEXT2);
    Rectangle st = {x + p.width - 28 - 98, y, 98, 30};
    ui_panel(st, 9, C_CTRL, C_BORDER2);
    if (ui_button_icon((Rectangle){st.x + 2, st.y + 2, 26, 26}, "minus", "Oitava abaixo")) param_set(P_KEYS_OCTAVE, param_get(P_KEYS_OCTAVE) - 1);
    if (ui_button_icon((Rectangle){st.x + st.width - 28, st.y + 2, 26, 26}, "plus", "Oitava acima")) param_set(P_KEYS_OCTAVE, param_get(P_KEYS_OCTAVE) + 1);
    char t[8];
    snprintf(t, sizeof t, "C%d", param_geti(P_KEYS_OCTAVE));
    ui_text_center(t, st, 12.5f, UI_SEMIBOLD, C_TEXT);
    y += 44;
    ui_text("Ou clique no piano lá embaixo:", x, y, 12, UI_REGULAR, C_TEXT2);
    ui_text("mais embaixo na tecla = mais forte.", x, y + 18, 12, UI_REGULAR, C_TEXT2);
}

static void input_midi(Gui *g, Rectangle p)
{
    float x = p.x + 14, w = p.width - 28, y = p.y + 92;
    ui_text("Dispositivo MIDI", x, y, 11, UI_REGULAR, C_TEXT2);
    if (ui_dropdown((Rectangle){x, y + 17, w - 44, 34}, g->midi_items, &g->midi_active, DD_MIDI)) apply_midi(g);
    if (ui_button((Rectangle){x + w - 38, y + 17, 38, 34}, "@refresh", UI_BTN_NORMAL)) refresh_all_devices(g);
    ui_tooltip((Rectangle){x + w - 38, y + 17, 38, 34}, "Procura de novo os dispositivos MIDI (ligou um agora?).");
    y += 66;
    bool open = midi_is_open();
    ui_led(x + 5, y + 8, 4, open, C_GREEN);
    ui_text(!midi_available() ? "Compilado sem MIDI" : (open ? "Conectado" : "Nenhum dispositivo conectado"), x + 16, y, 12.5f, UI_SEMIBOLD, open ? C_GREEN : C_TEXT2);
    y += 34;
    static const char *lines[][2] = {
        {"Pitch bend", "±2 semitons"}, {"Pedal (CC64)", "sustain"}, {"Roda (CC1)", "vibrato"},
        {"CC74", "corte do filtro"}, {"CC71", "ressonância"}, {"CC7", "volume geral"},
    };
    for (int i = 0; i < 6; i++) {
        ui_text(lines[i][0], x, y + i * 24.0f, 12, UI_SEMIBOLD, C_TEXT);
        ui_text(lines[i][1], x + 112, y + i * 24.0f, 12, UI_REGULAR, C_TEXT2);
    }
    y += 6 * 24 + 18;
    ui_panel((Rectangle){x, y, w, 66}, 10, C_WELL, C_BORDER);
    ui_text("Quer usar outros knobs?", x + 12, y + 12, 12, UI_SEMIBOLD, C_TEXT);
    ui_text("Ligue o MIDI Learn (barra de cima),", x + 12, y + 30, 12, UI_REGULAR, C_TEXT2);
    ui_text("clique num controle e gire o knob.", x + 12, y + 46, 12, UI_REGULAR, C_TEXT2);
}

static void draw_input_panel(Gui *g)
{
    Rectangle p = {12, 68, 300, 496};
    ui_panel(p, 16, C_PANEL, C_BORDER);
    ui_caption("ENTRADA", p.x + 14, p.y + 16, C_TEXT2);
    int mode = param_geti(P_INPUT_MODE);
    bool ok = g->e->dev_ok && (mode != 1 || midi_is_open());
    ui_led(p.x + p.width - 84, p.y + 21, 3.5f, ok, C_GREEN);
    ui_text(ok ? "conectada" : "sem sinal", p.x + p.width - 74, p.y + 14, 11, UI_REGULAR, C_TEXT2);
    ui_tooltip((Rectangle){p.x + 14, p.y + 40, p.width - 28, 38}, "Como você vai tocar: teclado do computador, teclado MIDI ou guitarra/instrumento pela interface de áudio.");
    if (ui_segmented((Rectangle){p.x + 14, p.y + 40, p.width - 28, 38}, "Teclado;MIDI;Guitarra", &mode, C_CYAN, C_CYAN_BG, C_CYAN_HI))
        param_set(P_INPUT_MODE, (float)mode);
    if (mode == 0) input_keys(g, p);
    else if (mode == 1) input_midi(g, p);
    else input_guitar(g, p);
}

/* ------------------------------------------------------------------ */
/* Abas                                                                */
/* ------------------------------------------------------------------ */
static void wave_row(float x, float y, const char *name, int param)
{
    static const char *icons[4] = {"sine", "saw", "square", "tri"};
    static const char *names[4] = {"Seno", "Serra", "Quadrada", "Triângulo"};
    ui_text(name, x, y + 9, 11, UI_SEMIBOLD, C_TEXT2);
    int sel = param_geti(param);
    for (int i = 0; i < 4; i++) {
        Rectangle b = {x + 46 + i * 50.0f, y, 46, 32};
        bool on = i == sel, hov = ui_hover(b);
        ui_rect(b, 8, on ? C_AMBER_BG : (hov ? C_CTRL_HOV : C_CTRL));
        ui_rect_lines(b, 8, 1, on ? C_AMBER_BD : C_BORDER2);
        ui_icon(icons[i], b.x + 13, b.y + 9, 20, on ? C_AMBER : (Color){140, 149, 164, 255});
        ui_tooltip(b, names[i]);
        if (ui_learn()) ui_learn_zone(b, param);
        else if (ui_clicked(b)) param_set(param, (float)i);
    }
}

static void tab_sound(Gui *g)
{
    Rectangle area = {324, 118, 944, 446};
    float cw = (area.width - 20) / 3, ch = (area.height - 10) / 2;
    Rectangle c[6];
    for (int i = 0; i < 6; i++) c[i] = (Rectangle){area.x + (i % 3) * (cw + 10), area.y + (i / 3) * (ch + 10), cw, ch};

    ui_card(c[0], "OSCILADORES", C_AMBER);
    wave_row(c[0].x + 14, c[0].y + 38, "OSC 1", P_OSC1_WAVE);
    wave_row(c[0].x + 14, c[0].y + 76, "OSC 2", P_OSC2_WAVE);
    ui_knob(c[0].x + cw * 0.3f, c[0].y + 148, 46, P_OSC_MIX, C_AMBER, "Mistura");
    ui_knob_bipolar(c[0].x + cw * 0.7f, c[0].y + 148, 46, P_DETUNE, C_AMBER, "Desafinar");
    ui_tooltip((Rectangle){c[0].x + cw * 0.3f - 30, c[0].y + 122, 60, 76}, "Quanto do OSC 2 entra no som (0% = só OSC 1).");
    ui_tooltip((Rectangle){c[0].x + cw * 0.7f - 30, c[0].y + 122, 60, 76}, "Desafina o OSC 2 em semitons: um pouco engrossa o som; +12 = uma oitava acima.");

    ui_card(c[1], "FILTRO", C_AMBER);
    ui_text_right("passa-baixa 12 dB", c[1].x + cw - 14, c[1].y + 12, 11, UI_REGULAR, C_TEXT2);
    draw_filter_curve((Rectangle){c[1].x + 14, c[1].y + 36, cw - 28, 74});
    ui_knob(c[1].x + cw * 0.3f, c[1].y + 152, 54, P_CUTOFF, C_AMBER, "Corte");
    ui_knob(c[1].x + cw * 0.7f, c[1].y + 152, 46, P_RESONANCE, C_AMBER, "Ressonância");
    ui_tooltip((Rectangle){c[1].x + cw * 0.3f - 34, c[1].y + 120, 68, 80}, "Frequência de corte: mais alto = som mais brilhante.");
    ui_tooltip((Rectangle){c[1].x + cw * 0.7f - 30, c[1].y + 124, 60, 76}, "Realça as frequências perto do corte (som mais \"ácido\").");

    ui_card(c[2], "LFO", C_AMBER);
    char t[32];
    param_format(P_LFO_RATE, t, sizeof t);
    ui_led(c[2].x + cw - 64, c[2].y + 19, 3.5f, sinf((float)GetTime() * param_get(P_LFO_RATE) * 2 * PI) > 0, C_AMBER);
    ui_text_right(t, c[2].x + cw - 14, c[2].y + 12, 11, UI_REGULAR, C_TEXT2);
    draw_lfo((Rectangle){c[2].x + 14, c[2].y + 36, cw - 28, 74});
    ui_knob(c[2].x + cw * 0.3f, c[2].y + 152, 46, P_LFO_RATE, C_AMBER, "Velocidade");
    ui_knob(c[2].x + cw * 0.7f, c[2].y + 152, 46, P_MODWHEEL, C_AMBER, "Roda mod.");
    ui_tooltip((Rectangle){c[2].x + cw * 0.7f - 30, c[2].y + 124, 60, 76}, "Quantidade de vibrato (igual à roda de modulação de um teclado MIDI).");

    const int amp[4] = {P_ATTACK, P_DECAY, P_SUSTAIN, P_RELEASE};
    const int flt[4] = {P_FATTACK, P_FDECAY, P_FSUSTAIN, P_FRELEASE};
    const char *lab[4] = {"Ataque", "Decai.", "Sust.", "Release"};
    for (int k = 0; k < 2; k++) {
        Rectangle r = c[3 + k];
        ui_card(r, k ? "ENVELOPE DO FILTRO" : "ENVELOPE DE VOLUME", C_AMBER);
        const int *ids = k ? flt : amp;
        draw_adsr((Rectangle){r.x + 14, r.y + 36, cw - 28, 60}, ids[0], ids[1], ids[2], ids[3]);
        for (int i = 0; i < 4; i++) ui_knob(r.x + 14 + (cw - 28) * (i + 0.5f) / 4, r.y + 136, 38, ids[i], C_AMBER, lab[i]);
    }
    ui_tooltip((Rectangle){c[3].x, c[3].y, cw, 30}, "Como o volume de cada nota nasce, se mantém e morre.");
    ui_tooltip((Rectangle){c[4].x, c[4].y, cw, 30}, "Como o filtro abre e fecha em cada nota (ligado ao corte na aba Modulação).");

    ui_card(c[5], "SAÍDA", C_AMBER);
    ui_knob(c[5].x + cw * 0.3f, c[5].y + 72, 46, P_GLIDE, C_AMBER, "Glide");
    ui_knob(c[5].x + cw * 0.7f, c[5].y + 72, 46, P_SYNTH_LEVEL, C_AMBER, "Nível");
    ui_tooltip((Rectangle){c[5].x + cw * 0.3f - 30, c[5].y + 44, 60, 76}, "Tempo que a nota da guitarra leva para deslizar até a próxima.");
    Rectangle vb = {c[5].x + 14, c[5].y + ch - 50, cw - 28, 36};
    ui_panel(vb, 10, C_WELL, C_BORDER);
    ui_text("Vozes", vb.x + 12, vb.y + 10, 11, UI_REGULAR, C_TEXT2);
    int nv = synth_active_voices(&g->e->synth);
    for (int i = 0; i < MAX_VOICES; i++) ui_led(vb.x + 80 + i * 16.0f, vb.y + 18, 5, i < nv, C_AMBER);
    snprintf(t, sizeof t, "%d / %d", nv, MAX_VOICES);
    ui_text_right(t, vb.x + vb.width - 12, vb.y + 10, 12, UI_SEMIBOLD, C_TEXT);
}

/* ---------- efeitos: pedaleira ---------- */
static const Color FX_COLOR[FX_COUNT] = {
    {251, 146, 60, 255}, {45, 212, 191, 255}, {167, 139, 250, 255}, {56, 189, 248, 255}, {129, 140, 248, 255}};
static const char *FX_DESC[FX_COUNT] = {"saturação valvulada", "duas vozes, estéreo", "6 estágios", "no tempo do BPM", "sala (Freeverb)"};
static const char *FX_TIP[FX_COUNT] = {
    "Distorção: satura o som como um amplificador valvulado. Drive = quanto, Tom = brilho.",
    "Chorus: duplica o som com pequenas variações, deixando-o largo e \"molhado\".",
    "Phaser: varre filtros pelo som, criando aquele efeito de \"jato\".",
    "Delay: repete o som. Com uma divisão escolhida, as repetições seguem o BPM.",
    "Reverb: simula o som de uma sala. Tamanho = sala maior, Abafar = paredes macias."};

static void pedal(Gui *g, Rectangle r, int slot, int t, bool ghost)
{
    bool on = param_geti(fx_param_on(t));
    Color col = FX_COLOR[t];
    if (!ghost) ui_shadow(r, 18);
    ui_panel(r, 18, on ? (Color){23, 27, 33, 255} : (Color){19, 22, 27, 255}, ghost ? col : (on ? (Color){44, 51, 62, 255} : (Color){32, 37, 45, 255}));
    Rectangle handle = {r.x + 12, r.y + 12, 30, 30};
    bool hh = ui_hover(handle);
    ui_rect(handle, 8, hh ? C_CTRL_HOV : C_CTRL);
    ui_icon("drag", handle.x + 8, handle.y + 8, 14, (Color){140, 149, 164, 255});
    if (!ghost && g->drag_fx < 0) ui_tooltip(handle, "Arraste para mudar a ordem na cadeia.");
    if (!ghost && ui_pressed(handle)) { g->drag_fx = slot; g->drag_off = ui_mouse().x - r.x; }
    char num[16];
    snprintf(num, sizeof num, "%02d", slot + 1);
    ui_text_center(num, (Rectangle){r.x, r.y + 18, r.width, 16}, 11, UI_BOLD, C_TEXT2);
    ui_led(r.x + r.width - 24, r.y + 27, 5.5f, on, col);

    ui_text(fx_name(t), r.x + 14, r.y + 54, 19, UI_BOLD, on ? C_TEXT : C_TEXT2);
    ui_text(FX_DESC[t], r.x + 14, r.y + 80, 11.5f, UI_REGULAR, C_TEXT2);
    if (!ghost && g->drag_fx < 0) ui_tooltip((Rectangle){r.x, r.y + 50, r.width, 46}, FX_TIP[t]);

    static const int K[FX_COUNT][4] = {
        {P_DIST_DRIVE, P_DIST_TONE, P_DIST_MIX, -1},
        {P_CHO_RATE, P_CHO_DEPTH, P_CHO_MIX, -1},
        {P_PHA_RATE, P_PHA_DEPTH, P_PHA_FB, P_PHA_MIX},
        {P_DLY_FB, P_DLY_MIX, P_DLY_TIME, -1},
        {P_REV_SIZE, P_REV_DAMP, P_REV_MIX, -1}};
    static const char *KL[FX_COUNT][4] = {
        {"Drive", "Tom", "Mix", NULL}, {"Veloc.", "Prof.", "Mix", NULL}, {"Veloc.", "Prof.", "Realim.", "Mix"},
        {"Repetições", "Mix", "Tempo", NULL}, {"Tamanho", "Abafar", "Mix", NULL}};
    Color kc = on ? col : (Color){91, 100, 114, 255};
    int nk = 0;
    for (int k = 0; k < 4; k++) {
        if (K[t][k] < 0) continue;
        if (t == FX_DELAY && k == 2 && param_geti(P_DLY_SYNC) != 0) continue; /* tempo livre só em "Livre" */
        float cx = r.x + r.width * ((nk % 2) ? 0.73f : 0.27f), cy = r.y + 136 + (nk / 2) * (t == FX_DELAY ? 84.0f : 92.0f);
        if (!ghost) ui_knob(cx, cy, 48, K[t][k], kc, KL[t][k]);
        else ui_knob_draw(cx, cy, 48, param_get_norm(K[t][k]), kc, false);
        nk++;
    }
    if (t == FX_DELAY && !ghost) {
        float y = r.y + 208;
        ui_caption("TEMPO", r.x + 14, y, C_TEXT2);
        int s = param_geti(P_DLY_SYNC);
        if (ui_pills((Rectangle){r.x + 14, y + 16, r.width - 28, 47}, 3, 21, "Livre;1/4;1/8;1/8 ·;1/16;1/4 T", &s, true))
            param_set(P_DLY_SYNC, (float)s);
        ui_text("Ping-pong", r.x + 14, y + 74, 12, UI_REGULAR, C_TEXT);
        ui_param_switch((Rectangle){r.x + r.width - 58, y + 68, 44, 24}, P_DLY_PING, C_CYAN);
        ui_tooltip((Rectangle){r.x + 14, y + 68, r.width - 28, 24}, "As repetições alternam entre a esquerda e a direita.");
    }

    /* pedal de pisar */
    Rectangle fs = {r.x + r.width / 2 - 26, r.y + r.height - 80, 52, 52};
    bool fh = ui_hover(fs);
    Vector2 fc = {fs.x + 26, fs.y + 26};
    DrawCircleV((Vector2){fc.x, fc.y + 3}, 27, (Color){10, 12, 15, 255});
    DrawCircleV(fc, 27, on ? col : (Color){42, 48, 58, 255});
    DrawCircleV(fc, 24, fh ? C_CTRL_HOV : C_CTRL);
    DrawCircleV(fc, 15, (Color){42, 48, 58, 255});
    DrawCircleV((Vector2){fc.x, fc.y - 2}, 14, (Color){52, 58, 70, 255});
    ui_text_center(on ? "LIGADO" : "DESLIGADO", (Rectangle){r.x, r.y + r.height - 22, r.width, 14}, 11, UI_BOLD, on ? col : (Color){140, 149, 164, 255});
    if (!ghost) {
        if (ui_learn()) ui_learn_zone(fs, fx_param_on(t));
        else if (ui_clicked(fs)) param_set(fx_param_on(t), !on);
        ui_tooltip(fs, on ? "Clique para desligar" : "Clique para ligar");
    }
}

static void tab_effects(Gui *g)
{
    Rectangle area = {324, 118, 944, 446};
    /* cadeia de sinal */
    Rectangle bar = {area.x, area.y, area.width, 44};
    ui_panel(bar, 12, C_PANEL, C_BORDER);
    float x = bar.x + 16;
    ui_caption("CADEIA DE SINAL", x, bar.y + 16, C_TEXT2);
    x += 130;
    const char *names[8];
    Color cols[8];
    int n = 0;
    names[n] = param_geti(P_INPUT_MODE) == 2 ? "Synth + Guitarra" : "Synth";
    cols[n++] = C_TEXT;
    for (int s = 0; s < FX_COUNT; s++) {
        int t = param_geti(P_FX_ORDER0 + s);
        if (param_geti(fx_param_on(t))) { names[n] = fx_name(t); cols[n++] = FX_COLOR[t]; }
    }
    names[n] = "Looper"; cols[n++] = C_TEXT;
    names[n] = "Saída"; cols[n++] = C_TEXT;
    for (int i = 0; i < n; i++) {
        float tw = ui_text_width(names[i], 12, UI_SEMIBOLD) + 18;
        Rectangle c = {x, bar.y + 9, tw, 26};
        ui_panel(c, 8, (Color){25, 29, 36, 255}, C_BORDER2);
        ui_text_center(names[i], c, 12, UI_SEMIBOLD, cols[i]);
        x += tw;
        if (i < n - 1) { ui_icon("arrow", x + 4, bar.y + 15, 14, (Color){107, 115, 128, 255}); x += 22; }
    }

    /* pedais */
    float pw = 172, gap = (area.width - 5 * pw) / 4, py = area.y + 54, ph = area.height - 54;
    int order[FX_COUNT];
    for (int s = 0; s < FX_COUNT; s++) order[s] = param_geti(P_FX_ORDER0 + s);

    int drop = -1;
    if (g->drag_fx >= 0) {
        float mx = ui_mouse().x - g->drag_off + pw / 2;
        drop = (int)((mx - area.x) / (pw + gap));
        drop = drop < 0 ? 0 : (drop > FX_COUNT - 1 ? FX_COUNT - 1 : drop);
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) { /* soltou: reordena */
            int moving = order[g->drag_fx];
            int tmp[FX_COUNT], k = 0;
            for (int s = 0; s < FX_COUNT; s++) if (s != g->drag_fx) tmp[k++] = order[s];
            for (int s = FX_COUNT - 1; s > drop; s--) tmp[s] = tmp[s - 1];
            tmp[drop] = moving;
            for (int s = 0; s < FX_COUNT; s++) param_set(P_FX_ORDER0 + s, (float)tmp[s]);
            g->drag_fx = -1;
            set_status(g, "Ordem dos efeitos alterada");
            return;
        }
    }
    /* posições: com arraste, os outros abrem espaço */
    int vis = 0;
    for (int s = 0; s < FX_COUNT; s++) {
        if (s == g->drag_fx) continue;
        if (vis == drop) vis++;
        Rectangle r = {area.x + vis * (pw + gap), py, pw, ph};
        if (vis > 0) DrawRectangleRec((Rectangle){r.x - gap, py + ph / 2 - 2, gap, 4}, (Color){42, 48, 58, 255});
        pedal(g, r, s, order[s], false);
        vis++;
    }
    if (g->drag_fx >= 0) {
        Rectangle slot = {area.x + drop * (pw + gap), py, pw, ph};
        ui_rect_lines(slot, 18, 2, Fade(FX_COLOR[order[g->drag_fx]], 0.6f));
        Rectangle r = {ui_mouse().x - g->drag_off, py - 6, pw, ph};
        pedal(g, r, g->drag_fx, order[g->drag_fx], true);
    }
}

/* ---------- modulação ---------- */
static void tab_modulation(Gui *g)
{
    (void)g;
    Rectangle area = {324, 118, 944, 446};
    ui_card(area, "MATRIZ DE MODULAÇÃO", C_AMBER);
    ui_text("Cada cabo liga uma FONTE a um DESTINO. \"Via\" multiplica pela segunda fonte: LFO via Roda mod. -> Altura = vibrato na roda.",
            area.x + 16, area.y + 38, 12, UI_REGULAR, C_TEXT2);
    float cols[4] = {area.x + 110, area.x + 330, area.x + 550, area.x + 790};
    const char *hdr[4] = {"FONTE", "VIA (multiplica)", "DESTINO", "QUANTIDADE"};
    for (int i = 0; i < 4; i++) ui_caption(hdr[i], cols[i], area.y + 66, C_TEXT2);
    static const char *SRC = "Nenhuma;LFO;Env. filtro;Velocity;Roda mod.;Nível guitarra";
    static const char *DST = "Altura;Corte;Volume;Mistura OSC";
    static int vals[MOD_SLOTS][3];
    for (int i = 0; i < MOD_SLOTS; i++) {
        float y = area.y + 88 + i * 58.0f;
        bool active = param_geti(P_MOD_SRC(i)) != 0 && fabsf(param_get(P_MOD_AMT(i))) > 0.001f;
        ui_panel((Rectangle){area.x + 14, y, area.width - 28, 50}, 10, active ? (Color){24, 28, 34, 255} : C_WELL, C_BORDER);
        ui_led(area.x + 32, y + 25, 4, active, C_AMBER);
        char lab[16];
        snprintf(lab, sizeof lab, "Cabo %d", i + 1);
        ui_text(lab, area.x + 46, y + 17, 12.5f, UI_SEMIBOLD, active ? C_TEXT : C_TEXT2);
        vals[i][0] = param_geti(P_MOD_SRC(i));
        vals[i][1] = param_geti(P_MOD_VIA(i));
        vals[i][2] = param_geti(P_MOD_DST(i));
        ui_tooltip((Rectangle){cols[0], y + 8, 200, 34}, param_help(P_MOD_SRC(i)));
        ui_tooltip((Rectangle){cols[1], y + 8, 200, 34}, param_help(P_MOD_VIA(i)));
        ui_tooltip((Rectangle){cols[2], y + 8, 220, 34}, param_help(P_MOD_DST(i)));
        if (ui_dropdown((Rectangle){cols[0], y + 8, 200, 34}, SRC, &vals[i][0], DD_MOD + i * 3)) param_set(P_MOD_SRC(i), (float)vals[i][0]);
        if (ui_dropdown((Rectangle){cols[1], y + 8, 200, 34}, SRC, &vals[i][1], DD_MOD + i * 3 + 1)) param_set(P_MOD_VIA(i), (float)vals[i][1]);
        if (ui_dropdown((Rectangle){cols[2], y + 8, 220, 34}, DST, &vals[i][2], DD_MOD + i * 3 + 2)) param_set(P_MOD_DST(i), (float)vals[i][2]);
        /* quantidade: barra bipolar arrastável */
        Rectangle b = {cols[3], y + 19, 90, 12};
        float v = param_get_norm(P_MOD_AMT(i));
        ui_rect(b, 6, (Color){35, 40, 49, 255});
        float mid = b.x + b.width / 2, px = b.x + b.width * v;
        DrawRectangleRec((Rectangle){fminf(mid, px), b.y + 2, fabsf(px - mid), b.height - 4}, active ? C_AMBER : (Color){91, 100, 114, 255});
        DrawRectangleRec((Rectangle){mid - 0.5f, b.y - 3, 1, b.height + 6}, C_TEXT2);
        DrawCircleV((Vector2){px, b.y + b.height / 2}, 7, C_TEXT);
        Rectangle hit = {b.x - 8, y + 6, b.width + 16, 38};
        if (ui_learn()) ui_learn_zone(hit, P_MOD_AMT(i));
        else {
            if (ui_hover(hit) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) param_set_norm(P_MOD_AMT(i), (ui_mouse().x - b.x) / b.width);
            if (ui_hover(hit)) param_set_norm(P_MOD_AMT(i), v + GetMouseWheelMove() * 0.01f);
            if (ui_hover(hit) && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) param_set(P_MOD_AMT(i), 0);
        }
        ui_tooltip(hit, "Arraste para ajustar. Meio = nada; à direita soma, à esquerda inverte. Botão direito zera.");
        char t[16];
        param_format(P_MOD_AMT(i), t, sizeof t);
        ui_text(t, b.x + b.width + 12, y + 17, 12.5f, UI_SEMIBOLD, C_TEXT);
    }
}

/* ---------- tempo (compartilhado por arp e looper) ---------- */
static void tempo_card(Gui *g, Rectangle r)
{
    ui_card(r, "TEMPO", C_AMBER);
    int beat = atomic_load(&g->e->beat);
    for (int i = 0; i < 4; i++) ui_led(r.x + r.width - 74 + i * 17.0f, r.y + 19, 5, i == beat, i == 0 ? C_AMBER : C_TEXT);
    bool wide = r.width >= 300;
    Rectangle st = {r.x + 14, r.y + 42, wide ? 150 : r.width - 28, 56};
    ui_panel(st, 12, C_WELL, C_BORDER);
    if (ui_button_icon((Rectangle){st.x + 4, st.y + 13, 30, 30}, "minus", "Diminuir BPM")) param_set(P_BPM, param_get(P_BPM) - 1);
    if (ui_button_icon((Rectangle){st.x + st.width - 34, st.y + 13, 30, 30}, "plus", "Aumentar BPM")) param_set(P_BPM, param_get(P_BPM) + 1);
    char t[16];
    snprintf(t, sizeof t, "%d", param_geti(P_BPM));
    ui_text_center(t, (Rectangle){st.x + 34, st.y + 4, st.width - 68, 34}, 28, UI_BOLD, C_TEXT);
    ui_text_center("BPM", (Rectangle){st.x + 34, st.y + 36, st.width - 68, 14}, 10, UI_SEMIBOLD, C_TEXT2);
    Rectangle bpmhit = {st.x + 34, st.y, st.width - 68, st.height};
    if (ui_hover(bpmhit)) param_set(P_BPM, param_get(P_BPM) + GetMouseWheelMove());
    ui_tooltip(bpmhit, "Andamento. Use a roda do mouse, os botões ou o Tap.");
    Rectangle tap = wide ? (Rectangle){st.x + st.width + 10, st.y, r.width - st.width - 38, 56}
                         : (Rectangle){r.x + 14, st.y + st.height + 8, r.width - 28, 38};
    ui_tooltip(tap, "Clique no ritmo da música (4 vezes ou mais) para acertar o BPM.");
    if (ui_button(tap, "@tap Tap", UI_BTN_AMBER)) {
        double now = GetTime();
        if (g->ntaps > 0 && now - g->taps[(g->ntaps - 1) % 4] > 2.0) g->ntaps = 0;
        g->taps[g->ntaps % 4] = now;
        g->ntaps++;
        if (g->ntaps >= 2) {
            int n = g->ntaps < 4 ? g->ntaps : 4;
            double first = g->taps[(g->ntaps - n) % 4], last = g->taps[(g->ntaps - 1) % 4];
            param_set(P_BPM, (float)(60.0 * (n - 1) / (last - first)));
        }
    }
    ui_tooltip(tap, "Clique no ritmo da música (3 ou 4 vezes) para acertar o BPM.");
    float y = tap.y + tap.height + 16;
    ui_text("Metrônomo", r.x + 14, y + 4, 12.5f, UI_REGULAR, C_TEXT);
    ui_param_switch((Rectangle){r.x + r.width - 58, y, 44, 24}, P_METRO_ON, C_AMBER);
    ui_knob(r.x + r.width / 2, y + 76, 44, P_METRO_VOL, C_AMBER, "Vol. metrônomo");
}

/* ---------- arpejador + harmonizador ---------- */
static void tab_arp(Gui *g)
{
    Rectangle area = {324, 118, 944, 446};
    float w1 = 352, w2 = 352, w3 = area.width - w1 - w2 - 20;
    Rectangle a = {area.x, area.y, w1, area.height}, h = {a.x + w1 + 10, area.y, w2, area.height};
    Rectangle tc = {h.x + w2 + 10, area.y, w3, area.height};

    /* --- arpejador --- */
    ui_card(a, "ARPEJADOR", C_AMBER);
    ui_param_switch((Rectangle){a.x + a.width - 58, a.y + 8, 44, 24}, P_ARP_ON, C_AMBER);
    ui_tooltip((Rectangle){a.x + a.width - 58, a.y + 8, 44, 24}, "Liga o arpejador: segure um acorde e as notas tocam uma de cada vez.");
    Rectangle steps = {a.x + 14, a.y + 40, a.width - 28, 92};
    ui_panel(steps, 12, C_WELL, C_BORDER);
    {
        /* notas que o arpejador vai tocar (leitura aproximada do estado do áudio) */
        int notes[ARP_MAX_NOTES], n = g->e->arp.count, playing = g->e->arp.playing;
        if (n > ARP_MAX_NOTES) n = ARP_MAX_NOTES;
        for (int i = 0; i < n; i++) notes[i] = g->e->arp.order[i];
        for (int i = 1; i < n; i++) for (int j = i; j > 0 && notes[j - 1] > notes[j]; j--) { int t = notes[j]; notes[j] = notes[j - 1]; notes[j - 1] = t; }
        int oct = param_geti(P_ARP_OCT), total = n * oct;
        if (total > 16) total = 16;
        if (total == 0) ui_text_center("segure um acorde para ver o arpejo", steps, 12, UI_REGULAR, C_TEXT2);
        float bw = total ? (steps.width - 24 - (total - 1) * 5) / total : 0;
        for (int i = 0; i < total; i++) {
            int note = notes[i % n] + 12 * (i / n);
            float hgt = 14 + (note % 36) * 1.3f;
            bool on = note == playing && param_geti(P_ARP_ON);
            Rectangle b = {steps.x + 12 + i * (bw + 5), steps.y + steps.height - 26 - hgt, bw, hgt};
            ui_rect(b, 4, on ? C_AMBER : (Color){58, 46, 22, 255});
            char nm[8];
            note_name(note, nm, sizeof nm);
            if (bw > 18) ui_text_center(nm, (Rectangle){b.x - 4, steps.y + steps.height - 20, bw + 8, 12}, 9.5f, UI_SEMIBOLD, on ? C_AMBER_HI : C_TEXT2);
        }
    }
    ui_text("Modo", a.x + 14, a.y + 144, 11, UI_REGULAR, C_TEXT2);
    int m = param_geti(P_ARP_MODE);
    ui_tooltip((Rectangle){a.x + 14, a.y + 162, a.width - 28, 70}, param_help(P_ARP_MODE));
    if (ui_pills((Rectangle){a.x + 14, a.y + 162, a.width - 28, 70}, 3, 32, "Sobe;Desce;Sobe-desce;Aleatório;Como tocado", &m, false)) param_set(P_ARP_MODE, (float)m);
    ui_text("Divisão", a.x + 14, a.y + 244, 11, UI_REGULAR, C_TEXT2);
    int d = param_geti(P_ARP_RATE);
    if (ui_pills((Rectangle){a.x + 14, a.y + 262, a.width - 28, 32}, 5, 32, "1/4;1/8;1/8 T;1/16;1/32", &d, false)) param_set(P_ARP_RATE, (float)d);
    ui_knob(a.x + a.width * 0.2f, a.y + 352, 46, P_ARP_OCT, C_AMBER, "Oitavas");
    ui_knob(a.x + a.width * 0.5f, a.y + 352, 46, P_ARP_GATE, C_AMBER, "Duração");
    ui_tooltip((Rectangle){a.x + a.width * 0.5f - 30, a.y + 324, 60, 76}, "Quanto cada nota dura dentro do passo.");
    ui_param_switch((Rectangle){a.x + a.width * 0.8f - 22, a.y + 340, 44, 24}, P_ARP_HOLD, C_AMBER);
    ui_text_center("SEGURAR", (Rectangle){a.x + a.width * 0.8f - 40, a.y + 380, 80, 13}, 10.5f, UI_SEMIBOLD, C_TEXT2);
    ui_tooltip((Rectangle){a.x + a.width * 0.8f - 40, a.y + 330, 80, 70}, "Continua tocando depois de soltar as teclas; um acorde novo troca o anterior.");

    /* --- harmonizador --- */
    ui_card(h, "HARMONIZADOR", C_CYAN);
    ui_param_switch((Rectangle){h.x + h.width - 58, h.y + 8, 44, 24}, P_HARM_ON, C_CYAN);
    ui_tooltip((Rectangle){h.x + h.width - 58, h.y + 8, 44, 24}, "Acrescenta uma segunda voz (ou um acorde) seguindo o tom e a escala.");
    Rectangle ex = {h.x + 14, h.y + 40, h.width - 28, 92};
    ui_panel(ex, 12, C_WELL, C_BORDER);
    {
        int src = atomic_load(&g->e->guitar.st_note);
        if (src < 0) src = 60 + param_geti(P_HARM_KEY);
        int hs[2], n = engine_harmony_notes(src, hs);
        char a1[8], a2[8], a3[8];
        note_name(src, a1, sizeof a1);
        note_name(n > 0 ? hs[0] : -1, a2, sizeof a2);
        note_name(n > 1 ? hs[1] : -1, a3, sizeof a3);
        char right[24];
        snprintf(right, sizeof right, n > 1 ? "%s+%s" : "%s", a2, a3);
        bool on = param_geti(P_HARM_ON);
        float cx = ex.x + ex.width / 2;
        ui_text_center("VOCÊ TOCA", (Rectangle){ex.x, ex.y + 14, ex.width / 2 - 20, 12}, 10.5f, UI_SEMIBOLD, C_TEXT2);
        ui_text_center(a1, (Rectangle){ex.x, ex.y + 34, ex.width / 2 - 20, 40}, 32, UI_BOLD, C_TEXT);
        ui_icon("arrow", cx - 10, ex.y + 48, 20, on ? C_CYAN : C_TEXT2);
        ui_text_center("HARMONIA", (Rectangle){cx + 20, ex.y + 14, ex.width / 2 - 20, 12}, 10.5f, UI_SEMIBOLD, C_TEXT2);
        ui_text_center(right, (Rectangle){cx + 20, ex.y + 34, ex.width / 2 - 20, 40}, n > 1 ? 24 : 32, UI_BOLD, on ? C_CYAN_HI : C_TEXT2);
    }
    ui_text("Intervalo", h.x + 14, h.y + 144, 11, UI_REGULAR, C_TEXT2);
    int iv = param_geti(P_HARM_INTERVAL);
    ui_tooltip((Rectangle){h.x + 14, h.y + 162, h.width - 28, 66}, param_help(P_HARM_INTERVAL));
    if (ui_pills((Rectangle){h.x + 14, h.y + 162, h.width - 28, 66}, 4, 30, "Terça ↑;Quarta ↑;Quinta ↑;Sexta ↑;Oitava ↑;Terça ↓;Oitava ↓;Acorde", &iv, true))
        param_set(P_HARM_INTERVAL, (float)iv);
    ui_text("Tom", h.x + 14, h.y + 240, 11, UI_REGULAR, C_TEXT2);
    int key = param_geti(P_HARM_KEY);
    if (ui_pills((Rectangle){h.x + 14, h.y + 258, h.width - 28, 64}, 6, 29, "C;C#;D;D#;E;F;F#;G;G#;A;A#;B", &key, true)) param_set(P_HARM_KEY, (float)key);
    int sc = param_geti(P_HARM_SCALE);
    ui_text("Escala", h.x + 14, h.y + 334, 11, UI_REGULAR, C_TEXT2);
    if (ui_segmented((Rectangle){h.x + 14, h.y + 352, 196, 34}, "Maior;Menor;Fixo", &sc, C_CYAN, C_CYAN_BG, C_CYAN_HI)) param_set(P_HARM_SCALE, (float)sc);
    ui_tooltip((Rectangle){h.x + 14, h.y + 352, 196, 34}, "Maior/Menor: segue a escala do tom. Fixo: sempre o mesmo intervalo em semitons.");
    int ap = param_geti(P_HARM_SRC);
    ui_text("Aplicar em", h.x + 14, h.y + 394, 11, UI_REGULAR, C_TEXT2);
    ui_tooltip((Rectangle){h.x + 14, h.y + 410, 196, 30}, param_help(P_HARM_SRC));
    if (ui_segmented((Rectangle){h.x + 14, h.y + 410, 196, 30}, "Só guitarra;Tudo", &ap, C_CYAN, C_CYAN_BG, C_CYAN_HI)) param_set(P_HARM_SRC, (float)ap);
    ui_knob(h.x + h.width - 66, h.y + 378, 46, P_HARM_LEVEL, C_CYAN, "Volume");

    tempo_card(g, tc);
}

/* ---------- looper ---------- */
static void tab_looper(Gui *g)
{
    Rectangle area = {324, 118, 944, 446};
    Looper *lp = &g->e->looper;
    int st = atomic_load(&lp->st_state), pos = atomic_load(&lp->st_pos), len = atomic_load(&lp->st_len);
    float sr = g->e->sr;
    Rectangle c = {area.x, area.y, 620, area.height};
    ui_card(c, "LOOPER", C_RED);
    static const char *state_label[] = {"vazio", "gravando", "tocando", "gravando por cima", "parado"};
    Color sc = (st == LOOP_RECORDING || st == LOOP_OVERDUB) ? C_RED : (st == LOOP_PLAYING ? C_GREEN : C_TEXT2);
    float sw = ui_text_width(state_label[st], 11, UI_SEMIBOLD) + 16;
    Rectangle chip = {c.x + c.width - sw - 14, c.y + 9, sw, 20};
    ui_rect(chip, 6, Fade(sc, 0.15f));
    ui_text_center(state_label[st], chip, 11, UI_SEMIBOLD, sc);

    /* anel de progresso */
    Vector2 ctr = {c.x + 140, c.y + 170};
    DrawRing(ctr, 92, 104, 0, 360, 96, C_WELL);
    float prog = st == LOOP_RECORDING ? len / (60.0f * sr) : (len > 0 ? (float)pos / len : 0);
    if (st != LOOP_EMPTY && prog > 0.001f) DrawRing(ctr, 92, 104, -90, -90 + 360 * prog, 96, sc);
    DrawCircleV(ctr, 88, C_PANEL);
    char t[48];
    if (st == LOOP_EMPTY) snprintf(t, sizeof t, "--");
    else snprintf(t, sizeof t, "%.1f s", (st == LOOP_RECORDING ? len : pos) / sr);
    ui_text_center(t, (Rectangle){ctr.x - 80, ctr.y - 26, 160, 36}, 30, UI_BOLD, C_TEXT);
    if (st != LOOP_EMPTY && st != LOOP_RECORDING) snprintf(t, sizeof t, "de %.1f s", len / sr);
    else snprintf(t, sizeof t, st == LOOP_RECORDING ? "máx. 60 s" : "nada gravado");
    ui_text_center(t, (Rectangle){ctr.x - 80, ctr.y + 12, 160, 16}, 12, UI_REGULAR, C_TEXT2);

    /* botões */
    static const char *main_label[] = {"@rec Gravar", "@rec Fechar o loop", "@rec Gravar por cima", "@stop Parar de gravar", "@rec Gravar por cima"};
    float bx = c.x + 290, bw = c.width - 304;
    Rectangle mb = {bx, c.y + 70, bw, 64};
    if (ui_button(mb, main_label[st], UI_BTN_DANGER)) looper_command(lp, LCMD_REC);
    ui_tooltip(mb, "Botão principal (também na tecla Enter): grava, fecha o loop e grava camadas por cima.");
    float hw = (bw - 8) / 2;
    if (ui_button((Rectangle){bx, c.y + 144, hw, 40}, "@play Tocar", UI_BTN_NORMAL)) looper_command(lp, LCMD_PLAY);
    if (ui_button((Rectangle){bx + hw + 8, c.y + 144, hw, 40}, "@stop Parar", UI_BTN_NORMAL)) looper_command(lp, LCMD_STOP);
    if (ui_button((Rectangle){bx, c.y + 192, hw, 40}, "@undo Desfazer", UI_BTN_NORMAL)) looper_command(lp, LCMD_UNDO);
    if (ui_button((Rectangle){bx + hw + 8, c.y + 192, hw, 40}, "@trash Limpar", UI_BTN_NORMAL)) looper_command(lp, LCMD_CLEAR);
    ui_tooltip((Rectangle){bx, c.y + 192, hw, 40}, "Apaga só a última camada gravada por cima.");
    ui_knob(bx + bw / 2, c.y + 286, 46, P_LOOP_VOL, C_RED, "Volume do loop");

    ui_panel((Rectangle){c.x + 14, c.y + c.height - 82, c.width - 28, 68}, 10, C_WELL, C_BORDER);
    ui_text("1. Gravar e toque uma base.   2. Fechar o loop: ela passa a repetir.", c.x + 28, c.y + c.height - 70, 12, UI_REGULAR, C_TEXT2);
    ui_text("3. Gravar por cima para somar camadas.   Desfazer apaga a última.", c.x + 28, c.y + c.height - 52, 12, UI_REGULAR, C_TEXT2);
    ui_text("Grava o som com efeitos; o metrônomo fica de fora. Enter = botão vermelho.", c.x + 28, c.y + c.height - 34, 12, UI_REGULAR, C_TEXT2);

    tempo_card(g, (Rectangle){c.x + c.width + 10, area.y, area.width - c.width - 10, area.height});
}

/* ---------- saída MIDI ---------- */
static void tab_midi_out(Gui *g)
{
    Rectangle area = {324, 118, 944, 446};
    Rectangle c = {area.x, area.y, 470, area.height};
    ui_card(c, "SAÍDA MIDI", C_CYAN);
    float x = c.x + 14, w = c.width - 28;
    ui_text("Enviar", x, c.y + 42, 11, UI_REGULAR, C_TEXT2);
    int src = param_geti(P_MOUT_SRC);
    if (ui_segmented((Rectangle){x, c.y + 60, w, 38}, "Nada;Guitarra;Tudo", &src, C_CYAN, C_CYAN_BG, C_CYAN_HI)) param_set(P_MOUT_SRC, (float)src);
    ui_tooltip((Rectangle){x, c.y + 60, w, 38}, "Guitarra: só a nota tocada, com bends. Tudo: também teclado, MIDI, harmonia e arpejo.");
    ui_text("Destino", x, c.y + 112, 11, UI_REGULAR, C_TEXT2);
    if (ui_dropdown((Rectangle){x, c.y + 130, w - 44, 34}, g->mout_items, &g->mout_active, DD_MOUT)) apply_midi_out(g);
    ui_tooltip((Rectangle){x + w - 38, c.y + 130, 38, 34}, "Procura de novo os dispositivos (depois de ligar um cabo ou uma interface).");
    if (ui_button((Rectangle){x + w - 38, c.y + 130, 38, 34}, "@refresh", UI_BTN_NORMAL)) refresh_all_devices(g);
    bool open = midi_out_is_open();
    char st[200];
    if (open) snprintf(st, sizeof st, "Enviando para: %.40s", midi_out_open_name());
    else snprintf(st, sizeof st, "%s", src ? "Saída fechada (veja o destino)" : "Desligada");
    ui_led(x + 5, c.y + 186, 4, open, C_GREEN);
    ui_text(st, x + 16, c.y + 178, 12.5f, UI_SEMIBOLD, open ? C_GREEN : C_TEXT2);

    ui_knob(x + w * 0.2f, c.y + 248, 46, P_MOUT_CH, C_CYAN, "Canal");
    ui_knob(x + w * 0.5f, c.y + 248, 46, P_MOUT_BEND, C_CYAN, "Faixa do bend");
    ui_tooltip((Rectangle){x + w * 0.5f - 34, c.y + 220, 68, 80}, "Configure o mesmo valor (em semitons) no instrumento da DAW.");
    Rectangle tb = {x + w * 0.8f - 60, c.y + 230, 120, 36};
    if (ui_button(tb, "@play Testar", UI_BTN_CYAN) && open) midi_out_test();
    ui_tooltip(tb, "Envia um Dó para conferir se a DAW está recebendo.");
    int act = midi_out_activity();
    if (act != g->mout_seen) { g->mout_seen = act; g->mout_blink = GetTime(); }
    ui_led(x + w * 0.8f - 16, c.y + 290, 5, GetTime() - g->mout_blink < 0.08, C_GREEN);
    ui_text("atividade", x + w * 0.8f - 4, c.y + 283, 11, UI_REGULAR, C_TEXT2);

    ui_panel((Rectangle){x, c.y + c.height - 98, w, 84}, 10, C_WELL, C_BORDER);
    ui_text("A nota sai uns 20-30 ms depois da palhetada", x + 12, c.y + c.height - 86, 12, UI_REGULAR, C_TEXT2);
    ui_text("(tempo de reconhecer). Para ouvir só a DAW, deixe", x + 12, c.y + c.height - 68, 12, UI_REGULAR, C_TEXT2);
    ui_text("a Mistura em \"só gtr\" ou o Nível do synth em 0.", x + 12, c.y + c.height - 50, 12, UI_REGULAR, C_TEXT2);

    Rectangle h = {c.x + c.width + 10, area.y, area.width - c.width - 10, area.height};
    ui_card(h, "COMO USAR NA DAW", C_CYAN);
#if defined(_WIN32)
    static const char *how[] = {
        "No Windows não há porta virtual pronta:", "", "1. Instale o loopMIDI (gratuito).", "2. Crie uma porta (botão +).",
        "3. Clique em Atualizar e escolha a porta", "    como Destino.", "4. Na DAW (Reaper, FL Studio, Ableton...)",
        "    ative essa porta como entrada MIDI.", "5. Crie uma trilha com um instrumento", "    virtual e arme a gravação."};
#elif defined(__APPLE__)
    static const char *how[] = {
        "1. Destino: Porta virtual \"Sintetizador\".", "2. Na DAW (Logic, GarageBand, Reaper...)", "    a entrada \"Sintetizador\" aparece",
        "    sozinha nas entradas MIDI.", "3. Crie uma trilha com um instrumento", "    virtual e arme a gravação.",
        "4. Toque: a guitarra controla a trilha.", "", "", ""};
#else
    static const char *how[] = {
        "1. Destino: Porta virtual \"Sintetizador\".", "2. Na DAW (Reaper, Ardour, LMMS, Bitwig)", "    ative a entrada MIDI \"Sintetizador\"",
        "    (ALSA; no JACK use a2jmidid).", "3. Crie uma trilha com um instrumento", "    virtual e arme a gravação.",
        "4. Toque: a guitarra controla a trilha.", "", "", ""};
#endif
    for (int i = 0; i < 10; i++) ui_text(how[i], h.x + 16, h.y + 44 + i * 22.0f, 12.5f, UI_REGULAR, i == 0 ? C_TEXT : C_TEXT2);
}

/* ------------------------------------------------------------------ */
/* Janelas por cima                                                    */
/* ------------------------------------------------------------------ */
static Rectangle save_rect(void) { return (Rectangle){430, 54, 380, 214}; }
static Rectangle browser_rect(void) { return (Rectangle){212, 54, 760, 512}; }
static Rectangle audio_rect(void) { return (Rectangle){UI_W / 2 - 280, 96, 560, 520}; }

static int cmp_float(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }

static void latency_update(Gui *g)
{
    if (!g->lat_running) return;
    Engine *e = g->e;
    int st = atomic_load(&e->lat_state);
    if (st == LAT_FAIL) {
        g->lat_running = false;
        g->lat_ms = -1;
        atomic_store(&e->lat_state, LAT_IDLE);
        set_status(g, "Latência: o clique não voltou pela entrada. Confira o cabo/microfone e o volume.");
        return;
    }
    if (st == LAT_DONE) {
        g->lat_vals[g->lat_runs++] = atomic_load(&e->lat_result) * 1000.0f / e->sr;
        atomic_store(&e->lat_state, LAT_IDLE);
        g->lat_next = GetTime() + 0.25;
        if (g->lat_runs == 5) {
            qsort(g->lat_vals, 5, sizeof(float), cmp_float);
            g->lat_ms = g->lat_vals[2]; /* mediana: ignora uma medição estranha */
            g->lat_running = false;
            char m[160];
            snprintf(m, sizeof m, "Latência de ida e volta: %.1f ms (mediana de 5 medições)", g->lat_ms);
            set_status(g, m);
        }
        return;
    }
    if (st == LAT_IDLE && GetTime() >= g->lat_next) engine_latency_start(e);
}

static void draw_save_popup(Gui *g)
{
    Rectangle r = save_rect();
    ui_shadow(r, 12);
    ui_panel(r, 12, (Color){27, 31, 39, 255}, C_BORDER2);
    ui_caption("SALVAR PRESET", r.x + 16, r.y + 14, C_TEXT2);
    bool enter = ui_textbox((Rectangle){r.x + 16, r.y + 38, r.width - 32, 34}, g->save_name, 48, TB_SAVE, g->preset_name);
    ui_text("Categoria", r.x + 16, r.y + 84, 12, UI_SEMIBOLD, C_TEXT2);
    int pc = g->save_cat - 1;
    if (ui_pills((Rectangle){r.x + 16, r.y + 104, r.width - 32, 60}, 4, 28, "Lead;Pad;Baixo;Teclas;Pluck;Arpejo;Guitarra;Meus", &pc, false))
        g->save_cat = pc + 1;
    if (!ui_typing() && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))) enter = true;
    if (!ui_typing() && !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) ui_focus(TB_SAVE); /* o campo volta a receber as teclas */
    if (ui_button((Rectangle){r.x + r.width - 112, r.y + 170, 96, 32}, "Salvar", UI_BTN_AMBER) || enter) {
        save_preset(g, g->save_name[0] ? g->save_name : g->preset_name);
        g->save_open = false;
        ui_focus(0);
    }
    if (ui_button((Rectangle){r.x + r.width - 216, r.y + 170, 96, 32}, "Cancelar", UI_BTN_GHOST) || ui_pressed_outside(r) ||
        (IsKeyPressed(KEY_ESCAPE) && !ui_typing())) {
        g->save_open = false;
        ui_focus(0);
    }
}

/* ------------------------------------------------------------------ */
/* Navegador de presets                                                */
/* ------------------------------------------------------------------ */
static void lower_ascii(char *d, const char *s, int cap)
{
    int i = 0;
    for (; s[i] && i < cap - 1; i++) d[i] = (s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i];
    d[i] = 0;
}

static bool browser_match(Gui *g, int idx, int cat)
{
    if (cat >= 0 && preset_cat_of(g, idx) != cat) return false;
    if (!g->br_search[0]) return true;
    char q[48], a[160], b[200];
    lower_ascii(q, g->br_search, sizeof q);
    lower_ascii(a, preset_name_of(g, idx), sizeof a);
    lower_ascii(b, preset_desc_of(g, idx), sizeof b);
    return strstr(a, q) || strstr(b, q);
}

static int browser_list(Gui *g, int *out)
{
    int n = 0;
    for (int i = 0; i < preset_total(g); i++)
        if (browser_match(g, i, g->br_cat)) out[n++] = i;
    return n;
}

#define BR_ROW 54
#define BR_ROWS 7

static void browser_scroll_to(Gui *g, int pos)
{
    if (pos < g->br_first) g->br_first = pos;
    if (pos >= g->br_first + BR_ROWS) g->br_first = pos - BR_ROWS + 1;
}

static void open_browser(Gui *g)
{
    g->br_cat = -1;
    g->br_search[0] = 0;
    g->br_first = 0;
    g->br_last_click = -1;
    browser_scroll_to(g, g->preset_active);
    g->save_open = false;
}

static void draw_preset_browser(Gui *g)
{
    Rectangle r = browser_rect();
    ui_shadow(r, 14);
    ui_panel(r, 14, (Color){24, 28, 35, 255}, C_BORDER2);
    ui_caption("PRESETS", r.x + 20, r.y + 20, C_TEXT2);
    if (ui_button_icon((Rectangle){r.x + r.width - 44, r.y + 10, 32, 32}, "x", "Fechar")) g->browser_open = false;
    Rectangle sb = {r.x + r.width - 300, r.y + 10, 248, 32};
    if (ui_textbox(sb, g->br_search, 40, TB_SEARCH, "Buscar por nome ou som...")) g->br_first = 0;
    ui_tooltip(sb, "Procura no nome e na descrição. Ex.: \"delay\", \"baixo\", \"vibrato\".");

    int all[256];
    int n = browser_list(g, all);

    /* categorias à esquerda */
    float cx = r.x + 12, cy = r.y + 54, cwid = 168;
    DrawRectangle((int)(r.x + 192), (int)r.y + 52, 1, (int)r.height - 96, C_BORDER);
    for (int c = -1; c < PRESET_CAT_COUNT; c++) {
        int cnt = 0;
        for (int i = 0; i < preset_total(g); i++)
            if (browser_match(g, i, c)) cnt++;
        Rectangle row = {cx, cy, cwid, 34};
        bool sel = g->br_cat == c, hov = ui_hover(row);
        if (sel || hov) ui_rect(row, 8, sel ? C_AMBER_BG : C_CTRL);
        ui_text(c < 0 ? "Todas" : preset_cat_name(c), row.x + 12, row.y + 9, 13, sel ? UI_SEMIBOLD : UI_REGULAR,
                sel ? C_AMBER : (cnt ? C_TEXT : C_TEXT2));
        char t[16];
        snprintf(t, sizeof t, "%d", cnt);
        float tw = ui_text_width(t, 11.5f, UI_SEMIBOLD);
        ui_text(t, row.x + row.width - 12 - tw, row.y + 10, 11.5f, UI_SEMIBOLD, sel ? C_AMBER : C_TEXT2);
        if (ui_clicked(row)) { g->br_cat = c; g->br_first = 0; }
        if (c == PRESET_CAT_USER) ui_tooltip(row, "Os presets que você salvou com o botão Salvar.");
        cy += 36;
    }

    /* lista à direita */
    Rectangle lr = {r.x + 206, r.y + 54, r.width - 220, BR_ROWS * BR_ROW};
    if (ui_hover(lr)) g->br_first -= (int)GetMouseWheelMove();
    if (g->br_first > n - BR_ROWS) g->br_first = n - BR_ROWS;
    if (g->br_first < 0) g->br_first = 0;

    /* teclado: ↑ ↓ trocam de preset dentro da lista, Enter/Esc fecham */
    int pos = -1;
    for (int i = 0; i < n; i++)
        if (all[i] == g->preset_active) pos = i;
    int step = (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) - (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP));
    if (step && n > 0) {
        int np = pos < 0 ? 0 : (pos + step + n) % n;
        load_preset(g, all[np]);
        browser_scroll_to(g, np);
    }

    if (n == 0) {
        ui_text("Nenhum preset encontrado.", lr.x + 12, lr.y + 14, 13, UI_SEMIBOLD, C_TEXT2);
        if (g->br_cat == PRESET_CAT_USER && !g->br_search[0])
            ui_text("Ajuste um som e clique em Salvar, na barra de cima, para ele aparecer aqui.", lr.x + 12, lr.y + 36, 12,
                    UI_REGULAR, C_TEXT2);
    }
    for (int k = 0; k < BR_ROWS && g->br_first + k < n; k++) {
        int i = all[g->br_first + k];
        Rectangle row = {lr.x, lr.y + k * BR_ROW, lr.width - 14, BR_ROW - 4};
        bool act = i == g->preset_active, hov = ui_hover(row);
        ui_rect(row, 9, act ? (Color){42, 36, 24, 255} : (hov ? C_CTRL_HOV : C_CTRL));
        if (act) ui_rect_lines(row, 9, 1, C_AMBER);
        const char *tg = preset_cat_tag(preset_cat_of(g, i));
        float tw = ui_text_width(tg, 9.5f, UI_BOLD) + 10;
        Rectangle tag = {row.x + 12, row.y + 9, tw, 16};
        ui_rect(tag, 4, act ? C_AMBER_BG : (Color){40, 46, 56, 255});
        ui_text_center(tg, tag, 9.5f, UI_BOLD, act ? C_AMBER : C_TEXT2);
        ui_text(preset_name_of(g, i), tag.x + tw + 8, row.y + 7, 13.5f, UI_SEMIBOLD, act ? C_AMBER_HI : C_TEXT);
        ui_text(preset_desc_of(g, i), row.x + 12, row.y + 29, 11.5f, UI_REGULAR, C_TEXT2);
        if (act) ui_icon("play", row.x + row.width - 26, row.y + 17, 14, C_AMBER);
        if (ui_clicked(row)) {
            double now = GetTime();
            if (g->br_last_click == i && now - g->br_click_time < 0.4) g->browser_open = false;
            else load_preset(g, i);
            g->br_last_click = i;
            g->br_click_time = now;
        }
    }
    /* barra de rolagem */
    if (n > BR_ROWS) {
        float th = lr.height * BR_ROWS / n, ty = lr.y + (lr.height - th) * g->br_first / (float)(n - BR_ROWS);
        ui_rect((Rectangle){lr.x + lr.width - 6, lr.y, 4, lr.height}, 2, (Color){32, 37, 45, 255});
        ui_rect((Rectangle){lr.x + lr.width - 6, ty, 4, th}, 2, C_TEXT2);
    }

    DrawRectangle((int)r.x + 1, (int)(r.y + r.height - 44), (int)r.width - 2, 1, C_BORDER);
    ui_text("Clique para ouvir  ·  duplo clique ou Enter fecha  ·  setas para cima e para baixo trocam de preset", r.x + 20,
            r.y + r.height - 30, 12, UI_REGULAR, C_TEXT2);

    if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) && !ui_typing()) g->browser_open = false;
    if (IsKeyPressed(KEY_ESCAPE) && !ui_typing()) g->browser_open = false;
    if (ui_pressed_outside(r)) {
        /* clicar no próprio seletor fecha pelo botão; fora dele, fecha aqui */
        Rectangle sel = {248, 11, 248, 34};
        if (!CheckCollisionPointRec(ui_mouse(), sel)) g->browser_open = false;
    }
    if (!g->browser_open) ui_focus(0);
}

static void draw_audio_dialog(Gui *g)
{
    Rectangle r = audio_rect();
    DrawRectangle(0, 0, UI_W, UI_H, (Color){0, 0, 0, 120});
    ui_shadow(r, 16);
    ui_panel(r, 16, C_PANEL, C_BORDER2);
    ui_caption("CONFIGURAÇÕES DE ÁUDIO", r.x + 20, r.y + 18, C_TEXT);
    if (ui_button_icon((Rectangle){r.x + r.width - 40, r.y + 10, 30, 30}, "x", "Fechar")) g->audio_open = false;
    float x = r.x + 20, w = r.width - 40;
    ui_text("Saída de áudio", x, r.y + 52, 11, UI_REGULAR, C_TEXT2);
    if (ui_dropdown((Rectangle){x, r.y + 70, w - 44, 34}, g->out_items, &g->out_active, DD_OUT)) apply_audio(g);
    ui_tooltip((Rectangle){x + w - 38, r.y + 70, 38, 34}, "Procura de novo as saídas de áudio e reabre o som.");
    if (ui_button((Rectangle){x + w - 38, r.y + 70, 38, 34}, "@refresh", UI_BTN_NORMAL)) { build_device_lists(g); apply_audio(g); }
    char t[160];
    if (g->e->dev_ok && g->e->bridge_ms > 0.0f)
        snprintf(t, sizeof t, "Funcionando: %u Hz · buffer de %.1f ms · entrada e saída em aparelhos diferentes (+%.0f ms de ponte)",
                 g->e->dev_rate, g->e->dev_latency_ms, g->e->bridge_ms);
    else if (g->e->dev_ok) snprintf(t, sizeof t, "Funcionando: %u Hz · buffer de %.1f ms%s", g->e->dev_rate, g->e->dev_latency_ms,
                               g->e->asio_open ? " · ASIO" : (g->e->exclusive && !g->e->exclusive_failed ? " · exclusivo" : ""));
    else snprintf(t, sizeof t, "Áudio desligado");
    ui_led(x + 5, r.y + 124, 4, g->e->dev_ok, C_GREEN);
    ui_text(t, x + 16, r.y + 116, 12.5f, UI_SEMIBOLD, g->e->dev_ok ? C_TEXT : C_RED);
    if (g->audio_error[0]) ui_text(g->audio_error, x + 16, r.y + 136, 12, UI_REGULAR, C_RED);

    /* tamanho do buffer */
    ui_text("Tamanho do buffer", x, r.y + 162, 11, UI_REGULAR, C_TEXT2);
    static const int sizes[5] = {32, 64, 128, 256, 512};
    int cur = g->e->period_frames > 0 ? g->e->period_frames : FRAMES_PER_BUFFER, sel = 2;
    for (int i = 0; i < 5; i++) if (sizes[i] == cur) sel = i;
    ui_tooltip((Rectangle){x, r.y + 180, w, 38}, "Quantas amostras o som processa de cada vez. Menor = menos atraso, mas exige mais do computador.");
    if (ui_segmented((Rectangle){x, r.y + 180, w, 38}, "32 · 0,7 ms;64 · 1,3 ms;128 · 2,7 ms;256 · 5,3 ms;512 · 10,7 ms", &sel, C_CYAN, C_CYAN_BG, C_CYAN_HI)) {
        g->e->period_frames = sizes[sel];
        apply_audio(g);
        char m[120];
        if (sizes[sel] == 32) snprintf(m, sizeof m, "Buffer de 32 amostras: o mínimo. Funciona melhor com ASIO; se ouvir estalos, aumente.");
        else snprintf(m, sizeof m, "Buffer de %d amostras. Se ouvir estalos, aumente.", sizes[sel]);
        set_status(g, m);
    }
    ui_text("Menor = menos atraso. Se aparecerem estalos, use um valor maior.", x, r.y + 226, 12, UI_REGULAR, C_TEXT2);

    /* modo exclusivo / ASIO */
#ifdef _WIN32
    if (out_is_asio(g)) {
        ui_text("Driver ASIO", x, r.y + 262, 12.5f, UI_SEMIBOLD, C_TEXT);
        ui_text("Entrada e saída passam direto pelo driver da interface.", x, r.y + 282, 12, UI_REGULAR, C_TEXT2);
        Rectangle pb = {x + w - 150, r.y + 258, 150, 32};
        ui_tooltip(pb, "Abre o painel do fabricante para mudar o buffer e a taxa de amostragem do driver.");
        if (ui_button(pb, "@gear Painel do driver", UI_BTN_NORMAL)) engine_asio_panel(g->e);
    } else {
    bool ex = g->e->exclusive;
    ui_text("Modo exclusivo (WASAPI)", x, r.y + 262, 12.5f, UI_SEMIBOLD, C_TEXT);
    ui_text("Menos atraso no Windows; outros programas ficam sem som.", x, r.y + 282, 12, UI_REGULAR, C_TEXT2);
    ui_tooltip((Rectangle){x + w - 44, r.y + 264, 44, 24}, "Usa a placa de som só para este programa (WASAPI exclusivo): menos atraso, mas os outros programas ficam mudos.");
    if (ui_switch((Rectangle){x + w - 44, r.y + 264, 44, 24}, &ex, C_CYAN)) {
        g->e->exclusive = ex;
        apply_audio(g);
        if (ex && g->e->exclusive_failed) set_status(g, "A placa não aceitou o modo exclusivo; usando o compartilhado.");
    }
    }
#else
    ui_text("Modo exclusivo", x, r.y + 262, 12.5f, UI_SEMIBOLD, C_TEXT2);
    ui_text("Só no Windows. No Linux, use PipeWire/JACK com buffer pequeno.", x, r.y + 282, 12, UI_REGULAR, C_TEXT2);
#endif

    /* medidor de latência real */
    DrawRectangle((int)x, (int)r.y + 314, (int)w, 1, C_BORDER);
    ui_text("Medir a latência real", x, r.y + 330, 12.5f, UI_SEMIBOLD, C_TEXT);
    ui_text("Ligue um cabo da saída na entrada da interface (ou encoste o", x, r.y + 350, 12, UI_REGULAR, C_TEXT2);
    ui_text("microfone no alto-falante), com a entrada Guitarra ativa.", x, r.y + 367, 12, UI_REGULAR, C_TEXT2);
    Rectangle mb = {x, r.y + 394, 150, 36};
    char bl[40];
    if (g->lat_running) snprintf(bl, sizeof bl, "Medindo %d/5...", g->lat_runs + 1);
    else snprintf(bl, sizeof bl, "@play Medir");
    ui_tooltip(mb, "Toca 5 cliques e mede quanto tempo cada um leva para voltar pela entrada.");
    if (ui_button(mb, bl, UI_BTN_CYAN) && !g->lat_running) {
        if (param_geti(P_INPUT_MODE) != 2 || engine_capture_channels(g->e) == 0) {
            set_status(g, "Para medir, escolha a entrada Guitarra (o programa precisa ouvir a entrada).");
        } else {
            g->lat_running = true;
            g->lat_runs = 0;
            g->lat_next = 0;
            atomic_store(&g->e->lat_state, LAT_IDLE);
            set_status(g, "Medindo a latência: vão tocar 5 cliques curtos...");
        }
    }
    if (g->lat_ms > 0) {
        snprintf(t, sizeof t, "Ida e volta: %.1f ms", g->lat_ms);
        ui_text(t, x + 166, r.y + 396, 15, UI_BOLD, C_CYAN_HI);
        snprintf(t, sizeof t, "Da palhetada ao som: ~%.0f a %.0f ms (+ 12 a 17 ms para reconhecer a nota)", g->lat_ms + 12, g->lat_ms + 17);
        ui_text(t, x + 166, r.y + 416, 11.5f, UI_REGULAR, C_TEXT2);
    } else if (g->lat_ms < 0) {
        ui_text("Não ouvi o clique voltar. Confira o cabo e o volume.", x + 166, r.y + 404, 12, UI_REGULAR, C_RED);
    }
    ui_text("Dica: a parte do driver é a que mais varia entre computadores.", x, r.y + 470, 12, UI_REGULAR, C_TEXT2);
    if (ui_pressed_outside(r)) g->audio_open = false;
    if (IsKeyPressed(KEY_ESCAPE)) g->audio_open = false;
}

/* ------------------------------------------------------------------ */
/* Laço principal                                                      */
/* ------------------------------------------------------------------ */
static void draw_tabs(Gui *g)
{
    Rectangle bar = {324, 68, 944, 40};
    ui_panel(bar, 12, C_PANEL, C_BORDER);
    static const char *names[] = {"Som", "Efeitos", "Modulação", "Arp / Harmonia", "Looper", "Saída MIDI"};
    float w = (bar.width - 8 - 5 * 4) / 6;
    for (int i = 0; i < 6; i++) {
        Rectangle t = {bar.x + 4 + i * (w + 4), bar.y + 4, w, 32};
        bool on = g->tab == i, hov = ui_hover(t);
        if (on) { ui_rect(t, 9, C_AMBER_BG); ui_rect_lines(t, 9, 1, (Color){92, 68, 22, 255}); }
        else if (hov) ui_rect(t, 9, C_CTRL);
        ui_text_center(names[i], t, 13, on ? UI_SEMIBOLD : UI_REGULAR, on ? C_AMBER_HI : C_TEXT3);
        if (ui_clicked(t)) g->tab = i;
    }
}

static void draw_status_bar(Gui *g)
{
    DrawRectangle(0, UI_H - 28, UI_W, 28, C_BAR);
    DrawRectangle(0, UI_H - 29, UI_W, 1, (Color){31, 36, 44, 255});
    float y = UI_H - 21;
    if (GetTime() - g->status_time < 8 && g->status[0]) {
        ui_text(g->status, 16, y, 11.5f, UI_REGULAR, C_TEXT);
        return;
    }
    char t[200];
    ui_led(20, y + 7, 3.5f, g->e->dev_ok, C_GREEN);
    if (g->e->dev_ok) snprintf(t, sizeof t, "Áudio %u Hz · buffer %.1f ms", g->e->dev_rate, g->e->dev_latency_ms);
    else snprintf(t, sizeof t, "Áudio desligado");
    ui_text(t, 30, y, 11.5f, UI_REGULAR, C_TEXT2);
    float x = 30 + ui_text_width(t, 11.5f, UI_REGULAR) + 22;
    if (midi_out_is_open()) {
        snprintf(t, sizeof t, "Saída MIDI: %.40s", midi_out_open_name());
        ui_text(t, x, y, 11.5f, UI_REGULAR, C_TEXT2);
    }
    ui_text_right(ui_learn() ? "MIDI LEARN ATIVO" : "F1 tela cheia · Espaço sustain · Enter looper", UI_W - 16, y, 11.5f,
                  ui_learn() ? UI_SEMIBOLD : UI_REGULAR, ui_learn() ? C_AMBER : C_TEXT2);
}

int gui_run(Engine *e, const GuiOptions *opt)
{
    static Gui gs;
    Gui *g = &gs;
    memset(g, 0, sizeof *g);
    g->e = e;
    g->mouse_note = -1;
    g->drag_fx = -1;
    for (int i = 0; i < 64; i++) g->key_note[i] = -1;
    snprintf(g->preset_name, sizeof g->preset_name, "Inicial");

    /* Alta resolução (Retina) só no macOS. No Windows, com a escala da tela
       acima de 100%, o raylib desenha em pontos e entrega o mouse em pixels:
       os cliques caem no lugar errado. Fora do Mac tudo fica em pixels e a
       janela é aumentada aqui mesmo, de acordo com a escala da tela. */
    unsigned flags = FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT;
#ifdef __APPLE__
    flags |= FLAG_WINDOW_HIGHDPI;
#endif
    SetConfigFlags(flags);
    int win_w = opt && opt->width ? opt->width : UI_W, win_h = opt && opt->height ? opt->height : UI_H;
    InitWindow(win_w, win_h, "Virtual Synth");
#ifndef __APPLE__
    if (!(opt && (opt->width || opt->screenshot))) {
        float dpi = GetWindowScaleDPI().x;
        int mon = GetCurrentMonitor();
        int mw = GetMonitorWidth(mon), mh = GetMonitorHeight(mon);
        if (dpi > 1.05f && mw > 0 && mh > 0) {
            /* do tamanho da escala, mas cabendo na tela (barra de tarefas e bordas) */
            float k = fminf(dpi, fminf(mw * 0.92f / UI_W, mh * 0.85f / UI_H));
            if (k > 1.0f) {
                win_w = (int)(UI_W * k);
                win_h = (int)(UI_H * k);
                SetWindowSize(win_w, win_h);
                SetWindowPosition((mw - win_w) / 2, (mh - win_h) / 2);
            }
        }
    }
#endif
    SetWindowMinSize(640, 400);
    SetExitKey(KEY_NULL); /* Esc não fecha sem querer */
    SetTargetFPS(60);
    ui_init();

    build_device_lists(g);
    build_preset_list(g);
    if (!(opt && opt->no_settings)) load_settings(g);
    if (opt && opt->start_preset >= 0 && opt->start_preset < factory_preset_count()) {
        factory_preset_apply(opt->start_preset);
        g->preset_active = opt->start_preset;
        snprintf(g->preset_name, sizeof g->preset_name, "%s", factory_preset_name(opt->start_preset));
    }
    if (opt && opt->start_mode >= 0) param_set(P_INPUT_MODE, (float)opt->start_mode);
    if (opt && opt->start_tab) g->tab = opt->start_tab;
    g->last_mode = param_geti(P_INPUT_MODE);
    apply_audio(g);
    apply_midi(g);
    g->mout_last_src = param_geti(P_MOUT_SRC);
    if (g->mout_last_src) apply_midi_out(g);
    if (!g->status[0]) set_status(g, "Pronto! Toque pelo teclado do computador, pelo piano ou escolha outra entrada.");

    int frame = 0;
    while (!WindowShouldClose()) {
        int mode = param_geti(P_INPUT_MODE);
        if (mode != g->last_mode) {
            g->last_mode = mode;
            apply_audio(g);
            apply_midi(g);
            static const char *names[] = {"Entrada: teclado do computador / piano", "Entrada: MIDI", "Entrada: guitarra / microfone"};
            if (!g->audio_error[0]) set_status(g, names[mode]);
        }
        int msrc = param_geti(P_MOUT_SRC);
        if ((msrc != 0) != (g->mout_last_src != 0)) apply_midi_out(g);
        g->mout_last_src = msrc;
        if (ui_learn() && ui_learn_target() >= 0) {
            int cnt, cc = midi_last_cc(&cnt);
            if (cnt != g->learn_seen && cc >= 0) {
                midi_map(cc, ui_learn_target());
                midi_map_save("midi-learn.txt");
                char m[160];
                snprintf(m, sizeof m, "MIDI learn: CC%d -> %s", cc, param_def(ui_learn_target())->label);
                set_status(g, m);
                ui_set_learn_target(-1);
                g->learn_seen = cnt;
            }
        }
        if (recorder_active(&g->rec)) drain_recording(g);
        latency_update(g);
        handle_keyboard(g);
        if (IsKeyPressed(KEY_F1)) ToggleFullscreen();

        /* janelas por cima bloqueiam o resto */
        if (g->audio_open) ui_set_modal(true, audio_rect());
        else if (g->save_open) ui_set_modal(true, save_rect());
        else if (g->browser_open) ui_set_modal(true, browser_rect());
        else ui_set_modal(false, (Rectangle){0});

        ui_begin_frame(C_BG);
        draw_top_bar(g);
        draw_input_panel(g);
        draw_tabs(g);
        switch (g->tab) {
        case 0: tab_sound(g); break;
        case 1: tab_effects(g); break;
        case 2: tab_modulation(g); break;
        case 3: tab_arp(g); break;
        case 4: tab_looper(g); break;
        case 5: tab_midi_out(g); break;
        }
        draw_scope(g, (Rectangle){12, 572, 1256, 46});
        draw_piano(g, (Rectangle){12, 634, 1256, 130});
        draw_status_bar(g);
        ui_modal_content();
        if (g->save_open) draw_save_popup(g);
        if (g->browser_open) draw_preset_browser(g);
        if (g->audio_open) draw_audio_dialog(g);
        ui_end_frame();

        frame++;
        if (opt && opt->screenshot && frame == opt->screenshot_frame) ui_screenshot(opt->screenshot);
        if (opt && opt->exit_frame && frame >= opt->exit_frame) break;
    }

    if (recorder_active(&g->rec)) toggle_recording(g);
    if (!(opt && opt->no_settings)) save_settings(g);
    ui_shutdown();
    CloseWindow();
    return 0;
}
