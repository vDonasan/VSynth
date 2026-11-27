/*
 * Janela do plugin dentro da DAW: uma janela filha da que o host dá
 * (HWND no Windows, NSView no macOS, Window no X11), que mostra a imagem
 * desenhada pelo rlsoft e passa mouse e teclado para ele.
 *
 * Uma implementação por sistema: plugin_gui_win32.c, plugin_gui_cocoa.m,
 * plugin_gui_x11.c. Tudo na thread principal (a da interface do host).
 */
#ifndef PLUGIN_GUI_H
#define PLUGIN_GUI_H

#include <stdbool.h>
#include <stdint.h>

#include "raylib.h" /* rlsoft */

typedef struct PlugWin PlugWin;

/* desenha um quadro (com o RlContext da janela já ligado) */
typedef void (*PlugWinFrame)(void *user);

/* API de janela do CLAP que este sistema usa: "win32", "cocoa" ou "x11" */
const char *plugwin_api(void);
/* As medidas que o host usa: pixels no Windows e no X11, pontos no macOS. */
PlugWin *plugwin_create(uintptr_t parent, int w, int h, double scale, RlContext *ctx, PlugWinFrame frame, void *user);
void     plugwin_set_size(PlugWin *w, int width, int height, double scale);
void     plugwin_show(PlugWin *w, bool show);
/* um quadro (pelo timer do host); com own_timer a janela tem o seu */
void     plugwin_tick(PlugWin *w);
void     plugwin_own_timer(PlugWin *w, bool on);
/* pega o teclado só enquanto se digita num campo; fora disso devolve à
   DAW (atalhos como espaço para tocar continuam funcionando) */
void     plugwin_keyboard(PlugWin *w, bool want);
void     plugwin_destroy(PlugWin *w);

#endif
