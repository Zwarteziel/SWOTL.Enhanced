/* SWOTL Enhanced - high-level emulation of BIOS and DOS services. GPL-3.0-or-later. */
#ifndef SWOTL_HLE_H
#define SWOTL_HLE_H
#include "machine.h"

void hle_init(Machine *m);
int hle_load_program(Machine *m, const char *exe, const char *args);

/* shared helpers implemented in machine.c */
void vga_set_mode(Machine *m, int mode, int clear);
void vga_set_dac(Machine *m, int i, uint8_t r, uint8_t g, uint8_t b);
const uint8_t *machine_font8x8(void);
void machine_set_audio_rate(Machine *m, int rate);

/* host file lookup (case-insensitive) */
int host_resolve(Machine *m, const char *dospath, char *out, size_t outsz, int *exists);

#endif
