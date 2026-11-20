/*
 * Testa a tradução para bytes MIDI da saída (canal, note on/off, pitch
 * bend com a faixa escolhida) usando um PortMidi falso. Roda sem hardware.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "midi.h"
#include "params.h"
#include "platform.h"
#include "portmidi.h"

/* ---------- PortMidi falso ---------- */
PmMessage fake_sent[4096];
int fake_nsent = 0;
static PmDeviceInfo dev = {0, "fake", "Porta de teste", 0, 1, 0, 0};
static int dummy;
PmError Pm_Initialize(void) { return pmNoError; }
PmError Pm_Terminate(void) { return pmNoError; }
int Pm_CountDevices(void) { return 1; }
PmDeviceID Pm_GetDefaultInputDeviceID(void) { return pmNoDevice; }
const PmDeviceInfo *Pm_GetDeviceInfo(PmDeviceID id) { return id == 0 ? &dev : NULL; }
PmError Pm_OpenInput(PortMidiStream **s, PmDeviceID id, void *i, int32_t b, PmTimeProcPtr t, void *ti) { (void)s; (void)id; (void)i; (void)b; (void)t; (void)ti; return pmNoDevice; }
PmError Pm_OpenOutput(PortMidiStream **s, PmDeviceID id, void *i, int32_t b, PmTimeProcPtr t, void *ti, int32_t l) { (void)id; (void)i; (void)b; (void)t; (void)ti; (void)l; *s = &dummy; return pmNoError; }
PmError Pm_SetFilter(PortMidiStream *s, int32_t f) { (void)s; (void)f; return pmNoError; }
int Pm_Read(PortMidiStream *s, PmEvent *b, int32_t l) { (void)s; (void)b; (void)l; return 0; }
PmError Pm_WriteShort(PortMidiStream *s, PmTimestamp w, PmMessage m) { (void)s; (void)w; if (fake_nsent < 4096) fake_sent[fake_nsent++] = m; return pmNoError; }
PmError Pm_Close(PortMidiStream *s) { (void)s; return pmNoError; }
PmError Pm_CreateVirtualOutput(const char *n, const char *i, void *d) { (void)n; (void)i; (void)d; return pmNoDevice; }
PmError Pm_DeleteVirtualDevice(PmDeviceID id) { (void)id; return pmNoError; }

static int failures = 0;

static void expect(int idx, int status, int d1, int d2)
{
    PmMessage m = fake_sent[idx];
    int ok = Pm_MessageStatus(m) == status && Pm_MessageData1(m) == d1 && Pm_MessageData2(m) == d2;
    printf("  %02X %3d %3d  %s\n", Pm_MessageStatus(m), Pm_MessageData1(m), Pm_MessageData2(m), ok ? "ok" : "ERRADO");
    if (!ok) { printf("     esperado %02X %d %d\n", status, d1, d2); failures++; }
}

int main(void)
{
    params_init();
    param_set(P_MOUT_CH, 3);   /* canal 3 -> status com nibble 2 */
    param_set(P_MOUT_BEND, 2); /* faixa ±2 semitons */
    static EventQueue q;
    evq_init(&q);
    midi_refresh();
    printf("Saída MIDI (PortMidi falso): %d dispositivo(s) de saída: %s\n", midi_out_count(), midi_out_name(0));
    if (midi_out_open(0, &q) != 0) { printf("não abriu\n"); return 1; }

    Event evs[] = {
        {EV_NOTE_ON, SRC_AUDIO, 52, 90, 52.5f},   /* E3 meio semitom acima */
        {EV_PITCH_TRACK, SRC_AUDIO, 52, 0, 53.0f}, /* bend de +1 semitom */
        {EV_NOTE_OFF, SRC_AUDIO, 52, 0, 0},
    };
    for (unsigned i = 0; i < 3; i++) evq_push(&q, evs[i]);
    sleep_ms(30); /* a thread de saída envia */
    midi_out_close();

    printf("  enviado (status, dado1, dado2):\n");
    /* bend +0,5 st com faixa 2 -> 8192 + 0,25*8191 = 10240 -> LSB 0, MSB 80 */
    expect(0, 0xE2, 10240 & 0x7F, 10240 >> 7);
    expect(1, 0x92, 52, 90);
    /* bend +1 st -> 8192 + 0,5*8191 = 12288 -> LSB 0, MSB 96 */
    expect(2, 0xE2, 12288 & 0x7F, 12288 >> 7);
    expect(3, 0x82, 52, 0);
    expect(4, 0xB2, 123, 0);  /* ao fechar: all notes off */
    expect(5, 0xE2, 0, 64);   /* e pitch bend no centro (8192) */
    printf(failures ? "\n%d falha(s)\n" : "\nTodos os testes passaram.\n", failures);
    return failures ? 1 : 0;
}
