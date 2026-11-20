/*
 * Sintetizador — tocável por teclado do computador, piano na tela, MIDI
 * ou guitarra/microfone (detecção de altura), com efeitos, arpejador,
 * looper e gravação em WAV.
 *
 * Opções (úteis para testes):
 *   --tab N                abre na aba N (0 = Som ... 5 = Saída MIDI)
 *   --mode N               entrada inicial (0 teclado, 1 MIDI, 2 guitarra)
 *   --preset N             aplica o preset de fábrica N
 *   --screenshot ARQ.png   salva uma captura no quadro 30 e fecha
 *   --null-audio           sem placa de som (áudio descartado, tempo real)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "raylib.h"
#include "engine.h"
#include "gui.h"
#include "midi.h"
#include "params.h"

int main(int argc, char **argv)
{
    GuiOptions opt = {0};
    int null_audio = 0;
    params_init();
    for (int i = 1; i < argc; i++) {
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(argv[i], "--tab") && next) { opt.start_tab = atoi(next); i++; }
        else if (!strcmp(argv[i], "--mode") && next) { param_set(P_INPUT_MODE, (float)atof(next)); i++; }
        else if (!strcmp(argv[i], "--preset") && next) {
            opt.start_preset = atoi(next);
            if (opt.start_preset >= 0 && opt.start_preset < factory_preset_count()) factory_preset_apply(opt.start_preset);
            i++;
        }
        else if (!strcmp(argv[i], "--screenshot") && next) {
            opt.screenshot = next; opt.screenshot_frame = 30; opt.exit_frame = 31; i++;
        } else if (!strcmp(argv[i], "--null-audio")) {
            null_audio = 1;
        } else {
            printf("Uso: %s [--tab N] [--mode N] [--preset N] [--screenshot arquivo.png] [--null-audio]\n", argv[0]);
            return 1;
        }
    }

    SetTraceLogLevel(LOG_ERROR); /* esconde as mensagens informativas do raylib */
    static Engine engine;
    if (engine_init(&engine, SAMPLE_RATE) < 0) {
        fprintf(stderr, "Sem memória para o motor de áudio\n");
        return 1;
    }
    engine.null_backend = null_audio;
    midi_map_load("midi-learn.txt"); /* se não existir, fica o mapa padrão */

    gui_run(&engine, &opt);

    midi_close();
    midi_out_close();
    engine_free(&engine);
    return 0;
}
