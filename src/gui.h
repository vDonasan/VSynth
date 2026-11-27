#ifndef GUI_H
#define GUI_H

#include "engine.h"

typedef struct {
    int         width, height;     /* tamanho inicial da janela (0 = 1280x800) */
    int         start_tab;         /* aba inicial */
    int         start_preset;      /* preset de fábrica a aplicar (-1 = nenhum) */
    int         start_mode;        /* entrada inicial (-1 = a das configurações) */
    int         no_settings;       /* não lê nem grava configuracoes.ini (testes) */
    const char *screenshot;        /* salva uma captura da tela (testes) */
    int         screenshot_frame;
    int         exit_frame;        /* fecha sozinho depois de N quadros (testes) */
} GuiOptions;

int gui_run(Engine *e, const GuiOptions *opt);

/* Plugin (compilado com GUI_PLUGIN, sobre o rlsoft): a mesma interface
   desenhada numa janela da DAW. Ligue o RlContext da janela (rls_bind) e os
   parâmetros da instância (params_bind) antes de cada chamada. */
typedef struct Gui Gui;
Gui *gui_plugin_create(Engine *e, const char *preset_dir);
void gui_plugin_frame(Gui *g);
void gui_plugin_destroy(Gui *g);
void gui_plugin_screenshot(Gui *g, const char *path);
/* um campo de texto está com o foco (a janela precisa do teclado) */
#include <stdbool.h>
bool gui_plugin_typing(Gui *g);

#endif
