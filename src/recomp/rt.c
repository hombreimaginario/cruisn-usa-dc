/*
 * rt.c - Despachador, interrupciones y utilidades del codigo recompilado.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

rt_state C;
#ifdef RT_TRACE_ON
void (*rt_trace_hook)(uint32_t pc);
#endif
int rt_irq_disabled;            /* pruebas diferenciales: sin eventos ni IRQ */
static uint64_t next_periodic;

/* ---- busqueda de region ---- */

static const rt_region *find_region(uint32_t pc)
{
    unsigned lo = 0, hi = rt_num_regions;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (pc < rt_regions[mid].start)
            hi = mid;
        else if (pc >= rt_regions[mid].end)
            lo = mid + 1;
        else
            return &rt_regions[mid];
    }
    return NULL;
}

uint32_t rt_unknown(uint32_t pc)
{
    fprintf(stderr, "rt: PC %06X sin codigo recompilado (ciclo %llu, sp %06X)\n",
            (unsigned)pc, (unsigned long long)rt_cycles(), (unsigned)C.r[C3X_SP]);
    abort();
    return pc;
}

uint32_t rt_dispatch(uint32_t pc)
{
    const rt_region *r = find_region(pc & 0xFFFFFF);
    if (!r)
        return rt_unknown(pc);
    return r->fn(pc & 0xFFFFFF);
}

uint64_t rt_cycle_limit = ~0ULL;   /* pruebas: abortar si se supera */
void (*rt_limit_cb)(void);

uint32_t rt_call(uint32_t target, uint32_t ret)
{
    uint32_t pc = target;
    while (pc != ret) {
        if (UNLIKELY(rt_cycles() > rt_cycle_limit) && rt_limit_cb)
            rt_limit_cb();
        pc = rt_dispatch(pc);
    }
    return pc;
}

/* ---- eventos e interrupciones ---- */

uint32_t rt_service(uint32_t resume_pc)
{
    uint32_t pending;
    int bit;

    if (rt_irq_disabled) {
        C.next_ev = C.cyc + 1000;
        if (rt_cycles() > rt_cycle_limit && rt_limit_cb)
            rt_limit_cb();
        return resume_pc;
    }
    if (C.cyc < C.cyc_last)
        C.cyc_hi++;
    C.cyc_last = C.cyc;
    if (rt_cycles() >= next_periodic) {
        next_periodic = rt_cycles() + RT_EVENT_PERIOD;
        rt_platform_event();
    }
    C.next_ev = (uint32_t)next_periodic;

    if (!(C.r[C3X_ST] & C3X_ST_GIE))
        return resume_pc;
    pending = C.r[C3X_IE] & C.r[C3X_IF] & 0x7FF;
    if (!pending)
        return resume_pc;
    for (bit = 0; !(pending & (1u << bit)); bit++)
        ;
    C.r[C3X_IF] &= ~(1u << bit);
    C.r[C3X_ST] &= ~C3X_ST_GIE;
    PUSH(resume_pc);
    /* El manejador termina con RETI, que devuelve resume_pc. */
    return rt_call(RD(1 + bit) & 0xFFFFFF, resume_pc);
}

void rt_reset(void)
{
    int i;
    memset(&C, 0, sizeof(C));
    for (i = 0; i < 8; i++) {
        C.f[i] = 0.0f;
        C.e[i] = -128;
        C.rk[i] = 3;
    }
    next_periodic = RT_EVENT_PERIOD;
    C.next_ev = RT_EVENT_PERIOD;
}

void rt_run(void)
{
    uint32_t pc = RD(0) & 0xFFFFFF;
    for (;;)
        pc = rt_dispatch(pc);
}

void rt_idle(void)
{
    rt_platform_idle();
    RT_FORCE_CHECK();
}

/* ---- memoria ---- */

#ifdef RT_COUNT_SLOW
uint32_t rt_slow_count[2][256];
#endif

uint32_t rt_rd_slow(uint32_t a)
{
#ifdef RT_COUNT_SLOW
    rt_slow_count[0][a >> 16]++;
#endif
    if (a - VU_C31_RAM_BASE < VU_C31_RAM_WORDS)
        return vu.c31ram[a - VU_C31_RAM_BASE];
    if (a - VU_PROGROM_BASE < VU_PROGRAM_WORDS && vu.program)
        return vu.program[a - VU_PROGROM_BASE];   /* modelos 3D y tablas en ROM */
    return c3x_mem_read(a);
}

void rt_wr_slow(uint32_t a, uint32_t v)
{
#ifdef RT_COUNT_SLOW
    rt_slow_count[1][a >> 16]++;
#endif
    if (a - VU_C31_RAM_BASE < VU_C31_RAM_WORDS)
        vu.c31ram[a - VU_C31_RAM_BASE] = v;
    else if ((a >> 20) == 6) {              /* FIFO de poligonos */
        if (vu.fifo_count < 16)
            vu.fifo[vu.fifo_count++] = v;
    } else
        c3x_mem_write(a, v);
}

/* ---- operaciones poco frecuentes ---- */

uint32_t rt_bitrev(uint32_t ar, uint32_t ir)
{
    uint32_t ra = 0, ri = 0, r = 0, i;
    for (i = 0; i < 24; i++) {
        ra |= ((ar >> i) & 1u) << (23 - i);
        ri |= ((ir >> i) & 1u) << (23 - i);
    }
    ra = (ra + ri) & 0xFFFFFF;
    for (i = 0; i < 24; i++)
        r |= ((ra >> i) & 1u) << (23 - i);
    return (ar & 0xFF000000u) | r;
}

float rt_lde(float dst, float src)
{
    uint32_t d = float_to_c3x(dst), s = float_to_c3x(src);
    if ((s >> 24) == 0x80)
        return 0.0f;
    return c3x_to_float((s & 0xFF000000u) | (d & 0x00FFFFFFu));
}

float rt_ldm(float dst, float src)
{
    uint32_t d = float_to_c3x(dst), s = float_to_c3x(src);
    return c3x_to_float((d & 0xFF000000u) | (s & 0x00FFFFFFu));
}
