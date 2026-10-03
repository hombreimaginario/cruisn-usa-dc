#ifndef SOUND_PREVIEW_H
#define SOUND_PREVIEW_H

#include <stdint.h>

int  sound_preview_init(const char *bank_path, const char *wav_path);
void sound_preview_byte(uint32_t v);    /* byte del puerto DCS o 0x100|reset */
void sound_preview_frame(void);         /* una interrupcion de video */
void sound_preview_close(void);

#endif
