/*
 * mem.h - Mapa de memoria del Midway V-Unit visto por el TMS320C31.
 *
 * Todas las direcciones son de palabra (32 bits), como en el C3x. Fuentes:
 * VUNIT.EQU, C30.EQU y CUSA.CMD de la fuente original y el analisis de la ROM
 * (ver docs/ANALISIS.md).
 */
#ifndef VUNIT_MEM_H
#define VUNIT_MEM_H

#include <stdint.h>

#define VU_FASTRAM_WORDS  0x020000u   /* 0x000000: RAM de programa */
#define VU_RAM2_BASE      0x400000u   /* RAM secundaria (copia de prueba) */
#define VU_RAM2_WORDS     0x020000u
#define VU_FIFO_ADDR      0x600000u   /* escrituras de poligonos a la FIFO */
#define VU_C31_REGS       0x808000u   /* perifericos internos del C31 */
#define VU_C31_RAM_BASE   0x809800u   /* RAM interna de 2K palabras */
#define VU_C31_RAM_WORDS  0x000800u
#define VU_VIDEO_BASE     0x900000u   /* framebuffer 512x1024 de 16 bits */
#define VU_VIDEO_WORDS    0x080000u
#define VU_DMA_REGS       0x980000u
#define VU_CMOS_BASE      0x9C0000u
#define VU_CMOS_WORDS     0x002000u
#define VU_COLORAM_BASE   0x9E0000u   /* paleta: 32K entradas xRGB555 */
#define VU_COLORAM_WORDS  0x008000u
#define VU_TEXRAM_BASE    0xA00000u   /* WAVERAM: 2 texeles de 8 bits por palabra */
#define VU_TEXRAM_WORDS   0x200000u
#define VU_PROGROM_BASE   0xC00000u
#define VU_PROGRAM_WORDS  0x080000u   /* 4 x 512 KB */
#define VU_GFXROM_BASE    0xC80000u   /* justo despues del programa (verificado con MAME) */
#define VU_GFXROM_WORDS   0x200000u   /* 16 x 512 KB */

/* Entradas (activas a nivel bajo en el hardware real). */
typedef struct {
    uint32_t switches;   /* bits SW_* de VUNIT.EQU; 1 = pulsado */
    uint8_t  wheel;      /* 0x80 = centro */
    uint8_t  gas;
    uint8_t  brake;
    uint32_t dipsw;
} vunit_inputs;

typedef struct {
    uint32_t fastram[VU_FASTRAM_WORDS];
    uint32_t ram2[VU_RAM2_WORDS];
    uint32_t c31ram[VU_C31_RAM_WORDS];
    uint32_t c31regs[0x100];
    uint32_t cmos[VU_CMOS_WORDS];
    uint16_t coloram[VU_COLORAM_WORDS];
    uint16_t video[VU_VIDEO_WORDS];
    uint8_t  texram[VU_TEXRAM_WORDS * 2];
    const uint32_t *program;
    const uint32_t *gfx;

    /* motor de poligonos */
    uint32_t dma_regs[0x100];
    uint32_t fifo[16];
    int      fifo_count;
    uint32_t page_control;     /* DMA_SETUP */
    uint32_t polys_frame;      /* poligonos dibujados en el frame actual */

    /* miscelanea */
    uint32_t syscntl;
    uint32_t adc_value;
    int      adc_irq_delay;
    uint64_t timer_base[2];
    vunit_inputs in;

    /* trazado de accesos sin mapear */
    int      trace_unmapped;
} vunit_mem;

#ifdef VU_SWAPPABLE
/* Pruebas diferenciales: la memoria se puede intercambiar entre copias. */
extern vunit_mem *vu_cur;
#define vu (*vu_cur)
#else
extern vunit_mem vu;
#endif
extern uint32_t vu_watch_addr;   /* depuracion: traza escrituras */

/* Ciclo actual de la CPU y PC para temporizadores y depuracion. */
extern uint64_t (*vu_get_cycles)(void);
extern uint32_t (*vu_get_pc)(void);
extern void     (*vu_raise_irq)(int bit);

/* Lectura de la ROM grafica cuando no cabe en memoria (vu.gfx == NULL):
 * la plataforma la sirve desde disco con una cache de paginas. */
extern uint32_t (*vu_gfx_fetch)(uint32_t word_index);

void vu_reset(const uint32_t *program, const uint32_t *gfx);
int  vu_skip_memtests(void);
void vu_tick(void);                       /* llamar cada ~1000 instrucciones */
void vu_report_unmapped(void);

/* video.c */
void vu_dma_process(void);
void vu_video_to_rgb565(uint16_t *dst, int pitch, int page);

#endif /* VUNIT_MEM_H */
