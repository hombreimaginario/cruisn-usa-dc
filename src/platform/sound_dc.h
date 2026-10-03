#ifndef SOUND_DC_H
#define SOUND_DC_H

#include <stdint.h>

/* Carga /cd/sound.bin (tools/dcs_bank.py). Devuelve 0 si hay sonido. */
int sound_init(const char *path);
/* Byte escrito por el juego en el puerto del DCS (o 0x100|bit de reset) */
void sound_dcs_write(uint32_t v);
/* Llamar una vez por frame: alimenta el stream de musica */
void sound_frame(void);

#endif
