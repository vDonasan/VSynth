#ifndef AUDIO_ASIO_H
#define AUDIO_ASIO_H

/*
 * Áudio pelo ASIO (só no Windows, compilado com -DSYNTH_ASIO=ON).
 * Usa o PortAudio com o SDK ASIO da Steinberg (licença GPLv3).
 * O driver ASIO cuida da entrada e da saída ao mesmo tempo.
 */
typedef void (*AsioProcess)(void *user, const float *in, int in_channels, float *out, int frames);

int         asio_driver_count(void);           /* inicia o PortAudio na primeira chamada */
const char *asio_driver_name(int i);
/* Abre o driver `i` em `sr` Hz com `frames` por bloco (0 = o que o driver preferir).
   Devolve 0 se abriu; o tamanho real do bloco vai em *frames_out. */
int  asio_open(int i, double sr, int frames, int want_input, AsioProcess cb, void *user, int *frames_out);
void asio_close(void);
int  asio_input_channels(void);                 /* entradas abertas (0 se nenhuma) */
const char *asio_error(void);                   /* última mensagem de erro */
void asio_control_panel(void);                  /* abre o painel do driver aberto */
void asio_shutdown(void);

#endif
