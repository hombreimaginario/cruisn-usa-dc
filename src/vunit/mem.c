/*
 * mem.c - Despacho de lecturas/escrituras del C31 al hardware emulado (HLE).
 *
 * Los accesos a RAM se resuelven aqui con arrays planos. Los registros de
 * video, entradas y sonido se iran implementando en sus propios modulos; por
 * ahora se registran para poder descubrir que usa el juego en cada fase.
 */
#include <stdio.h>
#include <string.h>

#include "mem.h"

vunit_mem vu;

void vu_reset(const uint32_t *program)
{
    memset(&vu, 0, sizeof(vu));
    vu.program = program;
    /* El hardware copia el inicio de la ROM a la FASTRAM al arrancar. */
    memcpy(vu.fastram, program, VU_FASTRAM_WORDS * sizeof(uint32_t));
}

static void unmapped(const char *what, uint32_t addr, uint32_t value)
{
#ifdef VU_TRACE_UNMAPPED
    printf("vunit: %s sin mapear %06X = %08X\n", what, (unsigned)addr, (unsigned)value);
#else
    (void)what; (void)addr; (void)value;
#endif
}

uint32_t vu_read(uint32_t addr)
{
    addr &= 0xFFFFFFu;
    if (addr < VU_FASTRAM_WORDS)
        return vu.fastram[addr];
    if (addr - VU_C31_RAM_BASE < VU_C31_RAM_WORDS)
        return vu.c31ram[addr - VU_C31_RAM_BASE];
    if (addr - VU_CMOS_BASE < VU_CMOS_WORDS)
        return vu.cmos[addr - VU_CMOS_BASE] & 0xFFu;
    if (addr - VU_COLORAM_BASE < VU_COLORAM_WORDS)
        return vu.coloram[addr - VU_COLORAM_BASE];
    unmapped("lectura", addr, 0);
    return 0;
}

void vu_write(uint32_t addr, uint32_t value)
{
    addr &= 0xFFFFFFu;
    if (addr < VU_FASTRAM_WORDS) {
        vu.fastram[addr] = value;
        return;
    }
    if (addr - VU_C31_RAM_BASE < VU_C31_RAM_WORDS) {
        vu.c31ram[addr - VU_C31_RAM_BASE] = value;
        return;
    }
    if (addr - VU_CMOS_BASE < VU_CMOS_WORDS) {
        vu.cmos[addr - VU_CMOS_BASE] = value & 0xFFu;
        return;
    }
    if (addr - VU_COLORAM_BASE < VU_COLORAM_WORDS) {
        vu.coloram[addr - VU_COLORAM_BASE] = (uint16_t)value;
        return;
    }
    unmapped("escritura", addr, value);
}
