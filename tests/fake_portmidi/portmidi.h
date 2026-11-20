/* PortMidi "de mentira" para testar a saída MIDI sem hardware:
   guarda as mensagens que seriam enviadas. Só para tests/test_midi_out.c. */
#ifndef FAKE_PORTMIDI_H
#define FAKE_PORTMIDI_H
#include <stdint.h>
typedef void PortMidiStream;
#define PmStream PortMidiStream
typedef int PmDeviceID;
typedef int32_t PmMessage;
typedef int32_t PmTimestamp;
typedef PmTimestamp (*PmTimeProcPtr)(void *);
typedef enum { pmNoError = 0, pmNoDevice = -1 } PmError;
typedef struct { PmMessage message; PmTimestamp timestamp; } PmEvent;
typedef struct { int structVersion; const char *interf; char *name; int input, output, opened, is_virtual; } PmDeviceInfo;
#define PM_FILT_ACTIVE 1
#define PM_FILT_CLOCK 2
#define PM_FILT_SYSEX 4
#define Pm_Message(status, data1, data2) ((((data2) << 16) & 0xFF0000) | (((data1) << 8) & 0xFF00) | ((status) & 0xFF))
#define Pm_MessageStatus(msg) ((msg) & 0xFF)
#define Pm_MessageData1(msg) (((msg) >> 8) & 0xFF)
#define Pm_MessageData2(msg) (((msg) >> 16) & 0xFF)
extern PmMessage fake_sent[4096];
extern int fake_nsent;
PmError Pm_Initialize(void);
PmError Pm_Terminate(void);
int Pm_CountDevices(void);
PmDeviceID Pm_GetDefaultInputDeviceID(void);
const PmDeviceInfo *Pm_GetDeviceInfo(PmDeviceID id);
PmError Pm_OpenInput(PortMidiStream **s, PmDeviceID id, void *info, int32_t buf, PmTimeProcPtr t, void *ti);
PmError Pm_OpenOutput(PortMidiStream **s, PmDeviceID id, void *info, int32_t buf, PmTimeProcPtr t, void *ti, int32_t lat);
PmError Pm_SetFilter(PortMidiStream *s, int32_t filters);
int Pm_Read(PortMidiStream *s, PmEvent *buf, int32_t len);
PmError Pm_WriteShort(PortMidiStream *s, PmTimestamp when, PmMessage msg);
PmError Pm_Close(PortMidiStream *s);
PmError Pm_CreateVirtualOutput(const char *name, const char *interf, void *info);
PmError Pm_DeleteVirtualDevice(PmDeviceID id);
#endif
