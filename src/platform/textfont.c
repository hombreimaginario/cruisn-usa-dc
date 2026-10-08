/*
 * textfont.c - Texto sin la BIOS.
 *
 * bfont de KallistiOS pide a la BIOS acceso a su fuente en cada llamada
 * (syscall_font_lock) y espera hasta conseguirlo. En la consola real la BIOS
 * comparte ese acceso con el lector de discos: con lecturas del CD en
 * marcha la espera puede no terminar y el juego se queda en negro. En
 * Flycast la BIOS es una emulacion y nunca pasa.
 *
 * Aqui se copian los caracteres ASCII de la fuente una sola vez al arrancar,
 * antes de tocar el CD, y despues se pinta siempre desde esa copia.
 */
#include <kos.h>
#include <dc/biosfont.h>
#include <dc/syscalls.h>

#include <string.h>

#include "textfont.h"

#define NCHARS 95                        /* ASCII 32-126 */

static uint8_t glyphs[NCHARS][BFONT_BYTES_PER_CHAR];
static int ready;

void tf_init(void)
{
    int c;
    if (ready)
        return;
    /* al arrancar el lector aun no se usa: el acceso a la fuente es inmediato */
    while (syscall_font_lock() != 0)
        ;
    for (c = 0; c < NCHARS; c++)
        memcpy(glyphs[c], bfont_find_char((uint32_t)(32 + c)), BFONT_BYTES_PER_CHAR);
    syscall_font_unlock();
    ready = 1;
}

static void draw_row(uint16_t *o, uint16_t bits, uint16_t fg, uint16_t bg)
{
    int x;
    for (x = 0; x < TF_W; x++)
        o[x] = (bits & (0x800 >> x)) ? fg : bg;
}

void tf_draw(uint16_t *buf, int width, const char *s, uint16_t fg, uint16_t bg)
{
    if (!ready)
        return;
    for (; *s; s++, buf += TF_W) {
        int c = (unsigned char)*s, y;
        const uint8_t *g;
        uint16_t *o = buf;
        if (c < 32 || c > 126)
            c = '?';
        g = glyphs[c - 32];
        /* dos filas de 12 bits cada 3 bytes, como bfont_draw_ex */
        for (y = 0; y < TF_H; y += 2, g += 3) {
            draw_row(o, (uint16_t)((g[0] << 4) | (g[1] >> 4)), fg, bg);
            o += width;
            draw_row(o, (uint16_t)(((g[1] & 0x0F) << 8) | g[2]), fg, bg);
            o += width;
        }
    }
}
