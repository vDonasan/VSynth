/*
 * Biblioteca de interface desenhada à mão sobre o raylib.
 *
 * Estilo "equipamento de estúdio": fundo grafite, cantos arredondados,
 * knobs com arco de valor, LEDs e medidores segmentados, fonte Inter.
 *
 * É "modo imediato": a cada quadro a tela inteira é redesenhada e cada
 * função desenha o controle e já devolve se o usuário mexeu nele.
 *
 *   if (ui_button(r, "Salvar", UI_BTN_NORMAL)) salvar();
 *
 * Tudo é desenhado numa tela virtual de 1280x800, ampliada para o
 * tamanho da janela. As fontes são geradas no tamanho real dos pixels,
 * então o texto fica nítido também em telas de alta resolução.
 */
#ifndef UI_LIB_H
#define UI_LIB_H

#include <stdbool.h>

#include "raylib.h"

#define UI_W 1280
#define UI_H 800

/* ---------- cores ---------- */
#define C_BG        (Color){13, 15, 19, 255}     /* fundo */
#define C_BAR       (Color){18, 21, 26, 255}     /* barras de cima e de baixo */
#define C_PANEL     (Color){21, 24, 30, 255}     /* painéis */
#define C_WELL      (Color){15, 18, 22, 255}     /* áreas rebaixadas (telas, medidores) */
#define C_CTRL      (Color){27, 31, 39, 255}     /* botões */
#define C_CTRL_HOV  (Color){34, 39, 48, 255}
#define C_BORDER    (Color){35, 40, 49, 255}
#define C_BORDER2   (Color){42, 48, 58, 255}
#define C_TEXT      (Color){236, 238, 242, 255}
#define C_TEXT2     (Color){154, 163, 178, 255}  /* secundário (contraste > 4.5:1) */
#define C_TEXT3     (Color){180, 188, 200, 255}  /* abas inativas */
#define C_AMBER     (Color){245, 165, 36, 255}   /* som */
#define C_AMBER_HI  (Color){251, 191, 76, 255}
#define C_AMBER_BG  (Color){42, 34, 19, 255}
#define C_AMBER_BD  (Color){107, 78, 20, 255}
#define C_CYAN      (Color){56, 189, 248, 255}   /* entrada / harmonia */
#define C_CYAN_HI   (Color){125, 211, 252, 255}
#define C_CYAN_BG   (Color){29, 43, 56, 255}
#define C_CYAN_BD   (Color){43, 86, 117, 255}
#define C_GREEN     (Color){52, 211, 153, 255}
#define C_YELLOW    (Color){251, 191, 36, 255}
#define C_RED       (Color){240, 82, 82, 255}
#define C_KNOB_TRK  (Color){43, 49, 60, 255}
#define C_KNOB_CAP  (Color){35, 40, 51, 255}
#define C_KNOB_EDGE (Color){54, 61, 74, 255}

typedef enum { UI_REGULAR, UI_SEMIBOLD, UI_BOLD } UiWeight;
typedef enum { UI_BTN_NORMAL, UI_BTN_AMBER, UI_BTN_CYAN, UI_BTN_DANGER, UI_BTN_GHOST } UiButtonStyle;

/* ---------- ciclo de vida ---------- */
void  ui_init(void);
void  ui_shutdown(void);
/* Começa o quadro: calcula a escala da janela e prepara a tela virtual. */
void  ui_begin_frame(Color clear);
/* Desenha o que ficou por cima (listas abertas, dicas) e mostra na janela. */
void  ui_end_frame(void);
/* Salva a tela virtual num PNG (testes / capturas). */
void  ui_screenshot(const char *path);

/* ---------- texto ---------- */
void  ui_text(const char *s, float x, float y, float size, UiWeight w, Color c);
void  ui_text_center(const char *s, Rectangle r, float size, UiWeight w, Color c);
void  ui_text_right(const char *s, float right, float y, float size, UiWeight w, Color c);
float ui_text_width(const char *s, float size, UiWeight w);
/* Rótulo pequeno em maiúsculas com espaçamento (títulos de seção). */
void  ui_caption(const char *s, float x, float y, Color c);

/* ---------- formas ---------- */
void ui_rect(Rectangle r, float radius, Color fill);
void ui_rect_lines(Rectangle r, float radius, float thick, Color c);
void ui_panel(Rectangle r, float radius, Color fill, Color border);
void ui_shadow(Rectangle r, float radius);
/* Cartão de módulo: fundo, borda, ponto colorido e título. */
void ui_card(Rectangle r, const char *title, Color dot);
void ui_led(float cx, float cy, float radius, bool on, Color c);
void ui_icon(const char *name, float x, float y, float size, Color c); /* ícones vetoriais simples */

/* ---------- estado de interação ---------- */
Vector2 ui_mouse(void);
bool  ui_hover(Rectangle r);              /* mouse em cima e nada por cima dele */
bool  ui_clicked(Rectangle r);            /* soltou o botão em cima */
bool  ui_pressed(Rectangle r);            /* apertou o botão em cima */
bool  ui_blocked(void);                   /* uma lista/janela está aberta por cima */
bool  ui_typing(void);                    /* um campo de texto está com o foco */
void  ui_tooltip(Rectangle r, const char *text); /* dica ao parar o mouse em cima */
/* Janela modal: enquanto ligada, só a área `r` recebe o mouse.
   Chame no começo do quadro, antes de desenhar o resto. */
void  ui_set_modal(bool on, Rectangle r);
bool  ui_pressed_outside(Rectangle r);    /* clicou fora (para fechar popups) */

/* ---------- controles ---------- */
bool ui_button(Rectangle r, const char *text, UiButtonStyle style);
bool ui_button_icon(Rectangle r, const char *icon, const char *aria);
bool ui_switch(Rectangle r, bool *on, Color accent);
/* Controle segmentado: "A;B;C". Devolve true se mudou. */
bool ui_segmented(Rectangle r, const char *items, int *active, Color accent, Color bg, Color fg);
/* Grade de pílulas: `cols` colunas, altura de linha `rowh`. */
bool ui_pills(Rectangle r, int cols, float rowh, const char *items, int *active, bool cyan);
/* Lista suspensa: abre uma lista por cima de tudo. */
bool ui_dropdown(Rectangle r, const char *items, int *active, int id);
/* Campo de texto de uma linha. Devolve true ao apertar Enter. */
bool ui_textbox(Rectangle r, char *buf, int cap, int id, const char *placeholder);
void ui_focus(int id); /* dá o foco a um campo de texto (0 = nenhum) */
/* Medidor segmentado: nível 0..1, onde começa o amarelo e o vermelho. */
void ui_meter(Rectangle r, int segments, float level, float warn, float red);

/*
 * Knob ligado direto a um parâmetro da tabela (params.h):
 *  - arrastar para cima/baixo muda; Shift = ajuste fino
 *  - roda do mouse muda; duplo clique volta ao padrão
 *  - no modo MIDI learn, clicar escolhe o knob como alvo
 * `size` é o diâmetro; o rótulo e o valor ficam embaixo.
 */
bool ui_knob(float cx, float cy, float size, int param, Color accent, const char *label);
/* Igual, mas o arco sai do centro (parâmetros com valor negativo/positivo). */
bool ui_knob_bipolar(float cx, float cy, float size, int param, Color accent, const char *label);
/* Só o desenho de um knob (valor 0..1), sem interação. */
void ui_knob_draw(float cx, float cy, float size, float value, Color accent, bool bipolar);
/* Chave liga/desliga ligada a um parâmetro booleano. */
bool ui_param_switch(Rectangle r, int param, Color accent);

/* MIDI learn */
void ui_set_learn(bool on);
bool ui_learn(void);
int  ui_learn_target(void);
void ui_set_learn_target(int param);
/* Marca uma área como alvo de MIDI learn para `param` (desenha o contorno). */
void ui_learn_zone(Rectangle r, int param);

#endif
