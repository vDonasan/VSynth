/* No plugin o MIDI entra e sai pela DAW, não por dispositivos: a interface
   (gui.c) é a mesma do app, então as funções de dispositivo MIDI existem
   mas não fazem nada. */
#include "midi.h"

int         midi_available(void) { return 0; }
int         midi_refresh(void) { return 0; }
const char *midi_device_name(int i) { (void)i; return ""; }
int         midi_open(int i, EventQueue *q) { (void)i; (void)q; return -1; }
void        midi_close(void) {}
int         midi_is_open(void) { return 0; }
const char *midi_open_name(void) { return ""; }
int         midi_out_count(void) { return 0; }
const char *midi_out_name(int i) { (void)i; return ""; }
int         midi_virtual_supported(void) { return 0; }
int         midi_out_open(int i, EventQueue *q) { (void)i; (void)q; return -1; }
void        midi_out_close(void) {}
int         midi_out_is_open(void) { return 0; }
const char *midi_out_open_name(void) { return ""; }
void        midi_out_test(void) {}
int         midi_out_activity(void) { return 0; }
int         midi_cc_param(int cc) { (void)cc; return -1; }
int         midi_param_cc(int param) { (void)param; return -1; }
void        midi_map(int cc, int param) { (void)cc; (void)param; }
int         midi_last_cc(int *counter) { if (counter) *counter = 0; return -1; }
int         midi_map_save(const char *path) { (void)path; return 0; }
int         midi_map_load(const char *path) { (void)path; return 0; }
