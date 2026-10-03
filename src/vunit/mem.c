/*
 * mem.c - Bus del Midway V-Unit (emulacion de alto nivel).
 *
 * Implementa c3x_mem_read/c3x_mem_write para el interprete y el codigo
 * recompilado. Los accesos a zonas que todavia no se emulan se cuentan por
 * pagina para descubrir que hardware usa el juego en cada fase.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../c3x/c3x.h"
#include "mem.h"

#ifdef VU_SWAPPABLE
static vunit_mem vu_storage;
vunit_mem *vu_cur = &vu_storage;
#else
vunit_mem vu;
#endif

static uint64_t no_cycles(void) { return 0; }
static uint32_t no_pc(void) { return 0; }
static void no_irq(int bit) { (void)bit; }

uint64_t (*vu_get_cycles)(void) = no_cycles;
uint32_t (*vu_get_pc)(void) = no_pc;
void     (*vu_raise_irq)(int bit) = no_irq;
uint32_t (*vu_gfx_fetch)(uint32_t word_index);

/* ---- registro de accesos sin mapear ---- */

#define UNMAPPED_SLOTS 64
static struct {
    uint32_t addr, first_pc, last_value, count;
    int write;
} unmapped_log[UNMAPPED_SLOTS];

static void unmapped(int write, uint32_t addr, uint32_t value)
{
    int i;
    for (i = 0; i < UNMAPPED_SLOTS; i++) {
        if (unmapped_log[i].count == 0) {
            unmapped_log[i].addr = addr;
            unmapped_log[i].write = write;
            unmapped_log[i].first_pc = vu_get_pc();
            unmapped_log[i].last_value = value;
            unmapped_log[i].count = 1;
            if (vu.trace_unmapped)
                printf("vunit: %s sin mapear %06X = %08X (pc %06X)\n",
                       write ? "escritura" : "lectura", (unsigned)addr,
                       (unsigned)value, (unsigned)unmapped_log[i].first_pc);
            return;
        }
        if (unmapped_log[i].addr == addr && unmapped_log[i].write == write) {
            unmapped_log[i].count++;
            unmapped_log[i].last_value = value;
            return;
        }
    }
}

void vu_report_unmapped(void)
{
    int i;
    for (i = 0; i < UNMAPPED_SLOTS && unmapped_log[i].count; i++)
        printf("  %s %06X x%u (primer pc %06X, ultimo valor %08X)\n",
               unmapped_log[i].write ? "W" : "R", (unsigned)unmapped_log[i].addr,
               (unsigned)unmapped_log[i].count, (unsigned)unmapped_log[i].first_pc,
               (unsigned)unmapped_log[i].last_value);
}

/* ---- reset ---- */

void vu_reset(const uint32_t *program, const uint32_t *gfx)
{
    memset(&vu, 0, sizeof(vu));
    vu.program = program;
    vu.gfx = gfx;
    /* El hardware arranca con la ROM visible en FASTRAM. */
    memcpy(vu.fastram, program, VU_FASTRAM_WORDS * sizeof(uint32_t));
    memset(vu.cmos, 0xFF, sizeof(vu.cmos));
    vu.in.wheel = 0x80;
    vu.in.dipsw = 0xFFFFFFFFu;
    memset(unmapped_log, 0, sizeof(unmapped_log));
}

/* Los tests de memoria del arranque copian codigo a la RAM interna del C31 y
 * tardan ~8 s de juego. En la version recompilada (y en Dreamcast) se saltan
 * sustituyendo sus CALL por NOP. Devuelve el numero de parches aplicados. */
int vu_skip_memtests(void)
{
    static const struct { uint32_t addr, word; } calls[] = {
        { 0x004AFF, 0x620062D5u },   /* CALL TEST_STATIC_CHIPS */
        { 0x004B17, 0x62006381u },   /* CALL TEST_CHIPS */
    };
    unsigned i;
    int n = 0;
    for (i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        if (vu.fastram[calls[i].addr] == calls[i].word) {
            vu.fastram[calls[i].addr] = 0x0C800000u;   /* NOP */
            n++;
        }
    }
    return n;
}

/* ---- perifericos internos del C31 (temporizadores) ---- */

static uint32_t timer_counter(int t)
{
    /* El temporizador interno cuenta a H1/2: la mitad de la frecuencia de
     * instruccion. */
    return (uint32_t)((vu_get_cycles() - vu.timer_base[t]) / 2);
}

static uint32_t c31regs_read(uint32_t off)
{
    if (off == 0x24) return timer_counter(0);
    if (off == 0x34) return timer_counter(1);
    return vu.c31regs[off & 0xFF];
}

static void c31regs_write(uint32_t off, uint32_t v)
{
    if (off == 0x20 || off == 0x30) {
        int t = off == 0x30;
        /* GO (bit 6) reinicia el contador */
        if (v & 0x40)
            vu.timer_base[t] = vu_get_cycles();
    } else if (off == 0x24 || off == 0x34) {
        vu.timer_base[off == 0x34] = vu_get_cycles() - (uint64_t)v * 2;
    }
    vu.c31regs[off & 0xFF] = v;
}

/* ---- registros de video / DMA ---- */

static uint32_t dma_read(uint32_t off)
{
    switch (off) {
    case 0x00:              /* FIFO_CNTR */
        return vu.fifo_count;
    case 0x20: {            /* CRT_VCNT: linea actual aproximada */
        uint64_t per_frame = 25000000 / 57;
        return (uint32_t)((vu_get_cycles() % per_frame) * 432 / per_frame);
    }
    case 0x40:              /* DMA_SETUP */
        return vu.page_control;
    case 0x82:              /* FIFO_STATUS: siempre vacia y libre */
        return 0;
    case 0x83:              /* FIFO_INC: entrega el paquete al motor */
        vu_dma_process();
        return 0;
    default:
        return vu.dma_regs[off & 0xFF];
    }
}

static void dma_write(uint32_t off, uint32_t v)
{
    if (off == 0x40) {
        vu.page_control = v;
        return;
    }
    vu.dma_regs[off & 0xFF] = v;
}

/* ---- E/S ---- */

static uint32_t io_read(uint32_t addr)
{
    switch (addr) {
    case 0x991030:          /* SWITCH3: bits 31-16 de la palabra de switches */
        return ~(vu.in.switches >> 16) & 0xFFFF;
    case 0x991060:          /* SWITCH1: bits 15-0 */
        return ~vu.in.switches & 0xFFFF;
    case 0x991050:          /* SWITCH2 / watchdog */
        return 0xFFFF;
    case 0x992000:
        return vu.in.dipsw;
    case 0x993000:          /* ATOD_R: valor en los bits 31-24 */
        return vu.adc_value << 24;
    case 0x994000:
        return vu.syscntl;
    }
    unmapped(0, addr, 0);
    return 0xFFFFFFFFu;
}

static void io_write(uint32_t addr, uint32_t v)
{
    switch (addr) {
    case 0x993000: {        /* ATOD_R: inicia conversion del canal v>>24 */
        uint32_t ch = (v >> 24) & 0xFF;
        vu.adc_value = ch == 4 ? vu.in.wheel : ch == 5 ? vu.in.gas : ch == 6 ? vu.in.brake : 0;
        vu.adc_irq_delay = 2;
#ifdef ADC_DEBUG
        printf("adc: canal %u -> %02X (pc %06X)\n", (unsigned)ch, (unsigned)vu.adc_value, (unsigned)vu_get_pc());
#endif
        return;
    }
    case 0x994000:
        vu.syscntl = v;
        return;
    case 0x995000:          /* puerto de sonido: pendiente (fase 4) */
    case 0x995020:          /* proteccion de escritura de CMOS */
    case 0x996000:          /* salida de luces / IDE */
    case 0x991000:
        return;
    }
    unmapped(1, addr, v);
}

void vu_tick(void)
{
    if (vu.adc_irq_delay && --vu.adc_irq_delay == 0)
        vu_raise_irq(3);
}

/* ---- bus ---- */

uint32_t c3x_mem_read(uint32_t addr)
{
    if (addr < VU_FASTRAM_WORDS)
        return vu.fastram[addr];
    switch (addr >> 20) {
    case 0x4:
        if (addr - VU_RAM2_BASE < VU_RAM2_WORDS)
            return vu.ram2[addr - VU_RAM2_BASE];
        break;
    case 0x8:
        if (addr - VU_C31_RAM_BASE < VU_C31_RAM_WORDS)
            return vu.c31ram[addr - VU_C31_RAM_BASE];
        if ((addr & 0xFFFF00) == VU_C31_REGS)
            return c31regs_read(addr & 0xFF);
        break;
    case 0x9:
        if (addr < VU_VIDEO_BASE + VU_VIDEO_WORDS)
            return vu.video[addr - VU_VIDEO_BASE];
        if ((addr & 0xFFFF00) == VU_DMA_REGS)
            return dma_read(addr & 0xFF);
        if (addr - VU_CMOS_BASE < VU_CMOS_WORDS)
            return vu.cmos[addr - VU_CMOS_BASE];
        if (addr - VU_COLORAM_BASE < VU_COLORAM_WORDS)
            return vu.coloram[addr - VU_COLORAM_BASE];
        return io_read(addr);
    case 0xA: case 0xB: {
        uint32_t o = (addr - VU_TEXRAM_BASE) * 2;
        return vu.texram[o] | (vu.texram[o + 1] << 8);
    }
    case 0xC: case 0xD: case 0xE:
        if (addr - VU_PROGROM_BASE < VU_PROGRAM_WORDS)
            return vu.program[addr - VU_PROGROM_BASE];
        if (addr - VU_GFXROM_BASE < VU_GFXROM_WORDS)
            return vu.gfx ? vu.gfx[addr - VU_GFXROM_BASE]
                 : vu_gfx_fetch ? vu_gfx_fetch(addr - VU_GFXROM_BASE) : 0;
        return 0;
    }
    unmapped(0, addr, 0);
    return 0;
}

uint32_t vu_watch_addr = 0xFFFFFFFFu;

void c3x_mem_write(uint32_t addr, uint32_t v)
{
    if (addr == vu_watch_addr)
        printf("watch: [%06X] = %08X (pc %06X, ciclo %llu)\n", (unsigned)addr, (unsigned)v,
               (unsigned)vu_get_pc(), (unsigned long long)vu_get_cycles());
    if (addr == vu_watch_addr && getenv("CUSA_RING")) {
        unsigned i;
        printf("  ultimos PC:");
        for (i = 0; i < 64; i++)
            printf(" %06X", (unsigned)c3x_pc_ring[(c3x_pc_ring_pos + i) & 63]);
        printf("\n");
    }
    if (addr < VU_FASTRAM_WORDS) {
        vu.fastram[addr] = v;
        return;
    }
    switch (addr >> 20) {
    case 0x4:
        if (addr - VU_RAM2_BASE < VU_RAM2_WORDS) {
            vu.ram2[addr - VU_RAM2_BASE] = v;
            return;
        }
        break;
    case 0x6:               /* FIFO de poligonos */
        if (vu.fifo_count < 16)
            vu.fifo[vu.fifo_count++] = v;
        return;
    case 0x8:
        if (addr - VU_C31_RAM_BASE < VU_C31_RAM_WORDS) {
            vu.c31ram[addr - VU_C31_RAM_BASE] = v;
            return;
        }
        if ((addr & 0xFFFF00) == VU_C31_REGS) {
            c31regs_write(addr & 0xFF, v);
            return;
        }
        break;
    case 0x9:
        if (addr < VU_VIDEO_BASE + VU_VIDEO_WORDS) {
            vu.video[addr - VU_VIDEO_BASE] = (uint16_t)v;
            return;
        }
        if ((addr & 0xFFFF00) == VU_DMA_REGS) {
            dma_write(addr & 0xFF, v);
            return;
        }
        if (addr - VU_CMOS_BASE < VU_CMOS_WORDS) {
            vu.cmos[addr - VU_CMOS_BASE] = v;
            return;
        }
        if (addr - VU_COLORAM_BASE < VU_COLORAM_WORDS) {
            vu.coloram[addr - VU_COLORAM_BASE] = (uint16_t)v;
            return;
        }
        io_write(addr, v);
        return;
    case 0xA: case 0xB: {
        uint32_t o = (addr - VU_TEXRAM_BASE) * 2;
        vu.texram[o] = (uint8_t)v;
        vu.texram[o + 1] = (uint8_t)(v >> 8);
        return;
    }
    case 0xC: case 0xD: case 0xE:
        return;             /* escrituras a ROM: se ignoran */
    }
    unmapped(1, addr, v);
}
