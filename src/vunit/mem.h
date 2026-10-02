/*
 * mem.h - Mapa de memoria del Midway V-Unit visto por el TMS320C31.
 *
 * Todas las direcciones son de palabra (32 bits), como en el C3x. Fuente:
 * VUNIT.EQU y CUSA.CMD de la fuente original.
 */
#ifndef VUNIT_MEM_H
#define VUNIT_MEM_H

#include <stdint.h>

#define VU_FASTRAM_BASE   0x000000u   /* RAM de programa; el reset copia aqui la ROM */
#define VU_FASTRAM_WORDS  0x020000u
#define VU_FIFO_ADDR      0x600000u   /* escrituras de poligonos a la FIFO de DMA */
#define VU_C31_REGS       0x808000u   /* registros internos del C31 (timers, puerto serie) */
#define VU_C31_RAM_BASE   0x809800u   /* RAM interna de 2K palabras del C31 */
#define VU_C31_RAM_WORDS  0x000800u
#define VU_SCREEN_BASE    0x900000u   /* framebuffer 512x1024, 2 paginas de 512x512 */
#define VU_DMA_REGS       0x980000u   /* registros del motor de poligonos */
#define VU_SWITCHES       0x991000u
#define VU_DIPSW          0x992000u
#define VU_SOUND          0x995000u
#define VU_CMOS_BASE      0x9C0000u
#define VU_CMOS_WORDS     0x002000u
#define VU_COLORAM_BASE   0x9E0000u   /* paleta: 32K entradas RGB */
#define VU_COLORAM_WORDS  0x008000u
#define VU_WAVERAM_BASE   0xA00000u   /* texturas descargadas (2D image store) */

#define VU_PROGRAM_WORDS  0x080000u   /* 4 x 512 KB de ROM de programa */

typedef struct {
    uint32_t fastram[VU_FASTRAM_WORDS];
    uint32_t c31ram[VU_C31_RAM_WORDS];
    uint32_t cmos[VU_CMOS_WORDS];
    uint16_t coloram[VU_COLORAM_WORDS];   /* convertido a RGB555 */
    const uint32_t *program;              /* imagen de programa (romtool.py) */
} vunit_mem;

extern vunit_mem vu;

void     vu_reset(const uint32_t *program);
uint32_t vu_read(uint32_t addr);
void     vu_write(uint32_t addr, uint32_t value);

#endif /* VUNIT_MEM_H */
