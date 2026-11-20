/*
 * Interface gráfica (raylib + raygui).
 *
 * Tudo é desenhado numa tela virtual de 1280x800 que é escalada para o
 * tamanho da janela, então funciona em telas menores (ex.: 1366x768).
 *
 *  ┌ barra superior: presets · MIDI learn · gravar · pânico · volume ┐
 *  ├ ENTRADA (teclado/MIDI/guitarra) ┬ abas: Som · Efeitos · Modulação · Arpejador · Looper ┤
 *  ├ osciloscópio ───────────────────┴────────────────────────────────────────────────┤
 *  ├ piano clicável ─────────────────────────────────────────────────────────────────┤
 *  └ barra de status ────────────────────────────────────────────────────────────────┘
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "raylib.h"
#define RAYGUI_IMPLEMENTATION
#include "raygui.h"
#include "dark/style_dark.h"

#include "engine.h"
#include "fx.h"
#include "gui.h"
#include "midi.h"
#include "params.h"
#include "pitch.h"
#include "recorder.h"

#define W 1280
#define H 800
#define PRESET_DIR "presets"
#define REC_DIR    "gravacoes"
#define MAX_USER_PRESETS 64

/* ------------------------------------------------------------------ */
/* Estado da interface                                                 */
/* ------------------------------------------------------------------ */
typedef struct {
    Rectangle   r;
    const char *items;
    int        *active;
    bool       *edit;
    int         id;
} Dropdown;

enum { DD_PRESET, DD_OUT, DD_IN, DD_MIDI, DD_MOUT };

typedef struct {
    Engine *e;
    int     tab;

    /* dispositivos */
    char out_items[4096], in_items[4096], midi_items[4096], mout_items[4096];
    int  out_active, in_active, midi_active, mout_active, in_ch;
    bool out_edit, in_edit, midi_edit, mout_edit, ch_edit;
    int  mout_last_src, mout_seen;
    double mout_blink;
    char audio_error[256];

    /* presets */
    char preset_items[8192];
    char user_presets[MAX_USER_PRESETS][128];
    int  n_user, preset_active;
    bool preset_edit, name_edit;
    char preset_name[64], name_backup[64];

    /* MIDI learn */
    bool learn;
    int  learn_target, learn_seen;

    /* notas tocadas pela interface */
    int  key_note[64];
    int  mouse_note;
    bool sustain;

    /* gravação */
    Recorder rec;
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

    Dropdown dd[6];
    int      ndd;
    int      last_mode;
} Gui;

static Font  g_font;
static int   g_text;
static Color C_BG, C_LINE, C_TEXT, C_DIM, C_ACCENT, C_PANEL;

/* ------------------------------------------------------------------ */
/* Utilidades                                                          */
/* ------------------------------------------------------------------ */
static void set_status(Gui *g, const char *msg)
{
    snprintf(g->status, sizeof g->status, "%s", msg);
    g->status_time = GetTime();
}

static void text(const char *s, float x, float y, Color c)
{
    DrawTextEx(g_font, s, (Vector2){x, y}, (float)g_text, 1, c);
}

static void text_size(const char *s, float x, float y, float size, Color c)
{
    DrawTextEx(g_font, s, (Vector2){x, y}, size, 1, c);
}

static void defer_dropdown(Gui *g, Rectangle r, const char *items, int *active, bool *edit, int id)
{
    if (g->ndd < 6) g->dd[g->ndd++] = (Dropdown){r, items, active, edit, id};
}

static bool any_dropdown_open(Gui *g)
{
    return g->out_edit || g->in_edit || g->midi_edit || g->preset_edit || g->mout_edit;
}

/* Desenha o controle adequado ao tipo do parâmetro:
   [rótulo] [slider/combobox/checkbox] [valor] */
static void pcontrol_ex(Gui *g, Rectangle row, int id, float label_w, float value_w)
{
    const ParamDef *d = param_def(id);
    bool was_locked = GuiIsLocked();
    bool learnable = !(d->flags & F_HIDDEN);
    if (g->learn) GuiLock();

    Rectangle lab = {row.x, row.y, label_w, row.height};
    Rectangle ctl = {row.x + label_w, row.y, row.width - label_w - value_w, row.height};
    Rectangle val = {ctl.x + ctl.width + 8, row.y, value_w - 8, row.height};

    if (d->kind == K_BOOL) {
        bool on = param_geti(id);
        Rectangle box = {row.x, row.y + (row.height - 20) / 2, 20, 20};
        if (GuiCheckBox(box, d->label, &on) || on != (bool)param_geti(id)) param_set(id, on);
        ctl = (Rectangle){row.x, row.y, row.width, row.height};
    } else {
        GuiLabel(lab, d->label);
        if (d->kind == K_CHOICE) {
            int a = param_geti(id);
            GuiComboBox(ctl, d->choices, &a);
            if (a != param_geti(id)) param_set(id, (float)a);
        } else {
            float n = param_get_norm(id);
            float before = n;
            GuiSlider(ctl, NULL, NULL, &n, 0, 1);
            if (n != before) param_set_norm(id, n);
            char buf[32];
            param_format(id, buf, sizeof buf);
            GuiLabel(val, buf);
        }
    }

    /* MIDI learn: CC atribuído aparece ao lado; clique escolhe o alvo */
    int cc = midi_param_cc(id);
    if (cc >= 0 && g->learn) {
        char t[16];
        snprintf(t, sizeof t, "CC%d", cc);
        float tx = (d->kind == K_BOOL || value_w < 40) ? row.x + row.width - 48 : val.x;
        DrawRectangle((int)tx - 2, (int)row.y + 2, 50, (int)row.height - 4, (Color){50, 35, 20, 255});
        text(t, tx + 2, row.y + (row.height - g_text) / 2, C_ACCENT);
    }
    if (g->learn && learnable) {
        Rectangle hit = d->kind == K_BOOL ? ctl : (Rectangle){row.x, row.y, row.width, row.height};
        DrawRectangleLinesEx(hit, 1, Fade(C_ACCENT, 0.35f));
        if (!was_locked && CheckCollisionPointRec(GetMousePosition(), hit)) {
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                g->learn_target = id;
                midi_last_cc(&g->learn_seen);
                char m[160];
                snprintf(m, sizeof m, "MIDI learn: mova um knob/fader do seu controlador para \"%s\"", d->label);
                set_status(g, m);
            } else if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && cc >= 0) {
                midi_map(cc, -1);
                midi_map_save("midi-learn.txt");
                set_status(g, "MIDI learn: ligação removida");
            }
        }
        if (g->learn_target == id) DrawRectangleLinesEx(hit, 2, C_ACCENT);
    }
    if (g->learn && !was_locked) GuiUnlock();
}

static void pcontrol(Gui *g, Rectangle row, int id) { pcontrol_ex(g, row, id, 118, 86); }

static void group(Rectangle r, const char *title) { GuiGroupBox(r, title); }

/* ------------------------------------------------------------------ */
/* Dispositivos                                                        */
/* ------------------------------------------------------------------ */
static void build_device_lists(Gui *g)
{
    Engine *e = g->e;
    engine_refresh_devices(e);
    snprintf(g->out_items, sizeof g->out_items, "Padrão do sistema");
    for (int i = 0; i < e->nplay; i++) {
        strncat(g->out_items, ";", sizeof g->out_items - strlen(g->out_items) - 1);
        strncat(g->out_items, e->play[i].name, sizeof g->out_items - strlen(g->out_items) - 1);
    }
    snprintf(g->in_items, sizeof g->in_items, "Padrão do sistema");
    for (int i = 0; i < e->ncap; i++) {
        strncat(g->in_items, ";", sizeof g->in_items - strlen(g->in_items) - 1);
        strncat(g->in_items, e->cap[i].name, sizeof g->in_items - strlen(g->in_items) - 1);
    }
    int n = midi_refresh();
    if (n == 0) snprintf(g->midi_items, sizeof g->midi_items, midi_available() ? "(nenhum dispositivo)" : "(sem suporte a MIDI)");
    else {
        g->midi_items[0] = 0;
        for (int i = 0; i < n; i++) {
            if (i) strncat(g->midi_items, ";", sizeof g->midi_items - strlen(g->midi_items) - 1);
            strncat(g->midi_items, midi_device_name(i), sizeof g->midi_items - strlen(g->midi_items) - 1);
        }
    }
    if (g->midi_active >= n) g->midi_active = 0;

    /* saída MIDI: porta virtual (Linux/macOS) + dispositivos */
    g->mout_items[0] = 0;
    if (midi_virtual_supported()) snprintf(g->mout_items, sizeof g->mout_items, "Porta virtual \"Sintetizador\"");
    for (int i = 0; i < midi_out_count(); i++) {
        if (g->mout_items[0]) strncat(g->mout_items, ";", sizeof g->mout_items - strlen(g->mout_items) - 1);
        strncat(g->mout_items, midi_out_name(i), sizeof g->mout_items - strlen(g->mout_items) - 1);
    }
    if (!g->mout_items[0])
        snprintf(g->mout_items, sizeof g->mout_items, midi_available() ? "(nenhuma saída MIDI)" : "(sem suporte a MIDI)");
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

static void apply_audio(Gui *g)
{
    int mode = param_geti(P_INPUT_MODE);
    int cap  = mode == 2 ? g->in_active - 1 : -2;
    g->audio_error[0] = 0;
    if (engine_open(g->e, g->out_active - 1, cap, g->in_ch) < 0) {
        if (cap != -2 && engine_open(g->e, g->out_active - 1, -2, 0) == 0)
            snprintf(g->audio_error, sizeof g->audio_error, "Não abriu a entrada de áudio. Tente outro dispositivo.");
        else
            snprintf(g->audio_error, sizeof g->audio_error, "Não abriu a saída de áudio!");
        set_status(g, g->audio_error);
    }
    int chans = engine_capture_channels(g->e);
    if (chans > 0 && g->in_ch >= chans) { g->in_ch = 0; atomic_store(&g->e->cap_channel, 0); }
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
    g->preset_items[0] = 0;
    for (int i = 0; i < factory_preset_count(); i++) {
        if (i) strncat(g->preset_items, ";", sizeof g->preset_items - strlen(g->preset_items) - 1);
        strncat(g->preset_items, factory_preset_name(i), sizeof g->preset_items - strlen(g->preset_items) - 1);
    }
    for (int i = 0; i < g->n_user; i++) {
        strncat(g->preset_items, ";* ", sizeof g->preset_items - strlen(g->preset_items) - 1);
        strncat(g->preset_items, g->user_presets[i], sizeof g->preset_items - strlen(g->preset_items) - 1);
    }
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
    char m[160];
    snprintf(m, sizeof m, "Preset carregado: %s", g->preset_name);
    set_status(g, m);
}

static void save_preset(Gui *g)
{
    char clean[64];
    int j = 0;
    for (int i = 0; g->preset_name[i] && j < 60; i++) {
        unsigned char c = (unsigned char)g->preset_name[i];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == ';') continue;
        clean[j++] = (char)c;
    }
    clean[j] = 0;
    if (!j) { set_status(g, "Digite um nome para o preset"); return; }
    MakeDirectory(PRESET_DIR);
    char path[300];
    snprintf(path, sizeof path, PRESET_DIR "/%s.txt", clean);
    if (preset_save(path) == 0) {
        build_preset_list(g);
        for (int i = 0; i < g->n_user; i++)
            if (!strcmp(g->user_presets[i], clean)) g->preset_active = factory_preset_count() + i;
        char m[360];
        snprintf(m, sizeof m, "Preset salvo em %s", path);
        set_status(g, m);
    } else {
        set_status(g, "Não foi possível salvar o preset");
    }
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
        atomic_store(&g->e->rec_on, true);
        g->rec_start = GetTime();
        set_status(g, "Gravando...");
    } else {
        atomic_store(&g->e->rec_on, false);
        drain_recording(g);
        char m[600];
        snprintf(m, sizeof m, "Gravação salva: %s (%.1f s)", g->rec.path, g->rec.frames / g->e->sr);
        recorder_stop(&g->rec);
        set_status(g, m);
    }
}

/* ------------------------------------------------------------------ */
/* Teclado do computador e piano                                       */
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

static void looper_main_button(Gui *g);

static void handle_keyboard(Gui *g)
{
    Engine *e = g->e;
    if (g->name_edit || g->ch_edit) return; /* digitando um nome: teclas não tocam */

    int base = 12 * (param_geti(P_KEYS_OCTAVE) + 1);
    for (int i = 0; i < NKEYS; i++) {
        if (IsKeyPressed(KEYMAP[i].key)) {
            int note = base + KEYMAP[i].semi;
            if (note <= 127) {
                g->key_note[i] = note;
                engine_ui_event(e, EV_NOTE_ON, note, 100);
            }
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
    if (IsKeyPressed(KEY_ENTER))  looper_main_button(g);
}

#define PIANO_LOW  36 /* C2 */
#define PIANO_HIGH 96 /* C7 */

static bool is_black(int n) { int k = n % 12; return k == 1 || k == 3 || k == 6 || k == 8 || k == 10; }

static Rectangle key_rect(Rectangle area, int note, float white_w)
{
    int whites = 0;
    for (int n = PIANO_LOW; n < note; n++) if (!is_black(n)) whites++;
    if (!is_black(note)) return (Rectangle){area.x + whites * white_w, area.y, white_w, area.height};
    float bw = white_w * 0.62f;
    return (Rectangle){area.x + whites * white_w - bw / 2, area.y, bw, area.height * 0.6f};
}

static int piano_hit(Rectangle area, Vector2 m, float white_w)
{
    if (!CheckCollisionPointRec(m, area)) return -1;
    for (int n = PIANO_LOW; n <= PIANO_HIGH; n++)
        if (is_black(n) && CheckCollisionPointRec(m, key_rect(area, n, white_w))) return n;
    for (int n = PIANO_LOW; n <= PIANO_HIGH; n++)
        if (!is_black(n) && CheckCollisionPointRec(m, key_rect(area, n, white_w))) return n;
    return -1;
}

static void draw_piano(Gui *g, Rectangle area)
{
    Engine *e = g->e;
    int whites = 0;
    for (int n = PIANO_LOW; n <= PIANO_HIGH; n++) if (!is_black(n)) whites++;
    float ww = area.width / whites;

    /* faixa que mostra onde está o teclado do computador */
    int base = 12 * (param_geti(P_KEYS_OCTAVE) + 1);
    Rectangle a = key_rect(area, base < PIANO_LOW ? PIANO_LOW : base, ww);
    int hi = base + 28 > PIANO_HIGH ? PIANO_HIGH : base + 28;
    Rectangle b = key_rect(area, hi, ww);
    if (base <= PIANO_HIGH) {
        DrawRectangle((int)a.x, (int)area.y - 7, (int)(b.x + b.width - a.x), 4, Fade(C_ACCENT, 0.6f));
    }

    /* mouse: clica e arrasta (glissando) */
    bool locked = GuiIsLocked();
    Vector2 m = GetMousePosition();
    if (!locked && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        int n = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || g->mouse_note >= 0 ? piano_hit(area, m, ww) : -1;
        if (n != g->mouse_note) {
            if (g->mouse_note >= 0) engine_ui_event(e, EV_NOTE_OFF, g->mouse_note, 0);
            if (n >= 0) {
                /* clicar mais embaixo na tecla = mais forte */
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
    for (int i = 0; i < NKEYS; i++) if (g->key_note[i] >= 0) lit[g->key_note[i]] |= 2;
    if (g->mouse_note >= 0) lit[g->mouse_note] |= 2;

    for (int pass = 0; pass < 2; pass++) {
        for (int n = PIANO_LOW; n <= PIANO_HIGH; n++) {
            if (is_black(n) != (pass == 1)) continue;
            Rectangle r = key_rect(area, n, ww);
            Color c = pass ? (Color){30, 30, 34, 255} : (Color){236, 236, 232, 255};
            if (lit[n]) c = pass ? ColorBrightness(C_ACCENT, -0.2f) : C_ACCENT;
            DrawRectangleRec((Rectangle){r.x + 1, r.y, r.width - 2, r.height - 1}, c);
            if (!pass && n % 12 == 0) {
                char nm[8];
                note_name(n, nm, sizeof nm);
                text(nm, r.x + 5, r.y + r.height - 22, lit[n] ? WHITE : (Color){120, 120, 120, 255});
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Painel de entrada                                                   */
/* ------------------------------------------------------------------ */
static void draw_level_meter(Rectangle r, float db, float gate_db)
{
    DrawRectangleRec(r, (Color){20, 20, 24, 255});
    float t = (db + 70.0f) / 70.0f;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    Color c = db > -6 ? RED : (db > gate_db ? (Color){80, 200, 120, 255} : (Color){90, 110, 130, 255});
    DrawRectangle((int)r.x, (int)r.y, (int)(r.width * t), (int)r.height, c);
    float gx = r.x + r.width * (gate_db + 70.0f) / 70.0f;
    DrawRectangle((int)gx - 1, (int)r.y - 3, 3, (int)r.height + 6, C_ACCENT);
    DrawRectangleLinesEx(r, 1, C_LINE);
}

static void draw_tuner(Gui *g, Rectangle r)
{
    Engine *e = g->e;
    float hz = atomic_load(&e->guitar.st_hz);
    double now = GetTime();
    if (hz > 0) {
        float midi = hz_to_midi(hz);
        g->tuner_note  = (int)lroundf(midi);
        g->tuner_cents += ((midi - g->tuner_note) * 100.0f - g->tuner_cents) * 0.3f;
        g->tuner_hz    = hz;
        g->tuner_time  = now;
    }
    group(r, "Afinador");
    bool fresh = now - g->tuner_time < 0.6;
    char nm[8] = "--";
    if (g->tuner_time > 0 && fresh) note_name(g->tuner_note, nm, sizeof nm);
    Color nc = !fresh ? C_DIM : (fabsf(g->tuner_cents) < 5 ? (Color){80, 220, 120, 255} : C_TEXT);
    text_size(nm, r.x + 14, r.y + 22, 32, nc);
    if (fresh) {
        char t[48];
        snprintf(t, sizeof t, "%.1f Hz %+.0f cents", g->tuner_hz, g->tuner_cents);
        text(t, r.x + 100, r.y + 18, C_TEXT);
    }
    Rectangle bar = {r.x + 100, r.y + 44, r.width - 112, 16};
    DrawRectangleRec(bar, (Color){20, 20, 24, 255});
    DrawRectangle((int)(bar.x + bar.width / 2 - bar.width * 0.05f), (int)bar.y, (int)(bar.width * 0.1f), (int)bar.height, Fade(GREEN, 0.25f));
    DrawLine((int)(bar.x + bar.width / 2), (int)bar.y - 3, (int)(bar.x + bar.width / 2), (int)(bar.y + bar.height + 3), C_DIM);
    if (fresh) {
        float c = g->tuner_cents < -50 ? -50 : (g->tuner_cents > 50 ? 50 : g->tuner_cents);
        float x = bar.x + bar.width * (0.5f + c / 100.0f);
        DrawRectangle((int)x - 2, (int)bar.y - 2, 4, (int)bar.height + 4, nc);
    }
    text("-50", bar.x, bar.y + 22, C_DIM);
    text("0", bar.x + bar.width / 2 - 4, bar.y + 22, C_DIM);
    text("+50", bar.x + bar.width - 28, bar.y + 22, C_DIM);
}

static void draw_input_panel(Gui *g)
{
    Engine *e = g->e;
    Rectangle p = {8, 50, 330, 470};
    GuiPanel(p, "ENTRADA");
    float x = p.x + 10, w = p.width - 20;

    int mode = param_geti(P_INPUT_MODE);
    GuiToggleGroup((Rectangle){x, 88, (w - 8) / 3, 30}, "Teclado;MIDI;Guitarra", &mode);
    if (mode != param_geti(P_INPUT_MODE)) param_set(P_INPUT_MODE, (float)mode);

    GuiLabel((Rectangle){x, 126, w, 20}, "Saída de áudio");
    defer_dropdown(g, (Rectangle){x, 146, w, 26}, g->out_items, &g->out_active, &g->out_edit, DD_OUT);

    if (mode == 0) {
        float y = 186;
        text("Toque pelo teclado do computador:", x, y, C_TEXT);
        y += 26;
        /* desenha as duas fileiras como teclas de piano: pretas entre as brancas */
        static const char *whites[2] = {"QWERTYUIOP", "ZXCVBNM,./"};
        static const char *blacks[2] = {"23 567 90", "SD GHJ L;"};
        for (int rr = 0; rr < 2; rr++) {
            float ky = y + rr * 58;
            for (int i = 0; blacks[rr][i]; i++) {
                if (blacks[rr][i] == ' ') continue;
                Rectangle k = {x + 8 + i * 30.0f + 15, ky, 26, 22};
                DrawRectangleRec(k, (Color){30, 30, 34, 255});
                DrawRectangleLinesEx(k, 1, C_LINE);
                char c[2] = {blacks[rr][i], 0};
                text(c, k.x + 9, k.y + 3, C_TEXT);
            }
            for (int i = 0; whites[rr][i]; i++) {
                Rectangle k = {x + 8 + i * 30.0f, ky + 26, 26, 22};
                DrawRectangleRec(k, (Color){232, 232, 228, 255});
                char c[2] = {whites[rr][i], 0};
                text(c, k.x + 9, k.y + 3, (Color){30, 30, 30, 255});
            }
        }
        y += 2 * 58 + 10;
        text("Setas < >   oitava", x, y, C_DIM); y += 20;
        text("Espaço      pedal de sustain", x, y, C_DIM); y += 20;
        text("Enter       botão do looper", x, y, C_DIM); y += 26;
        text("Ou clique no piano lá embaixo:", x, y, C_DIM); y += 20;
        text("mais embaixo na tecla = mais forte", x, y, C_DIM); y += 20;
        y += 8;
        pcontrol_ex(g, (Rectangle){x, y, w, 26}, P_KEYS_OCTAVE, 90, 50);
    } else if (mode == 1) {
        GuiLabel((Rectangle){x, 182, w, 20}, "Dispositivo MIDI");
        defer_dropdown(g, (Rectangle){x, 202, w - 92, 26}, g->midi_items, &g->midi_active, &g->midi_edit, DD_MIDI);
        if (GuiButton((Rectangle){x + w - 86, 202, 86, 26}, "#211#Atualizar")) {
            midi_close();
            midi_out_close();
            build_device_lists(g);
            apply_midi(g);
            apply_midi_out(g);
        }
        const char *st = !midi_available() ? "Compilado sem MIDI (SYNTH_MIDI=OFF)"
                       : midi_is_open() ? "Conectado" : "Nenhum dispositivo conectado";
        text(st, x, 240, midi_is_open() ? (Color){80, 220, 120, 255} : C_DIM);
        float y = 276;
        static const char *lines[] = {
            "Pitch bend: +/- 2 semitons",
            "Pedal (CC64): sustain",
            "Roda de mod. (CC1): vibrato",
            "CC74: corte  CC71: ressonância",
            "CC7: volume geral",
            "",
            "Use \"MIDI Learn\" (barra de cima)",
            "para ligar qualquer knob ou",
            "fader do seu controlador a",
            "qualquer controle da tela.",
        };
        for (unsigned i = 0; i < sizeof lines / sizeof lines[0]; i++, y += 19) text(lines[i], x, y, C_DIM);
    } else {
        GuiLabel((Rectangle){x, 182, w, 20}, "Entrada (guitarra / microfone)");
        defer_dropdown(g, (Rectangle){x, 202, w, 26}, g->in_items, &g->in_active, &g->in_edit, DD_IN);

        int chans = engine_capture_channels(e);
        GuiLabel((Rectangle){x, 238, 60, 24}, "Canal");
        int ch = g->in_ch + 1;
        if (GuiSpinner((Rectangle){x + 56, 238, 96, 24}, NULL, &ch, 1, chans > 0 ? chans : 1, g->ch_edit)) g->ch_edit = !g->ch_edit;
        if (ch - 1 != g->in_ch) { g->in_ch = ch - 1; atomic_store(&e->cap_channel, g->in_ch); }
        pcontrol_ex(g, (Rectangle){x + 170, 238, w - 170, 24}, P_GTR_BENDS, 0, 0);

        pcontrol_ex(g, (Rectangle){x, 270, w, 22}, P_GTR_GATE, 74, 64);
        pcontrol_ex(g, (Rectangle){x, 298, w, 22}, P_GTR_GAIN, 74, 64);
        GuiLabel((Rectangle){x, 324, 74, 18}, "Nível");
        draw_level_meter((Rectangle){x + 74, 328, w - 74 - 64, 12}, atomic_load(&e->guitar.st_db), param_get(P_GTR_GATE));
        char dbt[16];
        float db = atomic_load(&e->guitar.st_db);
        snprintf(dbt, sizeof dbt, db < -90 ? "--" : "%.0f dB", db);
        text(dbt, x + w - 56, 324, C_DIM);
        pcontrol_ex(g, (Rectangle){x, 352, w, 22}, P_GTR_BLEND, 74, 64);
        pcontrol_ex(g, (Rectangle){x, 380, w, 22}, P_GTR_OCTAVE, 74, 64);
        draw_tuner(g, (Rectangle){x, 414, w, 96});
    }
}

/* ------------------------------------------------------------------ */
/* Abas                                                                */
/* ------------------------------------------------------------------ */
static void tab_sound(Gui *g)
{
    float x1 = 356, x2 = 818, w = 446;
    group((Rectangle){x1, 96, w, 138}, "Osciladores");
    float y = 110;
    int wv = param_geti(P_OSC1_WAVE);
    GuiLabel((Rectangle){x1 + 10, y, 110, 26}, "Onda OSC1");
    GuiToggleGroup((Rectangle){x1 + 128, y, 74, 26}, "Seno;Serra;Quadrada;Triângulo", &wv);
    if (wv != param_geti(P_OSC1_WAVE)) param_set(P_OSC1_WAVE, (float)wv);
    y += 30;
    wv = param_geti(P_OSC2_WAVE);
    GuiLabel((Rectangle){x1 + 10, y, 110, 26}, "Onda OSC2");
    GuiToggleGroup((Rectangle){x1 + 128, y, 74, 26}, "Seno;Serra;Quadrada;Triângulo", &wv);
    if (wv != param_geti(P_OSC2_WAVE)) param_set(P_OSC2_WAVE, (float)wv);
    pcontrol(g, (Rectangle){x1 + 10, 172, w - 20, 22}, P_OSC_MIX);
    pcontrol(g, (Rectangle){x1 + 10, 202, w - 20, 22}, P_DETUNE);

    group((Rectangle){x2, 96, w, 138}, "Filtro passa-baixa");
    pcontrol(g, (Rectangle){x2 + 10, 112, w - 20, 22}, P_CUTOFF);
    pcontrol(g, (Rectangle){x2 + 10, 142, w - 20, 22}, P_RESONANCE);
    pcontrol(g, (Rectangle){x2 + 10, 172, w - 20, 22}, P_GLIDE);
    pcontrol(g, (Rectangle){x2 + 10, 202, w - 20, 22}, P_SYNTH_LEVEL);

    group((Rectangle){x1, 248, w, 138}, "Envelope de volume");
    group((Rectangle){x2, 248, w, 138}, "Envelope do filtro (fonte \"Env. filtro\")");
    const int amp[4] = {P_ATTACK, P_DECAY, P_SUSTAIN, P_RELEASE};
    const int flt[4] = {P_FATTACK, P_FDECAY, P_FSUSTAIN, P_FRELEASE};
    for (int i = 0; i < 4; i++) {
        pcontrol(g, (Rectangle){x1 + 10, 264 + i * 30.0f, w - 20, 22}, amp[i]);
        pcontrol(g, (Rectangle){x2 + 10, 264 + i * 30.0f, w - 20, 22}, flt[i]);
    }

    group((Rectangle){x1, 400, w, 108}, "LFO");
    pcontrol(g, (Rectangle){x1 + 10, 418, w - 20, 22}, P_LFO_RATE);
    pcontrol(g, (Rectangle){x1 + 10, 448, w - 20, 22}, P_MODWHEEL);
    text("Roda de mod. = vibrato (aba Modulação)", x1 + 10, 480, C_DIM);

    group((Rectangle){x2, 400, w, 108}, "Informações");
    char t[160];
    Engine *e = g->e;
    snprintf(t, sizeof t, "Vozes tocando: %d de %d", synth_active_voices(&e->synth), MAX_VOICES);
    text(t, x2 + 12, 416, C_DIM);
    if (e->dev_ok) {
        snprintf(t, sizeof t, "Áudio: %u Hz, buffer %.1f ms", e->dev_rate, e->dev_latency_ms);
    } else snprintf(t, sizeof t, "Áudio: desligado");
    text(t, x2 + 12, 440, C_DIM);
    snprintf(t, sizeof t, "Saída: %.36s", e->dev_ok && e->dev_name[0] ? e->dev_name : "padrão do sistema");
    text(t, x2 + 12, 464, C_DIM);
}

static void tab_effects(Gui *g)
{
    /* resumo da cadeia */
    char chain[256] = "Cadeia:  synth";
    for (int s = 0; s < FX_COUNT; s++) {
        int t = param_geti(P_FX_ORDER0 + s);
        if (!param_geti(fx_param_on(t))) continue;
        strncat(chain, "  ->  ", sizeof chain - strlen(chain) - 1);
        strncat(chain, fx_name(t), sizeof chain - strlen(chain) - 1);
    }
    strncat(chain, "  ->  looper  ->  saída", sizeof chain - strlen(chain) - 1);
    text(chain, 360, 94, C_DIM);

    static const int ctl[FX_COUNT][4] = {
        {P_DIST_DRIVE, P_DIST_TONE, P_DIST_MIX, -1},
        {P_CHO_RATE, P_CHO_DEPTH, P_CHO_MIX, -1},
        {P_PHA_RATE, P_PHA_DEPTH, P_PHA_FB, P_PHA_MIX},
        {P_DLY_SYNC, P_DLY_TIME, P_DLY_FB, P_DLY_MIX},
        {P_REV_SIZE, P_REV_DAMP, P_REV_MIX, -1},
    };
    for (int s = 0; s < FX_COUNT; s++) {
        int t = param_geti(P_FX_ORDER0 + s);
        float y = 118 + s * 79.0f;
        Rectangle row = {356, y, 906, 73};
        bool on = param_geti(fx_param_on(t));
        if (on) DrawRectangleRec(row, Fade(C_ACCENT, 0.10f));
        DrawRectangleLinesEx(row, 1, on ? Fade(C_ACCENT, 0.6f) : C_LINE);

        char num[4];
        snprintf(num, sizeof num, "%d", s + 1);
        text(num, 364, y + 28, on ? C_ACCENT : C_DIM);
        if (GuiButton((Rectangle){382, y + 6, 28, 28}, "#121#") && s > 0) {
            int a = param_geti(P_FX_ORDER0 + s - 1);
            param_set(P_FX_ORDER0 + s - 1, (float)t);
            param_set(P_FX_ORDER0 + s, (float)a);
        }
        if (GuiButton((Rectangle){382, y + 39, 28, 28}, "#120#") && s < FX_COUNT - 1) {
            int b = param_geti(P_FX_ORDER0 + s + 1);
            param_set(P_FX_ORDER0 + s + 1, (float)t);
            param_set(P_FX_ORDER0 + s, (float)b);
        }
        pcontrol_ex(g, (Rectangle){422, y + 8, 140, 24}, fx_param_on(t), 0, 0);
        if (t == FX_DELAY) pcontrol_ex(g, (Rectangle){422, y + 42, 140, 24}, P_DLY_PING, 0, 0);

        for (int k = 0; k < 4; k++) {
            if (ctl[t][k] < 0) continue;
            float cx = 580 + (k % 2) * 344.0f, cy = y + 8 + (k / 2) * 34.0f;
            pcontrol_ex(g, (Rectangle){cx, cy, 334, 24}, ctl[t][k], 110, 84);
        }
    }
}

static void tab_modulation(Gui *g)
{
    text("Cada cabo liga uma FONTE a um DESTINO. \"Via\" multiplica pela segunda fonte:", 360, 96, C_DIM);
    text("ex.: LFO via Roda mod. -> Altura = vibrato controlado pela roda. Quantidade: altura em", 360, 116, C_DIM);
    text("semitons, corte em oitavas. Valores negativos invertem o efeito.", 360, 136, C_DIM);
    float cols[4] = {440, 640, 840, 1010};
    const char *hdr[4] = {"Fonte", "Via", "Destino", "Quantidade"};
    for (int i = 0; i < 4; i++) text(hdr[i], cols[i], 170, C_TEXT);
    for (int i = 0; i < MOD_SLOTS; i++) {
        float y = 196 + i * 50.0f;
        char lab[16];
        snprintf(lab, sizeof lab, "Cabo %d", i + 1);
        bool active = param_geti(P_MOD_SRC(i)) != 0 && param_get(P_MOD_AMT(i)) != 0;
        text(lab, 362, y + 4, active ? C_ACCENT : C_DIM);
        pcontrol_ex(g, (Rectangle){cols[0], y, 190, 28}, P_MOD_SRC(i), 0, 0);
        pcontrol_ex(g, (Rectangle){cols[1], y, 190, 28}, P_MOD_VIA(i), 0, 0);
        pcontrol_ex(g, (Rectangle){cols[2], y, 160, 28}, P_MOD_DST(i), 0, 0);
        pcontrol_ex(g, (Rectangle){cols[3], y + 2, 252, 24}, P_MOD_AMT(i), 0, 70);
    }
}

static void tempo_group(Gui *g, Rectangle r)
{
    group(r, "Tempo");
    float x = r.x + 10, w = r.width - 20;
    pcontrol(g, (Rectangle){x, r.y + 18, w, 24}, P_BPM);
    if (GuiButton((Rectangle){x, r.y + 52, 150, 28}, "Tap tempo")) {
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
    pcontrol_ex(g, (Rectangle){x + 170, r.y + 54, 150, 24}, P_METRO_ON, 0, 0);
    int beat = atomic_load(&g->e->beat);
    for (int i = 0; i < 4; i++) {
        Color c = i == beat ? (i == 0 ? C_ACCENT : C_TEXT) : Fade(C_TEXT, 0.15f);
        DrawCircle((int)(x + w - 70 + i * 20), (int)(r.y + 66), 7, c);
    }
    pcontrol(g, (Rectangle){x, r.y + 90, w, 24}, P_METRO_VOL);
}

static void tab_arp(Gui *g)
{
    group((Rectangle){356, 96, 446, 412}, "Arpejador");
    float x = 366, w = 426;
    bool on = param_geti(P_ARP_ON);
    if (GuiToggle((Rectangle){x, 114, w, 36}, on ? "#131#Arpejador LIGADO" : "#131#Arpejador desligado", &on) || on != (bool)param_geti(P_ARP_ON))
        param_set(P_ARP_ON, on);
    pcontrol(g, (Rectangle){x, 166, w, 26}, P_ARP_MODE);
    pcontrol(g, (Rectangle){x, 202, w, 26}, P_ARP_RATE);
    pcontrol(g, (Rectangle){x, 238, w, 24}, P_ARP_OCT);
    pcontrol(g, (Rectangle){x, 272, w, 24}, P_ARP_GATE);
    pcontrol_ex(g, (Rectangle){x, 308, w, 24}, P_ARP_HOLD, 0, 0);
    static const char *help[] = {
        "Segure um acorde (teclado, MIDI ou piano):",
        "as notas tocam uma de cada vez, no BPM.",
        "Com a guitarra, a nota vira arpejo.",
        "Segurar: continua depois de soltar.",
    };
    for (int i = 0; i < 4; i++) text(help[i], 366, 346 + i * 20.0f, C_DIM);
    text("O BPM também comanda o metrônomo", 366, 436, C_DIM);
    text("e o delay sincronizado.", 366, 456, C_DIM);

    tempo_group(g, (Rectangle){818, 96, 446, 126});

    group((Rectangle){818, 236, 446, 272}, "Harmonizador");
    float hx = 828, hw = 426;
    pcontrol_ex(g, (Rectangle){hx, 252, hw, 24}, P_HARM_ON, 0, 0);
    pcontrol(g, (Rectangle){hx, 284, hw, 26}, P_HARM_INTERVAL);
    pcontrol(g, (Rectangle){hx, 316, hw, 26}, P_HARM_KEY);
    pcontrol(g, (Rectangle){hx, 348, hw, 26}, P_HARM_SCALE);
    pcontrol(g, (Rectangle){hx, 380, hw, 24}, P_HARM_LEVEL);
    pcontrol(g, (Rectangle){hx, 410, hw, 26}, P_HARM_SRC);
    /* exemplo ao vivo: a nota da guitarra (se houver) ou a tônica */
    int src_note = atomic_load(&g->e->guitar.st_note);
    if (src_note < 0) src_note = 60 + param_geti(P_HARM_KEY);
    int hs[2], n = engine_harmony_notes(src_note, hs);
    char ex[96], a1[8], a2[8], a3[8];
    note_name(src_note, a1, sizeof a1);
    note_name(n > 0 ? hs[0] : -1, a2, sizeof a2);
    note_name(n > 1 ? hs[1] : -1, a3, sizeof a3);
    if (n > 1) snprintf(ex, sizeof ex, "Exemplo: %s -> %s + %s", a1, a2, a3);
    else snprintf(ex, sizeof ex, "Exemplo: %s -> %s", a1, a2);
    text(ex, hx, 446, param_geti(P_HARM_ON) ? C_ACCENT : C_DIM);
    text("Maior/Menor: segue a escala do tom.", hx, 470, C_DIM);
}

static void tab_midi_out(Gui *g)
{
    group((Rectangle){356, 96, 446, 412}, "Saída MIDI");
    float x = 366, w = 426;
    pcontrol_ex(g, (Rectangle){x, 114, w, 26}, P_MOUT_SRC, 70, 0);
    GuiLabel((Rectangle){x, 152, w, 20}, "Destino");
    defer_dropdown(g, (Rectangle){x, 172, w - 92, 26}, g->mout_items, &g->mout_active, &g->mout_edit, DD_MOUT);
    if (GuiButton((Rectangle){x + w - 86, 172, 86, 26}, "#211#Atualizar")) {
        midi_close();
        midi_out_close();
        build_device_lists(g);
        apply_midi(g);
        apply_midi_out(g);
    }
    char st[200];
    if (midi_out_is_open()) snprintf(st, sizeof st, "Enviando para: %.40s", midi_out_open_name());
    else snprintf(st, sizeof st, param_geti(P_MOUT_SRC) ? "Saída fechada (veja o destino)" : "Desligada");
    text(st, x, 210, midi_out_is_open() ? (Color){80, 220, 120, 255} : C_DIM);
    pcontrol(g, (Rectangle){x, 244, w, 24}, P_MOUT_CH);
    pcontrol(g, (Rectangle){x, 276, w, 24}, P_MOUT_BEND);
    if (GuiButton((Rectangle){x, 316, 200, 30}, "#131#Enviar nota de teste") && midi_out_is_open()) midi_out_test();
    /* luz de atividade */
    int act = midi_out_activity();
    if (act != g->mout_seen) { g->mout_seen = act; g->mout_blink = GetTime(); }
    bool lit = GetTime() - g->mout_blink < 0.08;
    DrawCircle((int)(x + 226), 331, 8, lit ? (Color){80, 220, 120, 255} : Fade(C_TEXT, 0.15f));
    text("atividade", x + 240, 323, C_DIM);
    static const char *what[] = {
        "Guitarra: só a nota que você toca, com",
        "bends (pitch bend). Tudo: também teclado,",
        "piano, MIDI, harmonia e arpejo.",
        "Configure no instrumento da DAW a mesma",
        "faixa de pitch bend que está aqui.",
    };
    for (int i = 0; i < 5; i++) text(what[i], x, 364 + i * 20.0f, C_DIM);

    group((Rectangle){818, 96, 446, 412}, "Como usar na DAW");
#if defined(_WIN32)
    static const char *how[] = {
        "No Windows não há porta virtual pronta:",
        "1. Instale o loopMIDI (gratuito).",
        "2. Crie uma porta (botão +).",
        "3. Clique em Atualizar e escolha a",
        "   porta como Destino.",
        "4. Na DAW (Reaper, FL Studio, Ableton...)",
        "   ative essa porta como entrada MIDI.",
        "5. Crie uma trilha com um instrumento",
        "   virtual e arme a gravação.",
        "",
        "Para ouvir só a DAW: Mistura em",
        "\"só gtr\" ou Nível synth em 0.",
    };
#elif defined(__APPLE__)
    static const char *how[] = {
        "1. Destino: Porta virtual \"Sintetizador\".",
        "2. Na DAW (Logic, GarageBand, Reaper...)",
        "   a entrada \"Sintetizador\" aparece",
        "   sozinha nas entradas MIDI.",
        "3. Crie uma trilha com um instrumento",
        "   virtual e arme a gravação.",
        "4. Toque: a guitarra controla a trilha.",
        "",
        "Para ouvir só a DAW: Mistura em",
        "\"só gtr\" ou Nível synth em 0.",
        "",
        "",
    };
#else
    static const char *how[] = {
        "1. Destino: Porta virtual \"Sintetizador\".",
        "2. Na DAW (Reaper, Ardour, LMMS, Bitwig)",
        "   ative a entrada MIDI \"Sintetizador\"",
        "   (ALSA; no JACK use a2jmidid).",
        "3. Crie uma trilha com um instrumento",
        "   virtual e arme a gravação.",
        "4. Toque: a guitarra controla a trilha.",
        "",
        "Para ouvir só a DAW: Mistura em",
        "\"só gtr\" ou Nível synth em 0.",
        "",
        "",
    };
#endif
    for (int i = 0; i < 12; i++) text(how[i], 830, 116 + i * 20.0f, C_DIM);
    text("A nota sai uns 20-30 ms depois da", 830, 368, C_DIM);
    text("palhetada (tempo de reconhecer).", 830, 388, C_DIM);
}

static void looper_main_button(Gui *g) { looper_command(&g->e->looper, LCMD_REC); }

static void tab_looper(Gui *g)
{
    Looper *lp = &g->e->looper;
    int st = atomic_load(&lp->st_state);
    int pos = atomic_load(&lp->st_pos), len = atomic_load(&lp->st_len);
    float sr = g->e->sr;

    group((Rectangle){356, 96, 446, 412}, "Looper");
    static const char *main_label[] = {"#135#Gravar", "#135#Fechar o loop", "#135#Gravar por cima", "#133#Parar overdub", "#135#Gravar por cima"};
    static const char *state_label[] = {"Vazio", "GRAVANDO", "Tocando", "GRAVANDO POR CIMA", "Parado"};
    if (GuiButton((Rectangle){366, 114, 426, 64}, main_label[st])) looper_main_button(g);
    float bw = 101;
    if (GuiButton((Rectangle){366, 188, bw, 34}, "#131#Tocar")) looper_command(lp, LCMD_PLAY);
    if (GuiButton((Rectangle){366 + bw + 7, 188, bw, 34}, "#133#Parar")) looper_command(lp, LCMD_STOP);
    if (GuiButton((Rectangle){366 + 2 * (bw + 7), 188, bw, 34}, "#56#Desfazer")) looper_command(lp, LCMD_UNDO);
    if (GuiButton((Rectangle){366 + 3 * (bw + 7), 188, bw, 34}, "#143#Limpar")) looper_command(lp, LCMD_CLEAR);

    Color sc = (st == LOOP_RECORDING || st == LOOP_OVERDUB) ? RED : (st == LOOP_PLAYING ? (Color){80, 220, 120, 255} : C_DIM);
    text_size(state_label[st], 366, 234, 32, sc);
    char t[96];
    if (st == LOOP_RECORDING) snprintf(t, sizeof t, "%.1f s (máx. 60 s)", len / sr);
    else if (st != LOOP_EMPTY) snprintf(t, sizeof t, "%.1f / %.1f s", pos / sr, len / sr);
    else t[0] = 0;
    float prog = (st == LOOP_RECORDING) ? len / (60.0f * sr) : (len > 0 ? (float)pos / len : 0);
    GuiProgressBar((Rectangle){366, 274, 426, 18}, NULL, NULL, &prog, 0, 1);
    text(t, 366, 298, C_TEXT);
    pcontrol(g, (Rectangle){366, 330, 426, 24}, P_LOOP_VOL);
    group((Rectangle){818, 236, 446, 272}, "Como usar");
    static const char *help[] = {
        "1. \"Gravar\" e toque uma base.",
        "2. \"Fechar o loop\": ela passa a",
        "   repetir sem parar.",
        "3. \"Gravar por cima\" soma camadas.",
        "",
        "Desfazer: apaga a última camada.",
        "Enter: aperta o botão grande",
        "(bom para tocar guitarra).",
        "",
        "O looper grava o som com efeitos;",
        "o metrônomo fica de fora. Máx. 60 s.",
    };
    for (int i = 0; i < 11; i++) text(help[i], 830, 256 + i * 20.0f, C_DIM);
    tempo_group(g, (Rectangle){818, 96, 446, 126});
}

/* ------------------------------------------------------------------ */
/* Barra superior, osciloscópio e status                               */
/* ------------------------------------------------------------------ */
static void draw_top_bar(Gui *g)
{
    DrawRectangle(0, 0, W, 44, C_PANEL);
    DrawLine(0, 44, W, 44, C_LINE);
    text("SINTETIZADOR", 14, 14, C_ACCENT);

    GuiLabel((Rectangle){180, 9, 60, 26}, "Preset");
    defer_dropdown(g, (Rectangle){236, 9, 220, 26}, g->preset_items, &g->preset_active, &g->preset_edit, DD_PRESET);
    if (GuiTextBox((Rectangle){464, 9, 150, 26}, g->preset_name, 40, g->name_edit)) {
        g->name_edit = !g->name_edit;
        if (g->name_edit) { /* começa com o campo vazio para digitar o nome */
            snprintf(g->name_backup, sizeof g->name_backup, "%s", g->preset_name);
            g->preset_name[0] = 0;
        } else if (!g->preset_name[0]) {
            snprintf(g->preset_name, sizeof g->preset_name, "%s", g->name_backup);
        } else {
            save_preset(g); /* Enter (ou sair do campo) salva */
        }
    }
    if (g->name_edit && !g->preset_name[0]) text("digite o nome...", 470, 14, C_DIM);
    if (GuiButton((Rectangle){618, 9, 82, 26}, "#6#Salvar")) save_preset(g);

    bool learn = g->learn;
    GuiToggle((Rectangle){712, 9, 116, 26}, "#122#MIDI Learn", &learn);
    if (learn != g->learn) {
        g->learn = learn;
        g->learn_target = -1;
        set_status(g, learn ? "MIDI learn: clique num controle e mova um knob do controlador (botão direito desfaz)"
                            : "MIDI learn desligado");
    }

    bool rec = recorder_active(&g->rec);
    char rl[48];
    if (rec) {
        double s = GetTime() - g->rec_start;
        snprintf(rl, sizeof rl, "#133#Parar %02d:%02d", (int)s / 60, (int)s % 60);
    } else snprintf(rl, sizeof rl, "#135#Gravar WAV");
    if (rec) DrawRectangle(836, 7, 132, 30, Fade(RED, 0.5f + 0.3f * sinf((float)GetTime() * 6)));
    if (GuiButton((Rectangle){838, 9, 128, 26}, rl)) toggle_recording(g);

    if (GuiButton((Rectangle){974, 9, 82, 26}, "#113#Pânico")) {
        Event off = {EV_ALL_OFF, SRC_KEYS, 0, 0, 0};
        evq_push(&g->e->q_ui, off);
        looper_command(&g->e->looper, LCMD_STOP);
        set_status(g, "Todas as notas foram silenciadas");
    }
    pcontrol_ex(g, (Rectangle){1066, 11, 150, 22}, P_MASTER, 64, 0);

    /* medidor de saída */
    float peak = atomic_load(&g->e->peak);
    g->out_level = fmaxf(peak, g->out_level * 0.92f);
    Rectangle m = {1224, 10, 46, 24};
    DrawRectangleRec(m, (Color){20, 20, 24, 255});
    float db = 20 * log10f(g->out_level + 1e-6f), t = (db + 48) / 48;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    DrawRectangle((int)m.x, (int)m.y, (int)(m.width * t), (int)m.height, g->out_level > 0.97f ? RED : (Color){80, 200, 120, 255});
    DrawRectangleLinesEx(m, 1, C_LINE);
}

static void draw_scope(Gui *g, Rectangle r)
{
    DrawRectangleRec(r, (Color){16, 18, 22, 255});
    DrawRectangleLinesEx(r, 1, C_LINE);
    DrawLine((int)r.x, (int)(r.y + r.height / 2), (int)(r.x + r.width), (int)(r.y + r.height / 2), Fade(C_LINE, 0.6f));
    static float buf[SCOPE_SIZE];
    unsigned end = atomic_load(&g->e->scope_pos);
    for (int i = 0; i < SCOPE_SIZE; i++) buf[i] = atomic_load_explicit(&g->e->scope[(end + i) & (SCOPE_SIZE - 1)], memory_order_relaxed);
    /* sincroniza na subida pelo zero para a onda ficar parada */
    int start = 0;
    for (int i = 1; i < SCOPE_SIZE / 2; i++) if (buf[i - 1] <= 0 && buf[i] > 0) { start = i; break; }
    int n = SCOPE_SIZE / 2;
    float gain = 0;
    for (int i = 0; i < n; i++) gain = fmaxf(gain, fabsf(buf[start + i]));
    gain = gain > 0.02f ? 0.45f / gain : 0.45f / 0.02f; /* ajusta a escala sozinho */
    Vector2 prev = {r.x, r.y + r.height / 2};
    for (int i = 0; i < n; i++) {
        Vector2 p = {r.x + r.width * i / (n - 1), r.y + r.height / 2 - buf[start + i] * gain * r.height};
        if (i) DrawLineEx(prev, p, 1.5f, C_ACCENT);
        prev = p;
    }
    text("osciloscópio", r.x + 8, r.y + 4, C_DIM);
}

/* ------------------------------------------------------------------ */
/* Laço principal                                                      */
/* ------------------------------------------------------------------ */
static void handle_dropdown_changes(Gui *g, int id, int before)
{
    switch (id) {
    case DD_PRESET: load_preset(g, g->preset_active); break; /* mesmo preset = recarrega */
    case DD_OUT:
    case DD_IN:     if ((id == DD_OUT ? g->out_active : g->in_active) != before) apply_audio(g); break;
    case DD_MIDI:   if (g->midi_active != before) apply_midi(g); break;
    case DD_MOUT:   if (g->mout_active != before) apply_midi_out(g); break;
    }
}

int gui_run(Engine *e, const GuiOptions *opt)
{
    static Gui gs;
    Gui *g = &gs;
    memset(g, 0, sizeof *g);
    g->e = e;
    g->learn_target = -1;
    g->mouse_note = -1;
    for (int i = 0; i < 64; i++) g->key_note[i] = -1;
    snprintf(g->preset_name, sizeof g->preset_name, "Inicial");
    if (opt) g->tab = opt->start_tab;
    if (opt && opt->start_preset > 0 && opt->start_preset < factory_preset_count()) {
        g->preset_active = opt->start_preset;
        snprintf(g->preset_name, sizeof g->preset_name, "%s", factory_preset_name(opt->start_preset));
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(opt && opt->width ? opt->width : W, opt && opt->height ? opt->height : H, "Sintetizador");
    SetWindowMinSize(640, 400);
    SetExitKey(KEY_NULL); /* Esc não fecha sem querer */
    SetTargetFPS(60);
    GuiLoadStyleDark();
    g_font = GuiGetFont();
    g_text = GuiGetStyle(DEFAULT, TEXT_SIZE);
    GuiSetStyle(DEFAULT, TEXT_SPACING, 1);
    C_BG     = GetColor((unsigned)GuiGetStyle(DEFAULT, BACKGROUND_COLOR));
    C_LINE   = GetColor((unsigned)GuiGetStyle(DEFAULT, LINE_COLOR));
    C_TEXT   = GetColor((unsigned)GuiGetStyle(DEFAULT, TEXT_COLOR_NORMAL));
    C_DIM    = Fade(C_TEXT, 0.6f);
    C_ACCENT = (Color){240, 150, 60, 255}; /* laranja de destaque */
    C_PANEL  = GetColor((unsigned)GuiGetStyle(DEFAULT, BASE_COLOR_NORMAL));

    RenderTexture2D canvas = LoadRenderTexture(W, H);
    SetTextureFilter(canvas.texture, TEXTURE_FILTER_BILINEAR);

    build_device_lists(g);
    build_preset_list(g);
    g->last_mode = param_geti(P_INPUT_MODE);
    apply_audio(g);
    apply_midi(g);
    if (!g->status[0]) set_status(g, "Pronto! Toque pelo teclado do computador, pelo piano ou escolha outra entrada.");

    int frame = 0;
    while (!WindowShouldClose()) {
        /* escala a tela virtual para o tamanho da janela */
        float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
        float scale = fminf(sw / W, sh / H);
        float ox = (sw - W * scale) / 2, oy = (sh - H * scale) / 2;
        SetMouseOffset((int)-ox, (int)-oy);
        SetMouseScale(1 / scale, 1 / scale);

        /* troca de modo de entrada: reabre o áudio e o MIDI */
        int mode = param_geti(P_INPUT_MODE);
        if (mode != g->last_mode) {
            g->last_mode = mode;
            apply_audio(g);
            apply_midi(g);
            static const char *names[] = {"Entrada: teclado do computador / piano",
                                          "Entrada: MIDI", "Entrada: guitarra / microfone"};
            if (!g->audio_error[0]) set_status(g, names[mode]);
        }
        /* saída MIDI ligada/desligada */
        int msrc = param_geti(P_MOUT_SRC);
        if ((msrc != 0) != (g->mout_last_src != 0)) apply_midi_out(g);
        g->mout_last_src = msrc;

        /* MIDI learn: chegou um CC novo? */
        if (g->learn && g->learn_target >= 0) {
            int cnt, cc = midi_last_cc(&cnt);
            if (cnt != g->learn_seen && cc >= 0) {
                midi_map(cc, g->learn_target);
                midi_map_save("midi-learn.txt");
                char m[160];
                snprintf(m, sizeof m, "MIDI learn: CC%d -> %s", cc, param_def(g->learn_target)->label);
                set_status(g, m);
                g->learn_target = -1;
            }
        }
        if (recorder_active(&g->rec)) drain_recording(g);
        handle_keyboard(g);

        BeginTextureMode(canvas);
        ClearBackground(C_BG);
        g->ndd = 0;
        bool dd_open = any_dropdown_open(g);
        if (dd_open) GuiLock();

        draw_top_bar(g);
        draw_input_panel(g);
        GuiToggleGroup((Rectangle){346, 50, 152, 30}, "#124#Som;#123#Efeitos;#174#Modulação;#139#Arp/Harmonia;#77#Looper;#122#Saída MIDI", &g->tab);
        GuiPanel((Rectangle){346, 84, 926, 436}, NULL);
        switch (g->tab) {
        case 0: tab_sound(g); break;
        case 1: tab_effects(g); break;
        case 2: tab_modulation(g); break;
        case 3: tab_arp(g); break;
        case 4: tab_looper(g); break;
        case 5: tab_midi_out(g); break;
        }
        draw_scope(g, (Rectangle){8, 528, 1264, 66});
        draw_piano(g, (Rectangle){8, 610, 1264, 160});

        const char *st = g->status;
        char def[400];
        if (GetTime() - g->status_time > 8 || !st[0]) {
            snprintf(def, sizeof def, "Entrada: %s   |   Saída: %.50s   |   %s",
                     mode == 0 ? "teclado/piano" : (mode == 1 ? (midi_is_open() ? midi_open_name() : "MIDI (nenhum)") : g->e->cap_name),
                     g->e->dev_ok ? g->e->dev_name : "desligada", g->learn ? "MIDI LEARN ATIVO" : "F1: tela cheia");
            st = def;
        }
        GuiStatusBar((Rectangle){0, H - 26, W, 26}, st);

        /* listas suspensas por último, por cima de tudo */
        GuiUnlock();
        for (int i = g->ndd - 1; i >= 0; i--) {
            Dropdown *d = &g->dd[i];
            if (dd_open && !*d->edit) GuiLock();
            int before = *d->active;
            if (GuiDropdownBox(d->r, d->items, d->active, *d->edit)) {
                *d->edit = !*d->edit;
                if (!*d->edit) handle_dropdown_changes(g, d->id, before);
            }
            GuiUnlock();
        }
        EndTextureMode();

        BeginDrawing();
        ClearBackground(BLACK);
        DrawTexturePro(canvas.texture, (Rectangle){0, 0, W, -H}, (Rectangle){ox, oy, W * scale, H * scale}, (Vector2){0, 0}, 0, WHITE);
        EndDrawing();

        if (IsKeyPressed(KEY_F1)) ToggleFullscreen();
        frame++;
        if (opt && opt->screenshot && frame == opt->screenshot_frame) {
            Image img = LoadImageFromTexture(canvas.texture);
            ImageFlipVertical(&img);
            ExportImage(img, opt->screenshot);
            UnloadImage(img);
        }
        if (opt && opt->exit_frame && frame >= opt->exit_frame) break;
    }

    if (recorder_active(&g->rec)) toggle_recording(g);
    UnloadRenderTexture(canvas);
    CloseWindow();
    return 0;
}
