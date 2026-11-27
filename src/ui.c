#include "ui.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "midi.h"
#include "params.h"

/* fontes embutidas (geradas pelo CMake a partir de assets/fonts) */
extern const unsigned char font_inter_regular[], font_inter_semibold[], font_inter_bold[];
extern const unsigned int font_inter_regular_size, font_inter_semibold_size, font_inter_bold_size;

#define MAX_FONTS 64

typedef struct { int weight, px; Font font; } CachedFont;

struct UiState {
    RenderTexture2D canvas;
    float  rs;                 /* resolução interna: 1x, 2x ou 3x a tela virtual (no plugin, a escala exata) */
    bool   exact;              /* plugin: desenha direto na escala da janela (texto nítido a 125%, 150%...) */
    float  scale, ox, oy;
    CachedFont fonts[MAX_FONTS];
    int    nfonts;
    int    codepoints[400];
    int    ncodepoints;

    Vector2 mouse;
    bool   down, pressed, released;
    int    active;             /* controle sendo arrastado (0 = nenhum) */
    float  drag_y, drag_v;
    double last_click;
    int    last_click_id;
    bool   want_hand;

    /* lista suspensa aberta */
    int    dd_open, dd_changed;
    Rectangle dd_rect;
    const char *dd_items;
    int   *dd_active;
    bool   dd_just_opened;

    /* campo de texto com foco */
    int    focus;
    char   focus_buf[256];

    /* dica */
    const char *tip, *tip_prev;
    Rectangle tip_rect;
    double tip_since;

    bool   learn;
    int    learn_target;

    bool   modal, in_modal;
    Rectangle modal_rect;
};

/* o app usa um só; cada janela do plugin tem o seu (ui_bind) */
static UiState U_default;
static UiState *U_cur = &U_default;
#define U (*U_cur)

UiState *ui_state_create(void) { return calloc(1, sizeof(UiState)); }
void     ui_state_destroy(UiState *s) { free(s); }
UiState *ui_bind(UiState *s)
{
    UiState *o = U_cur;
    U_cur = s ? s : &U_default;
    return o;
}

/* ------------------------------------------------------------------ */
/* Fontes                                                              */
/* ------------------------------------------------------------------ */
static void build_codepoints(void)
{
    int n = 0;
    for (int c = 32; c < 127; c++) U.codepoints[n++] = c;
    for (int c = 160; c < 256; c++) U.codepoints[n++] = c;
    static const int extra[] = {0x2212, 0x2192, 0x2190, 0x2191, 0x2193, 0x2026, 0x2248, 0x201C, 0x201D,
                                0x2018, 0x2019, 0x2022, 0x2014, 0x2013, 0x266F, 0x00A2};
    for (unsigned i = 0; i < sizeof extra / sizeof extra[0]; i++) U.codepoints[n++] = extra[i];
    U.ncodepoints = n;
}

static Font font_for(UiWeight w, float size)
{
    int px = (int)lroundf(size * U.rs);
    for (int i = 0; i < U.nfonts; i++)
        if (U.fonts[i].weight == (int)w && U.fonts[i].px == px) return U.fonts[i].font;
    const unsigned char *data = w == UI_BOLD ? font_inter_bold : (w == UI_SEMIBOLD ? font_inter_semibold : font_inter_regular);
    int len = (int)(w == UI_BOLD ? font_inter_bold_size : (w == UI_SEMIBOLD ? font_inter_semibold_size : font_inter_regular_size));
    Font f = LoadFontFromMemory(".ttf", data, len, px, U.codepoints, U.ncodepoints);
    SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
    if (U.nfonts < MAX_FONTS) U.fonts[U.nfonts++] = (CachedFont){(int)w, px, f};
    return f;
}

static void unload_fonts(void)
{
    for (int i = 0; i < U.nfonts; i++) UnloadFont(U.fonts[i].font);
    U.nfonts = 0;
}

void ui_text(const char *s, float x, float y, float size, UiWeight w, Color c)
{
    Font f = font_for(w, size);
    DrawTextEx(f, s, (Vector2){roundf(x * U.rs) / U.rs, roundf(y * U.rs) / U.rs}, size, 0, c);
}

float ui_text_width(const char *s, float size, UiWeight w)
{
    return MeasureTextEx(font_for(w, size), s, size, 0).x;
}

void ui_text_center(const char *s, Rectangle r, float size, UiWeight w, Color c)
{
    float tw = ui_text_width(s, size, w);
    ui_text(s, r.x + (r.width - tw) / 2, r.y + (r.height - size * 1.2f) / 2, size, w, c);
}

void ui_text_right(const char *s, float right, float y, float size, UiWeight w, Color c)
{
    ui_text(s, right - ui_text_width(s, size, w), y, size, w, c);
}

void ui_caption(const char *s, float x, float y, Color c)
{
    Font f = font_for(UI_BOLD, 11);
    DrawTextEx(f, s, (Vector2){roundf(x * U.rs) / U.rs, roundf(y * U.rs) / U.rs}, 11, 1.1f, c);
}

/* ------------------------------------------------------------------ */
/* Ciclo de vida                                                       */
/* ------------------------------------------------------------------ */
void ui_set_exact_scale(bool on) { U.exact = on; }

void ui_init(void)
{
    memset(&U, 0, sizeof U);
    U.learn_target = -1;
    build_codepoints();
}

void ui_shutdown(void)
{
    unload_fonts();
    if (U.rs) UnloadRenderTexture(U.canvas);
}

void ui_begin_frame(Color clear)
{
    float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
    U.scale = fminf(sw / UI_W, sh / UI_H);
    /* pixels de verdade por ponto da janela (2 na tela Retina, 1 no resto) */
    float px = GetScreenWidth() > 0 ? (float)GetRenderWidth() / GetScreenWidth() : 1.0f;
    float rs = ceilf(U.scale * px - 0.05f);
    if (U.exact) rs = roundf(U.scale * px * 100.0f) / 100.0f; /* sem cópia ampliada: cada ponto no pixel certo */
    if (rs < (U.exact ? 0.25f : 1.0f)) rs = U.exact ? 0.25f : 1.0f;
    if (rs > 3) rs = 3;
    if (rs != U.rs) { /* mudou a escala: refaz a tela interna e as fontes */
        if (U.rs) UnloadRenderTexture(U.canvas);
        unload_fonts();
        U.canvas = LoadRenderTexture((int)lroundf(UI_W * rs), (int)lroundf(UI_H * rs));
        SetTextureFilter(U.canvas.texture, TEXTURE_FILTER_BILINEAR);
        U.rs = rs;
    }
    U.ox = (sw - UI_W * U.scale) / 2;
    U.oy = (sh - UI_H * U.scale) / 2;
    SetMouseOffset((int)-U.ox, (int)-U.oy);
    SetMouseScale(1 / U.scale, 1 / U.scale);

    U.mouse    = GetMousePosition();
    U.down     = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    U.pressed  = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    U.released = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
    if (!U.down && !U.released) U.active = 0;
    U.want_hand = false;
    U.tip = NULL;

    BeginTextureMode(U.canvas);
    ClearBackground(clear);
    BeginMode2D((Camera2D){.zoom = U.rs});
}

static void draw_dropdown_overlay(void);
static void draw_tooltip(void);

void ui_end_frame(void)
{
    draw_dropdown_overlay();
    draw_tooltip();
    EndMode2D();
    EndTextureMode();

    BeginDrawing();
    ClearBackground(BLACK);
    DrawTexturePro(U.canvas.texture, (Rectangle){0, 0, (float)U.canvas.texture.width, -(float)U.canvas.texture.height},
                   (Rectangle){U.ox, U.oy, UI_W * U.scale, UI_H * U.scale}, (Vector2){0, 0}, 0, WHITE);
    EndDrawing();
    SetMouseCursor(U.want_hand || U.active ? MOUSE_CURSOR_POINTING_HAND : MOUSE_CURSOR_DEFAULT);
    if (U.released) U.active = 0;
}

void ui_screenshot(const char *path)
{
    Image img = LoadImageFromTexture(U.canvas.texture);
    ImageFlipVertical(&img);
    ExportImage(img, path);
    UnloadImage(img);
}

/* ------------------------------------------------------------------ */
/* Formas                                                              */
/* ------------------------------------------------------------------ */
static float roundness(Rectangle r, float radius)
{
    float m = fminf(r.width, r.height);
    return m > 0 ? fminf(1.0f, 2.0f * radius / m) : 0;
}

void ui_rect(Rectangle r, float radius, Color fill)
{
    if (radius <= 0) DrawRectangleRec(r, fill);
    else DrawRectangleRounded(r, roundness(r, radius), 10, fill);
}

void ui_rect_lines(Rectangle r, float radius, float thick, Color c)
{
    DrawRectangleRoundedLinesEx(r, roundness(r, radius), 10, thick, c);
}

void ui_panel(Rectangle r, float radius, Color fill, Color border)
{
    ui_rect(r, radius, fill);
    ui_rect_lines(r, radius, 1, border);
}

void ui_shadow(Rectangle r, float radius)
{
    for (int i = 3; i >= 1; i--) {
        Rectangle s = {r.x - i, r.y + 2 * i, r.width + 2 * i, r.height + i};
        ui_rect(s, radius + i, (Color){0, 0, 0, (unsigned char)(28 / i)});
    }
}

void ui_card(Rectangle r, const char *title, Color dot)
{
    ui_panel(r, 14, C_PANEL, C_BORDER);
    DrawCircleV((Vector2){r.x + 17, r.y + 19}, 3, dot);
    ui_caption(title, r.x + 27, r.y + 12, C_TEXT);
}

void ui_led(float cx, float cy, float radius, bool on, Color c)
{
    if (on) {
        DrawCircleV((Vector2){cx, cy}, radius * 2.2f, Fade(c, 0.12f));
        DrawCircleV((Vector2){cx, cy}, radius * 1.5f, Fade(c, 0.22f));
        DrawCircleV((Vector2){cx, cy}, radius, c);
    } else {
        DrawCircleV((Vector2){cx, cy}, radius, (Color){42, 48, 58, 255});
    }
}

static void line(float x1, float y1, float x2, float y2, float t, Color c)
{
    DrawLineEx((Vector2){x1, y1}, (Vector2){x2, y2}, t, c);
    DrawCircleV((Vector2){x1, y1}, t / 2, c);
    DrawCircleV((Vector2){x2, y2}, t / 2, c);
}

/* Ícones simples desenhados com linhas (caixa size x size) */
void ui_icon(const char *n, float x, float y, float s, Color c)
{
    float t = fmaxf(1.4f, s * 0.11f), k = s / 14.0f;
#define L(a, b, cc, d) line(x + (a) * k, y + (b) * k, x + (cc) * k, y + (d) * k, t, c)
    if (!strcmp(n, "chev-down"))       { L(3.5f, 5, 7, 8.5f); L(7, 8.5f, 10.5f, 5); }
    else if (!strcmp(n, "chev-up"))    { L(3.5f, 8.5f, 7, 5); L(7, 5, 10.5f, 8.5f); }
    else if (!strcmp(n, "chev-left"))  { L(9, 2.5f, 4.5f, 7); L(4.5f, 7, 9, 11.5f); }
    else if (!strcmp(n, "chev-right")) { L(5, 2.5f, 9.5f, 7); L(9.5f, 7, 5, 11.5f); }
    else if (!strcmp(n, "x"))          { L(3.5f, 3.5f, 10.5f, 10.5f); L(10.5f, 3.5f, 3.5f, 10.5f); }
    else if (!strcmp(n, "plus"))       { L(7, 2.5f, 7, 11.5f); L(2.5f, 7, 11.5f, 7); }
    else if (!strcmp(n, "minus"))      { L(2.5f, 7, 11.5f, 7); }
    else if (!strcmp(n, "arrow"))      { L(1.5f, 7, 12, 7); L(8.5f, 3.5f, 12, 7); L(12, 7, 8.5f, 10.5f); }
    else if (!strcmp(n, "save"))       { L(2.5f, 2.5f, 9.5f, 2.5f); L(9.5f, 2.5f, 11.5f, 4.5f); L(11.5f, 4.5f, 11.5f, 11.5f);
                                         L(11.5f, 11.5f, 2.5f, 11.5f); L(2.5f, 11.5f, 2.5f, 2.5f); L(4.5f, 2.5f, 4.5f, 5.5f); L(4.5f, 5.5f, 8.5f, 5.5f); L(8.5f, 5.5f, 8.5f, 2.5f); }
    else if (!strcmp(n, "play"))       { DrawTriangle((Vector2){x + 4 * k, y + 2.5f * k}, (Vector2){x + 4 * k, y + 11.5f * k}, (Vector2){x + 11.5f * k, y + 7 * k}, c); }
    else if (!strcmp(n, "stop"))       { ui_rect((Rectangle){x + 3 * k, y + 3 * k, 8 * k, 8 * k}, 1.5f * k, c); }
    else if (!strcmp(n, "rec"))        { DrawCircleV((Vector2){x + 7 * k, y + 7 * k}, 4.5f * k, c); }
    else if (!strcmp(n, "undo"))       { L(4, 5, 9, 5); L(4, 5, 6.5f, 2.5f); L(4, 5, 6.5f, 7.5f);
                                         DrawRing((Vector2){x + 9 * k, y + 8 * k}, 3 * k - t / 2, 3 * k + t / 2, -90, 90, 16, c); L(9, 11, 5, 11); }
    else if (!strcmp(n, "trash"))      { L(2.5f, 4, 11.5f, 4); L(5.5f, 4, 5.5f, 2.5f); L(5.5f, 2.5f, 8.5f, 2.5f); L(8.5f, 2.5f, 8.5f, 4);
                                         L(3.5f, 4, 4.5f, 11.5f); L(4.5f, 11.5f, 9.5f, 11.5f); L(9.5f, 11.5f, 10.5f, 4); }
    else if (!strcmp(n, "refresh"))    { DrawRing((Vector2){x + 7 * k, y + 7 * k}, 4.5f * k - t / 2, 4.5f * k + t / 2, -60, 230, 24, c);
                                         L(11, 2.5f, 11.2f, 5.6f); L(11.2f, 5.6f, 8.2f, 5.4f); }
    else if (!strcmp(n, "gear"))       { DrawRing((Vector2){x + 7 * k, y + 7 * k}, 2 * k, 3.6f * k, 0, 360, 24, c);
                                         for (int i = 0; i < 6; i++) { float a = i * PI / 3; L(7 + 3.6f * cosf(a), 7 + 3.6f * sinf(a), 7 + 5.6f * cosf(a), 7 + 5.6f * sinf(a)); } }
    else if (!strcmp(n, "drag"))       { for (int i = 0; i < 3; i++) for (int j = 0; j < 2; j++) DrawCircleV((Vector2){x + (5 + 4 * j) * k, y + (3 + 4 * i) * k}, 1.3f * k, c); }
    else if (!strcmp(n, "midi"))       { DrawRing((Vector2){x + 7 * k, y + 7 * k}, 5.4f * k, 5.4f * k + t, 0, 360, 32, c);
                                         DrawCircleV((Vector2){x + 4.5f * k, y + 6.5f * k}, 1 * k, c); DrawCircleV((Vector2){x + 7 * k, y + 5 * k}, 1 * k, c);
                                         DrawCircleV((Vector2){x + 9.5f * k, y + 6.5f * k}, 1 * k, c); }
    else if (!strcmp(n, "tap"))        { DrawRing((Vector2){x + 7 * k, y + 7 * k}, 2.5f * k, 2.5f * k + t, 0, 360, 24, c);
                                         DrawRing((Vector2){x + 7 * k, y + 7 * k}, 5 * k, 5 * k + t * 0.8f, 200, 340, 16, c); }
    else if (!strcmp(n, "sine"))   { for (int i = 0; i < 12; i++) { float a0 = i / 12.0f, a1 = (i + 1) / 12.0f;
                                         L(1 + a0 * 12, 7 - 4.5f * sinf(a0 * 2 * PI), 1 + a1 * 12, 7 - 4.5f * sinf(a1 * 2 * PI)); } }
    else if (!strcmp(n, "saw"))    { L(1, 11, 7, 3); L(7, 3, 7, 11); L(7, 11, 13, 3); L(13, 3, 13, 11); }
    else if (!strcmp(n, "square")) { L(1, 11, 1, 3); L(1, 3, 4, 3); L(4, 3, 4, 11); L(4, 11, 7, 11); L(7, 11, 7, 3); L(7, 3, 10, 3); L(10, 3, 10, 11); L(10, 11, 13, 11); }
    else if (!strcmp(n, "tri"))    { L(1, 11, 4, 3); L(4, 3, 7, 11); L(7, 11, 10, 3); L(10, 3, 13, 11); }
#undef L
}

/* ------------------------------------------------------------------ */
/* Interação                                                           */
/* ------------------------------------------------------------------ */
Vector2 ui_mouse(void) { return U.mouse; }
bool ui_blocked(void) { return U.dd_open != 0; }
bool ui_typing(void) { return U.focus != 0; }

bool ui_hover(Rectangle r)
{
    if (U.dd_open || (U.active && !U.down)) return false;
    if (U.modal && (!U.in_modal || !CheckCollisionPointRec(U.mouse, U.modal_rect))) return false;
    return CheckCollisionPointRec(U.mouse, r);
}

void ui_set_modal(bool on, Rectangle r) { U.modal = on; U.modal_rect = r; U.in_modal = false; }
void ui_modal_content(void) { U.in_modal = true; }

bool ui_pressed_outside(Rectangle r)
{
    return U.pressed && !U.dd_open && !CheckCollisionPointRec(U.mouse, r);
}

bool ui_pressed(Rectangle r) { return U.pressed && !U.active && ui_hover(r); }
bool ui_clicked(Rectangle r) { return U.released && !U.dd_just_opened && ui_hover(r) && U.active <= 0; }

void ui_tooltip(Rectangle r, const char *text)
{
    if (!text || U.active || !ui_hover(r)) return;
    U.tip = text;
    U.tip_rect = r;
}

static void draw_tooltip(void)
{
    if (U.tip != U.tip_prev) { U.tip_prev = U.tip; U.tip_since = GetTime(); }
    if (!U.tip || GetTime() - U.tip_since < 0.6) return;
    /* quebra o texto em linhas de até ~280 px */
    char lines[6][160];
    int nl = 0;
    const char *p = U.tip;
    while (*p && nl < 6) {
        int last_space = -1, i = 0;
        char buf[160];
        while (p[i] && i < 159) {
            buf[i] = p[i];
            buf[i + 1] = 0;
            if (p[i] == ' ') last_space = i;
            if (ui_text_width(buf, 12, UI_REGULAR) > 280 && last_space > 0) { i = last_space; break; }
            i++;
        }
        snprintf(lines[nl], sizeof lines[nl], "%.*s", i, p);
        nl++;
        p += i;
        while (*p == ' ') p++;
    }
    float w = 0;
    for (int i = 0; i < nl; i++) w = fmaxf(w, ui_text_width(lines[i], 12, UI_REGULAR));
    Rectangle box = {U.mouse.x + 14, U.mouse.y + 18, w + 20, nl * 17.0f + 14};
    if (box.x + box.width > UI_W - 6) box.x = U.mouse.x - box.width - 8;
    if (box.y + box.height > UI_H - 6) box.y = U.mouse.y - box.height - 8;
    ui_shadow(box, 8);
    ui_panel(box, 8, (Color){30, 35, 44, 255}, C_BORDER2);
    for (int i = 0; i < nl; i++) ui_text(lines[i], box.x + 10, box.y + 7 + i * 17.0f, 12, UI_REGULAR, C_TEXT);
}

/* ------------------------------------------------------------------ */
/* Controles                                                           */
/* ------------------------------------------------------------------ */
static void button_colors(UiButtonStyle st, bool hov, Color *bg, Color *bd, Color *fg)
{
    switch (st) {
    case UI_BTN_AMBER:  *bg = C_AMBER_BG; *bd = C_AMBER_BD; *fg = C_AMBER_HI; break;
    case UI_BTN_CYAN:   *bg = C_CYAN_BG; *bd = C_CYAN_BD; *fg = C_CYAN_HI; break;
    case UI_BTN_DANGER: *bg = (Color){42, 21, 23, 255}; *bd = (Color){107, 42, 47, 255}; *fg = (Color){255, 213, 216, 255}; break;
    case UI_BTN_GHOST:  *bg = hov ? C_CTRL : (Color){0, 0, 0, 0}; *bd = (Color){0, 0, 0, 0}; *fg = hov ? C_TEXT : C_TEXT3; return;
    default:            *bg = hov ? C_CTRL_HOV : C_CTRL; *bd = C_BORDER2; *fg = C_TEXT; return;
    }
    if (hov) *bg = ColorBrightness(*bg, 0.08f);
}

/* Desenha "@icone Texto" centralizado */
static void icon_label(const char *text, Rectangle r, float size, UiWeight w, Color fg)
{
    char icon[24] = "";
    const char *label = text;
    if (text[0] == '@') {
        const char *sp = strchr(text, ' ');
        int n = sp ? (int)(sp - text - 1) : (int)strlen(text) - 1;
        snprintf(icon, sizeof icon, "%.*s", n, text + 1);
        label = sp ? sp + 1 : "";
    }
    float iw = icon[0] ? 14 : 0, gap = icon[0] && label[0] ? 7 : 0;
    float tw = label[0] ? ui_text_width(label, size, w) : 0;
    float x = r.x + (r.width - iw - gap - tw) / 2;
    if (icon[0]) ui_icon(icon, x, r.y + (r.height - 14) / 2, 14, fg);
    if (label[0]) ui_text(label, x + iw + gap, r.y + (r.height - size * 1.2f) / 2, size, w, fg);
}

bool ui_button(Rectangle r, const char *text, UiButtonStyle style)
{
    bool hov = ui_hover(r);
    if (hov) U.want_hand = true;
    Color bg, bd, fg;
    button_colors(style, hov, &bg, &bd, &fg);
    if (hov && U.down) bg = ColorBrightness(bg, -0.08f);
    ui_rect(r, 10, bg);
    if (bd.a) ui_rect_lines(r, 10, 1, bd);
    icon_label(text, r, 13, style == UI_BTN_NORMAL || style == UI_BTN_GHOST ? UI_SEMIBOLD : UI_BOLD, fg);
    return ui_clicked(r);
}

bool ui_button_icon(Rectangle r, const char *icon, const char *aria)
{
    ui_tooltip(r, aria);
    (void)aria;
    bool hov = ui_hover(r);
    if (hov) { U.want_hand = true; ui_rect(r, 8, C_CTRL_HOV); }
    ui_icon(icon, r.x + (r.width - 14) / 2, r.y + (r.height - 14) / 2, 14, hov ? C_TEXT : C_TEXT2);
    return ui_clicked(r);
}

bool ui_switch(Rectangle r, bool *on, Color accent)
{
    bool hov = ui_hover(r);
    if (hov) U.want_hand = true;
    bool changed = false;
    if (ui_clicked(r)) { *on = !*on; changed = true; }
    Rectangle t = {r.x, r.y + (r.height - 24) / 2, 44, 24};
    Color track = *on ? ColorBrightness(accent, -0.42f) : (Color){42, 48, 58, 255};
    ui_rect(t, 12, track);
    float kx = *on ? t.x + t.width - 12 : t.x + 12;
    DrawCircleV((Vector2){kx, t.y + 12}, 9, *on ? ColorBrightness(accent, 0.75f) : C_TEXT2);
    return changed;
}

static int count_items(const char *items) { int n = 1; for (const char *p = items; *p; p++) n += *p == ';'; return n; }

static void item_text(const char *items, int idx, char *buf, int len)
{
    const char *p = items;
    for (int i = 0; i < idx && p; i++) { p = strchr(p, ';'); if (p) p++; }
    if (!p) { buf[0] = 0; return; }
    const char *e = strchr(p, ';');
    snprintf(buf, len, "%.*s", e ? (int)(e - p) : (int)strlen(p), p);
}

bool ui_segmented(Rectangle r, const char *items, int *active, Color accent, Color bg, Color fg)
{
    (void)accent;
    int n = count_items(items);
    bool changed = false;
    ui_panel(r, 11, C_WELL, C_BORDER);
    float w = (r.width - 6 - (n - 1) * 3) / n;
    for (int i = 0; i < n; i++) {
        Rectangle b = {r.x + 3 + i * (w + 3), r.y + 3, w, r.height - 6};
        char t[64];
        item_text(items, i, t, sizeof t);
        bool hov = ui_hover(b);
        if (hov) U.want_hand = true;
        if (i == *active) { ui_rect(b, 8, bg); ui_rect_lines(b, 8, 1, ColorBrightness(bg, 0.35f)); }
        else if (hov) ui_rect(b, 8, C_CTRL);
        icon_label(t, b, 12.5f, i == *active ? UI_SEMIBOLD : UI_REGULAR, i == *active ? fg : C_TEXT3);
        if (ui_clicked(b) && *active != i) { *active = i; changed = true; }
    }
    return changed;
}

bool ui_pills(Rectangle r, int cols, float rowh, const char *items, int *active, bool cyan)
{
    int n = count_items(items);
    bool changed = false;
    float gap = 5, w = (r.width - (cols - 1) * gap) / cols;
    for (int i = 0; i < n; i++) {
        Rectangle b = {r.x + (i % cols) * (w + gap), r.y + (i / cols) * (rowh + gap), w, rowh};
        char t[64];
        item_text(items, i, t, sizeof t);
        bool on = i == *active, hov = ui_hover(b);
        if (hov) U.want_hand = true;
        Color bg = on ? (cyan ? C_CYAN_BG : C_AMBER_BG) : (hov ? C_CTRL_HOV : C_CTRL);
        Color bd = on ? (cyan ? C_CYAN_BD : C_AMBER_BD) : C_BORDER2;
        Color fg = on ? (cyan ? C_CYAN_HI : C_AMBER_HI) : C_TEXT3;
        ui_rect(b, 8, bg);
        ui_rect_lines(b, 8, 1, bd);
        ui_text_center(t, b, 12, UI_SEMIBOLD, fg);
        if (ui_clicked(b) && !on) { *active = i; changed = true; }
    }
    return changed;
}

bool ui_dropdown(Rectangle r, const char *items, int *active, int id)
{
    bool hov = ui_hover(r);
    if (hov) U.want_hand = true;
    ui_rect(r, 9, hov ? C_CTRL_HOV : C_CTRL);
    ui_rect_lines(r, 9, 1, U.dd_open == id ? C_CYAN_BD : C_BORDER2);
    char t[256];
    item_text(items, *active, t, sizeof t);
    /* corta o texto se não couber */
    float maxw = r.width - 38;
    while (strlen(t) > 3 && ui_text_width(t, 12.5f, UI_REGULAR) > maxw) {
        size_t l = strlen(t);
        t[l - 4] = '.'; t[l - 3] = '.'; t[l - 2] = '.'; t[l - 1] = 0;
    }
    ui_text(t, r.x + 11, r.y + (r.height - 15) / 2, 12.5f, UI_REGULAR, C_TEXT);
    ui_icon("chev-down", r.x + r.width - 24, r.y + (r.height - 14) / 2, 14, C_TEXT2);
    if (ui_clicked(r)) {
        U.dd_open = id;
        U.dd_rect = r;
        U.dd_items = items;
        U.dd_active = active;
        U.dd_just_opened = true;
    }
    if (U.dd_changed == id) { U.dd_changed = 0; return true; }
    return false;
}

static void draw_dropdown_overlay(void)
{
    if (!U.dd_open) return;
    int n = count_items(U.dd_items);
    float ih = 30, w = fmaxf(U.dd_rect.width, 220);
    float h = n * ih + 8;
    Rectangle box = {U.dd_rect.x, U.dd_rect.y + U.dd_rect.height + 4, w, h};
    if (box.y + h > UI_H - 8) box.y = U.dd_rect.y - h - 4; /* abre para cima */
    if (box.x + w > UI_W - 8) box.x = UI_W - 8 - w;
    ui_shadow(box, 10);
    ui_panel(box, 10, (Color){27, 31, 39, 255}, C_BORDER2);
    int hovered = -1;
    for (int i = 0; i < n; i++) {
        Rectangle it = {box.x + 4, box.y + 4 + i * ih, w - 8, ih};
        char t[256];
        item_text(U.dd_items, i, t, sizeof t);
        bool hov = CheckCollisionPointRec(U.mouse, it);
        if (hov) { hovered = i; ui_rect(it, 7, C_CTRL_HOV); U.want_hand = true; }
        if (i == *U.dd_active) DrawCircleV((Vector2){it.x + 10, it.y + ih / 2}, 3, C_CYAN);
        ui_text(t, it.x + 22, it.y + (ih - 15) / 2, 12.5f, i == *U.dd_active ? UI_SEMIBOLD : UI_REGULAR, C_TEXT);
    }
    bool just = U.dd_just_opened;
    U.dd_just_opened = false;
    if (just) return;
    if (U.released) {
        if (hovered >= 0) {
            if (*U.dd_active != hovered) { *U.dd_active = hovered; }
            U.dd_changed = U.dd_open; /* avisa no próximo quadro (também se repetiu) */
            U.dd_open = 0;
        } else if (!CheckCollisionPointRec(U.mouse, U.dd_rect)) {
            U.dd_open = 0;
        }
    }
    if (IsKeyPressed(KEY_ESCAPE)) U.dd_open = 0;
}

void ui_focus(int id)
{
    U.focus = id;
    while (GetCharPressed() > 0) {} /* descarta o que foi digitado antes */
}

bool ui_textbox(Rectangle r, char *buf, int cap, int id, const char *placeholder)
{
    bool hov = ui_hover(r), focused = U.focus == id, enter = false;
    if (hov) U.want_hand = true;
    if (ui_clicked(r) && !focused) { U.focus = id; focused = true; }
    else if (focused && U.pressed && !CheckCollisionPointRec(U.mouse, r)) { U.focus = 0; focused = false; }
    if (focused) {
        int ch;
        while ((ch = GetCharPressed()) > 0) {
            int bytes = 0;
            const char *u = CodepointToUTF8(ch, &bytes);
            int len = (int)strlen(buf);
            if (len + bytes < cap) { memcpy(buf + len, u, bytes); buf[len + bytes] = 0; }
        }
        if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
            int len = (int)strlen(buf);
            while (len > 0 && (buf[len - 1] & 0xC0) == 0x80) len--; /* volta um caractere UTF-8 inteiro */
            if (len > 0) len--;
            buf[len] = 0;
        }
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) { U.focus = 0; enter = true; }
        if (IsKeyPressed(KEY_ESCAPE)) U.focus = 0;
    }
    ui_rect(r, 9, focused ? C_WELL : C_CTRL);
    ui_rect_lines(r, 9, 1, focused ? C_CYAN_BD : C_BORDER2);
    float ty = r.y + (r.height - 15) / 2;
    if (!buf[0] && placeholder) ui_text(placeholder, r.x + 10, ty, 12.5f, UI_REGULAR, C_TEXT2);
    else ui_text(buf, r.x + 10, ty, 12.5f, UI_REGULAR, C_TEXT);
    if (focused && fmodf((float)GetTime(), 1.0f) < 0.55f) {
        float cx = r.x + 10 + ui_text_width(buf, 12.5f, UI_REGULAR) + 1;
        DrawRectangleRec((Rectangle){cx, r.y + 7, 1.5f, r.height - 14}, C_CYAN_HI);
    }
    return enter;
}

void ui_meter(Rectangle r, int n, float level, float warn, float red)
{
    float gap = 2, w = (r.width - (n - 1) * gap) / n;
    for (int i = 0; i < n; i++) {
        float pos = (i + 0.5f) / n;
        Color c = (Color){35, 40, 49, 255};
        if (pos <= level) c = pos >= red ? C_RED : (pos >= warn ? C_YELLOW : C_GREEN);
        ui_rect((Rectangle){r.x + i * (w + gap), r.y, w, r.height}, 2, c);
    }
}

/* ------------------------------------------------------------------ */
/* Knobs                                                               */
/* ------------------------------------------------------------------ */
void ui_knob_draw(float cx, float cy, float size, float v, Color accent, bool bipolar)
{
    Vector2 c = {cx, cy};
    float r = size / 2, th = fmaxf(3.0f, size * 0.085f);
    v = v < 0 ? 0 : (v > 1 ? 1 : v);
    /* 0% fica embaixo à esquerda (135°) e 100% embaixo à direita (405°) */
    DrawRing(c, r - th, r, 135, 405, 64, C_KNOB_TRK);
    float a = 135 + 270 * v;
    if (bipolar) {
        if (a >= 270) DrawRing(c, r - th, r, 270, fmaxf(a, 271), 48, accent);
        else DrawRing(c, r - th, r, a, 270, 48, accent);
    } else if (v > 0.002f) {
        DrawRing(c, r - th, r, 135, a, 64, accent);
    }
    float cap = r * 0.64f;
    DrawCircleV(c, cap + 1, C_KNOB_EDGE);
    DrawCircleV(c, cap, C_KNOB_CAP);
    float rad = a * DEG2RAD, pw = fmaxf(2.0f, size * 0.05f);
    line(cx + cosf(rad) * cap * 0.3f, cy + sinf(rad) * cap * 0.3f, cx + cosf(rad) * cap * 0.82f, cy + sinf(rad) * cap * 0.82f, pw, C_TEXT);
}

static bool knob_core(float cx, float cy, float size, int param, Color accent, const char *label, bool bipolar)
{
    const ParamDef *d = param_def(param);
    Rectangle hit = {cx - size / 2 - 8, cy - size / 2 - 6, size + 16, size + 42};
    int id = 1000 + param;
    bool changed = false, hov = ui_hover(hit);
    if (U.learn) {
        ui_learn_zone(hit, param);
    } else {
        if (hov || U.active == id) U.want_hand = true;
        if (ui_pressed(hit)) {
            double now = GetTime();
            if (U.last_click_id == id && now - U.last_click < 0.3) { /* duplo clique: valor padrão */
                param_set(param, d->def);
                changed = true;
                U.last_click_id = 0;
            } else {
                U.active = id;
                U.drag_y = U.mouse.y;
                U.drag_v = param_get_norm(param);
                U.last_click = now;
                U.last_click_id = id;
            }
        }
        if (U.active == id && U.down) {
            bool fine = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            float n = U.drag_v + (U.drag_y - U.mouse.y) * (fine ? 0.0015f : 0.006f);
            n = n < 0 ? 0 : (n > 1 ? 1 : n);
            if (fabsf(n - param_get_norm(param)) > 1e-6f) { param_set_norm(param, n); changed = true; }
        }
        float wheel = hov ? GetMouseWheelMove() : 0;
        if (wheel != 0) {
            bool fine = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            float step = (d->kind == K_INT || d->kind == K_CHOICE) ? 1.0f / (d->max - d->min) : (fine ? 0.005f : 0.025f);
            param_set_norm(param, param_get_norm(param) + wheel * step);
            changed = true;
        }
    }
    if (!U.learn) ui_tooltip(hit, param_help(param));
    bool lit = hov || U.active == id;
    ui_knob_draw(cx, cy, size, param_get_norm(param), lit ? ColorBrightness(accent, 0.15f) : accent, bipolar);
    char val[32];
    param_format(param, val, sizeof val);
    float vy = cy + size / 2 + 5;
    if (U.active == id) { /* valor em destaque enquanto arrasta */
        float tw = ui_text_width(val, 12, UI_SEMIBOLD) + 12;
        ui_rect((Rectangle){cx - tw / 2, vy - 2, tw, 19}, 6, ColorBrightness(accent, -0.55f));
    }
    ui_text_center(val, (Rectangle){cx - 60, vy, 120, 15}, 12, UI_SEMIBOLD, C_TEXT);
    char cap[48];
    snprintf(cap, sizeof cap, "%s", label ? label : d->label);
    for (unsigned char *p = (unsigned char *)cap; *p; p++) { /* maiúsculas, inclusive acentos (UTF-8 Latin-1) */
        if (*p >= 'a' && *p <= 'z') *p -= 32;
        else if (*p == 0xC3 && p[1] >= 0xA0 && p[1] <= 0xBE && p[1] != 0xB7) { p[1] -= 0x20; p++; }
    }
    ui_text_center(cap, (Rectangle){cx - 60, vy + 17, 120, 13}, 10.5f, UI_SEMIBOLD, C_TEXT2);
    return changed;
}

bool ui_knob(float cx, float cy, float size, int param, Color accent, const char *label)
{
    return knob_core(cx, cy, size, param, accent, label, false);
}

bool ui_knob_bipolar(float cx, float cy, float size, int param, Color accent, const char *label)
{
    return knob_core(cx, cy, size, param, accent, label, true);
}

bool ui_param_switch(Rectangle r, int param, Color accent)
{
    if (U.learn) ui_learn_zone((Rectangle){r.x - 3, r.y - 3, 50, r.height + 6}, param);
    bool on = param_geti(param);
    if (!U.learn) ui_tooltip(r, param_help(param));
    if (!U.learn && ui_switch(r, &on, accent)) { param_set(param, on); return true; }
    if (U.learn) ui_switch(r, &on, accent); /* só desenha */
    return false;
}

/* ------------------------------------------------------------------ */
/* MIDI learn                                                          */
/* ------------------------------------------------------------------ */
void ui_set_learn(bool on) { U.learn = on; U.learn_target = -1; }
bool ui_learn(void) { return U.learn; }
int  ui_learn_target(void) { return U.learn_target; }
void ui_set_learn_target(int p) { U.learn_target = p; }

void ui_learn_zone(Rectangle r, int param)
{
    const ParamDef *d = param_def(param);
    if (!d || (d->flags & F_HIDDEN)) return;
    bool target = U.learn_target == param;
    ui_rect_lines(r, 8, target ? 2 : 1, target ? C_AMBER : Fade(C_AMBER, 0.35f));
    int cc = midi_param_cc(param);
    if (cc >= 0) {
        char t[16];
        snprintf(t, sizeof t, "CC%d", cc);
        float tw = ui_text_width(t, 10, UI_BOLD) + 8;
        Rectangle tag = {r.x + r.width - tw - 2, r.y - 7, tw, 15};
        ui_rect(tag, 5, C_AMBER);
        ui_text_center(t, tag, 10, UI_BOLD, (Color){42, 26, 2, 255});
    }
    if (U.pressed && ui_hover(r)) U.learn_target = param;
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && CheckCollisionPointRec(U.mouse, r) && cc >= 0) midi_map(cc, -1);
}
