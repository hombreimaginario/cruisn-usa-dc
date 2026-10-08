#ifndef TEXTFONT_H
#define TEXTFONT_H

#include <stdint.h>

#define TF_W 12
#define TF_H 24

/* Copiar la fuente de la BIOS (llamar al arrancar, antes de leer el CD) */
void tf_init(void);
/* Texto en un bufer RGB565 de 'width' pixeles de ancho, sin pasar por la BIOS */
void tf_draw(uint16_t *buf, int width, const char *s, uint16_t fg, uint16_t bg);

#endif
