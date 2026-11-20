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
 *   --exit-after N         fecha sozinho depois de N quadros (testes)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define chdir _chdir
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "config.h"
#include "raylib.h"
#include "engine.h"
#include "gui.h"
#include "midi.h"
#include "params.h"

/* Onde ficam presets, configurações, gravações e o mapa do MIDI learn.
   - App do macOS (.app): em ~/Library/Application Support/Virtual Synth,
     porque o pacote do app não pode ser alterado.
   - Windows e Linux: ao lado do executável (versão "portátil": dá para
     levar a pasta inteira para outro computador). */
static void use_data_dir(void)
{
    const char *exe = GetApplicationDirectory();
#ifdef __APPLE__
    if (strstr(exe, ".app/Contents/MacOS")) {
        const char *home = getenv("HOME");
        if (!home) return;
        char dir[1024];
        snprintf(dir, sizeof dir, "%s/Library/Application Support/Virtual Synth", home);
        mkdir(dir, 0755);
        if (chdir(dir) != 0) fprintf(stderr, "Não deu para usar a pasta %s\n", dir);
        return;
    }
#endif
    if (exe && exe[0] && chdir(exe) != 0) fprintf(stderr, "Não deu para usar a pasta %s\n", exe);
}

int main(int argc, char **argv)
{
    GuiOptions opt = {0};
    opt.start_preset = -1;
    opt.start_mode   = -1;
    int null_audio = 0;
    params_init();
    for (int i = 1; i < argc; i++) {
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(argv[i], "--tab") && next) { opt.start_tab = atoi(next); i++; }
        else if (!strcmp(argv[i], "--mode") && next) { opt.start_mode = atoi(next); i++; }
        else if (!strcmp(argv[i], "--preset") && next) {
            opt.start_preset = atoi(next);
            i++;
        }
        else if (!strcmp(argv[i], "--screenshot") && next) {
            opt.screenshot = next; opt.screenshot_frame = 30; opt.exit_frame = 31; opt.no_settings = 1; i++;
        } else if (!strcmp(argv[i], "--exit-after") && next) { /* testes: fecha sozinho, salvando as configurações */
            opt.exit_frame = atoi(next);
            i++;
        } else if (!strcmp(argv[i], "--null-audio")) {
            null_audio = 1;
        } else {
            printf("Uso: %s [--tab N] [--mode N] [--preset N] [--screenshot arquivo.png] [--null-audio]\n", argv[0]);
            return 1;
        }
    }

    SetTraceLogLevel(LOG_ERROR); /* esconde as mensagens informativas do raylib */
    if (!opt.screenshot) use_data_dir(); /* capturas: ficam na pasta atual */
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
