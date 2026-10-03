/*
 * difftest.c - Prueba diferencial: interprete de referencia vs. recompilado.
 *
 * Ejecuta el juego con el interprete. Cada vez que entra en una funcion
 * (inicio de region) guarda el estado, ejecuta la funcion completa con el
 * interprete y despues con el codigo recompilado desde el mismo estado (ambas
 * sin interrupciones), y compara registros y FASTRAM. Informa de las
 * funciones que no coinciden. Cada funcion se prueba hasta N veces.
 *
 * Uso: cusa_difftest <dir_generated> [frames] [pruebas_por_funcion]
 */
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../recomp/rt.h"

#define INSNS_PER_FRAME (25000000 / 57)
#define MAX_STEPS 3000000

static c3x_state cpu;
static uint32_t cpu_gie_before;

static uint8_t *is_start;
static uint16_t *tested;
static int per_func = 2, reported, failures, tests;
static int irq_blocked;
static jmp_buf limit_jmp;



static uint64_t get_cycles(void) { return cpu.cycles; }
static uint32_t get_pc(void) { return cpu.pc; }
static void raise_irq(int bit)
{
    if (!irq_blocked)
        c3x_set_irq(&cpu, bit);
}

void rt_platform_event(void) { }
void rt_platform_idle(void) { }

static void *load_file(const char *dir, const char *name, size_t *size)
{
    char path[1024];
    FILE *f;
    void *buf;
    long n;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    if (size)
        *size = (size_t)n;
    return buf;
}

static int close_float(uint32_t a, uint32_t b)
{
    float fa = c3x_to_float(a), fb = c3x_to_float(b);
    float d = fabsf(fa - fb), m = fmaxf(fabsf(fa), fabsf(fb));
    return d <= 1e-4f * m || d < 1e-12f;
}

static int close_d(double a, double b)
{
    double d = fabs(a - b), m = fmax(fabs(a), fabs(b));
    return d <= 1e-4 * m || d < 1e-9;
}

/* ---- ejecucion en paralelo bloque a bloque ---- */

static vunit_mem *mem_ref, *mem_rc;
static c3x_state ref;
static long ref_steps;
static int lock_failed;
static uint32_t prev_block;
static uint32_t cur_entry;

static int regs_match(uint32_t at)
{
    int i, bad = 0;
    for (i = 8; i < C3X_NUM_REGS; i++) {
        if (i == C3X_ST || i == C3X_IF || i == C3X_IOF || i == C3X_RC || i == C3X_RS || i == C3X_RE)
            continue;
        if (C.r[i] != ref.r[i]) {
            if (!bad)
                printf("  divergencia en bloque %06X (anterior %06X) funcion %06X:\n",
                       (unsigned)at, (unsigned)prev_block, (unsigned)cur_entry);
            printf("    reg %d recomp=%08X ref=%08X\n", i, (unsigned)C.r[i], (unsigned)ref.r[i]);
            bad++;
        }
    }
    for (i = 0; i < 8; i++) {
        uint8_t k = C.rk[i];
        uint32_t r0 = C.r[i];
        int32_t e0 = C.e[i];
        float f0 = C.f[i], fv;
        uint32_t iv;
        if (!(k & 2)) rt_sync_i(i);
        iv = C.r[i];
        if (!(k & 1)) rt_sync_f(i);
        fv = C.f[i];
        C.rk[i] = k; C.r[i] = r0; C.e[i] = e0; C.f[i] = f0;
        if ((iv >> 8) == (ref.r[i] >> 8) || close_d(fv, c3x_reg_double(&ref, i)))
            continue;
        if (!bad)
            printf("  divergencia en bloque %06X (anterior %06X) funcion %06X:\n",
                   (unsigned)at, (unsigned)prev_block, (unsigned)cur_entry);
        printf("    R%d recomp=%08X/%g ref=%08X/%g\n", i, (unsigned)C.r[i], C.f[i],
               (unsigned)ref.r[i], c3x_reg_double(&ref, i));
        bad++;
    }
    return bad == 0;
}

static void trace_hook(uint32_t pc)
{
    vu_cur = mem_ref;
    if (prev_block != 0) {          /* avanzar al menos una instruccion */
        c3x_step(&ref);
        ref_steps++;
    }
    while (ref.pc != pc && ref_steps < MAX_STEPS) {
        c3x_step(&ref);
        ref_steps++;
    }
    vu_cur = mem_rc;
    if (ref_steps >= MAX_STEPS)
        longjmp(limit_jmp, 2);
    if (!lock_failed && !regs_match(pc)) {
        lock_failed = 1;
        if (reported < 60)
            reported++;
    }
    prev_block = pc;
}

static void test_function(uint32_t entry)
{
    c3x_state start = cpu;
    vunit_mem *main_mem = vu_cur;
    uint32_t ret = c3x_mem_read(cpu.r[C3X_SP]) & 0xFFFFFF;
    uint32_t sp0 = cpu.r[C3X_SP];
    uint32_t pending_if = cpu.r[C3X_IF];
    int i, bad = 0, r;
    uint32_t a;

    if (ret >= VU_FASTRAM_WORDS && !(ret >= 0xC00000 && ret < 0xC80000))
        return;
    memcpy(mem_ref, main_mem, sizeof(vunit_mem));
    memcpy(mem_rc, main_mem, sizeof(vunit_mem));

    start.r[C3X_IF] = 0;
    start.r[C3X_ST] &= ~C3X_ST_GIE;
    ref = start;
    ref_steps = 0;
    lock_failed = 0;
    prev_block = 0;
    cur_entry = entry;
    irq_blocked = 1;

    memset(&C, 0, sizeof(C));
    memcpy(C.r, start.r, sizeof(start.r));
    for (i = 0; i < 8; i++) {
        C.f[i] = (float)c3x_reg_double(&start, i);
        C.e[i] = start.exp[i];
        C.rk[i] = 3;
    }
    rt_set_cycles(start.cycles);
    C.next_ev = C.cyc + 1000;
    rt_irq_disabled = 1;
    rt_cycle_limit = ~0ULL;
    rt_trace_hook = trace_hook;
    vu_cur = mem_rc;

    r = setjmp(limit_jmp);
    if (r == 0) {
        rt_call(entry, ret);
    }
    rt_trace_hook = NULL;
    rt_irq_disabled = 0;
    irq_blocked = 0;
    if (r != 0) {
        vu_cur = main_mem;      /* no vuelve: se descarta la prueba */
        return;
    }
    /* terminar la referencia */
    vu_cur = mem_ref;
    while (!(ref.pc == ret && ref.r[C3X_SP] == sp0 - 1) && ref_steps < MAX_STEPS) {
        c3x_step(&ref);
        ref_steps++;
    }
    if (ref_steps >= MAX_STEPS) {
        vu_cur = main_mem;
        return;
    }
    tests++;
    if (lock_failed)
        bad++;
    if ((C.r[C3X_ST] & 0x0D) != (ref.r[C3X_ST] & 0x0D)) {
        if (reported < 60)
            printf("  %06X: ST al volver recomp=%04X ref=%04X\n", (unsigned)entry,
                   (unsigned)C.r[C3X_ST], (unsigned)ref.r[C3X_ST]);
        bad++;
    }
    for (a = 0; a < VU_FASTRAM_WORDS; a++) {
        uint32_t x = mem_rc->fastram[a], y = mem_ref->fastram[a];
        if (x != y && !close_float(x, y)) {
            if (reported < 60)
                printf("  %06X: mem[%05X] recomp=%08X ref=%08X\n", (unsigned)entry,
                       (unsigned)a, (unsigned)x, (unsigned)y);
            bad++;
            break;
        }
    }
    if (memcmp(mem_rc->texram, mem_ref->texram, sizeof(mem_rc->texram)) != 0) {
        size_t k;
        for (k = 0; k < sizeof(mem_rc->texram) && mem_rc->texram[k] == mem_ref->texram[k]; k++)
            ;
        if (reported < 60)
            printf("  %06X: WAVERAM distinta desde el byte %zX\n", (unsigned)entry, k);
        bad++;
    }
    if (bad) {
        failures++;
        if (reported++ < 60)
            printf("FALLO funcion %06X (ret %06X, %ld pasos)\n", (unsigned)entry, (unsigned)ret, ref_steps);
    }
    /* seguir con el resultado de la referencia */
    {
        vunit_mem *t = main_mem;
        vu_cur = mem_ref;
        mem_ref = t;
    }
    cpu = ref;
    cpu.r[C3X_IF] |= pending_if;
    cpu.r[C3X_ST] = (cpu.r[C3X_ST] & ~C3X_ST_GIE) | (cpu_gie_before & C3X_ST_GIE);
}

int main(int argc, char **argv)
{
    const char *dir;
    uint32_t *program, *gfx, *cm;
    size_t n = 0;
    int frames, frame;
    unsigned k;

    if (argc < 2) {
        fprintf(stderr, "uso: %s <dir_generated> [frames] [pruebas_por_funcion]\n", argv[0]);
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    dir = argv[1];
    frames = argc > 2 ? atoi(argv[2]) : 600;
    per_func = argc > 3 ? atoi(argv[3]) : 2;

    program = load_file(dir, "program.bin", NULL);
    gfx = load_file(dir, "gfx.bin", NULL);
    mem_ref = malloc(sizeof(vunit_mem));
    mem_rc = malloc(sizeof(vunit_mem));
    is_start = calloc(VU_FASTRAM_WORDS, 1);
    tested = calloc(VU_FASTRAM_WORDS, sizeof(uint16_t));
    if (!program || !mem_ref || !mem_rc) {
        fprintf(stderr, "falta program.bin o memoria\n");
        return 1;
    }
    for (k = 0; k < rt_num_regions; k++)
        if (rt_regions[k].start < VU_FASTRAM_WORDS)
            is_start[rt_regions[k].start] = 1;

    vu_get_cycles = get_cycles;
    vu_get_pc = get_pc;
    vu_raise_irq = raise_irq;
    vu_reset(program, gfx);
    vu_skip_memtests();
    cm = load_file(dir, "cmos.bin", &n);
    if (cm && n == sizeof(vu.cmos))
        memcpy(vu.cmos, cm, n);
    c3x_reset(&cpu);

    for (frame = 0; frame < frames; frame++) {
        uint64_t end = (uint64_t)(frame + 1) * INSNS_PER_FRAME;
        uint64_t next_tick = cpu.cycles + 500;
        while (cpu.cycles < end) {
            uint32_t pc = cpu.pc;
            if (pc < VU_FASTRAM_WORDS && is_start[pc] && tested[pc] < per_func && !cpu.delay_count) {
                tested[pc]++;
                cpu_gie_before = cpu.r[C3X_ST];
                test_function(pc);
                continue;
            }
            c3x_run(&cpu, cpu.cycles + 1);
            if (cpu.cycles >= next_tick) {
                vu_tick();
                next_tick = cpu.cycles + 500;
            }
        }
        c3x_set_irq(&cpu, 0);
        if ((frame + 1) % 100 == 0)
            printf("frame %d: %d pruebas, %d fallos\n", frame + 1, tests, failures);
    }
    printf("total: %d pruebas, %d funciones con diferencias\n", tests, failures);
    return 0;
}
