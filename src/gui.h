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

#endif
