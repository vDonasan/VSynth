#ifndef DRIFT_H
#define DRIFT_H

/* Ponte entre dois aparelhos de áudio com relógios diferentes (ex.: a
   interface USB na entrada e a placa do PC no fone). A entrada empurra
   amostras numa fila; a saída puxa por um reamostrador cujo passo se ajusta
   devagar para manter a fila no tamanho alvo. Assim a diferença de relógio
   (dezenas de ppm) é absorvida sem estalos: nunca falta nem sobra amostra.

   Um produtor (thread da entrada) e um consumidor (thread da saída), sem
   travas. */

#include <stdatomic.h>

#define DRIFT_MAX_CH 64 /* interfaces grandes têm muitos canais */

typedef struct {
    float   *buf;          /* fila circular, quadros intercalados */
    int      ch, cap;      /* canais; capacidade em quadros (potência de 2) */
    atomic_uint wpos, rpos;/* posições em quadros (crescem sem parar) */

    /* lado da saída */
    double   frac;         /* posição fracionária entre rpos e rpos+1 */
    double   ratio;        /* quadros de entrada consumidos por quadro de saída */
    double   integ;        /* parte integral do controle */
    double   fill_avg;     /* nível médio da fila (quadros) */
    int      target;       /* nível alvo (quadros) */
    int      primed;       /* já juntou o alvo antes de começar a tocar */
    unsigned underruns, overruns;
} Drift;

int  drift_init(Drift *d, int channels, int target_frames);
void drift_free(Drift *d);
/* thread da entrada */
void drift_push(Drift *d, const float *in, int frames);
/* thread da saída: sempre entrega 'frames' quadros (silêncio se faltar) */
void drift_pull(Drift *d, float *out, int frames);
/* nível atual da fila, em quadros (para mostrar/depurar) */
int  drift_fill(const Drift *d);

#endif
