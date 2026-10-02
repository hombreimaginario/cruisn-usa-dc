/*
 * main_host.c - Ejecuta el juego en el ordenador (macOS/Linux) sin Dreamcast.
 *
 * Sirve para depurar el arranque, el hardware emulado y, mas adelante, el
 * codigo recompilado. Vuelca la pagina visible del framebuffer a ficheros
 * PPM cada cierto numero de frames.
 *
 * Uso: cusa_host <dir_generated> [frames] [cada_n_frames] [dir_salida]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../c3x/c3x.h"
#include "../vunit/mem.h"

#define INSNS_PER_FRAME (25000000 / 57)
#define TICK 500

static c3x_state cpu;
static uint32_t *pc_hist;      /* CUSA_PCHIST: muestreo de PC */

/* Entradas programadas: CUSA_INPUT="frame:bits:duracion,..." (bits en hex,
 * ver SW_* en VUNIT.EQU). Ej.: "3100:10:10" pulsa DIAG en el frame 3100. */
static void apply_script(const char *script, int frame)
{
    const char *p = script;
    vu.in.switches = 0;
    while (p && *p) {
        int f = 0, dur = 0;
        unsigned bits = 0;
        if (sscanf(p, "%d:%x:%d", &f, &bits, &dur) == 3 && frame >= f && frame < f + dur)
            vu.in.switches |= bits;
        p = strchr(p, ',');
        if (p)
            p++;
    }
}

static void save_cmos(const char *dir)
{
    char path[1024];
    FILE *f;
    snprintf(path, sizeof(path), "%s/cmos.bin", dir);
    f = fopen(path, "wb");
    if (f) {
        fwrite(vu.cmos, sizeof(vu.cmos), 1, f);
        fclose(f);
    }
}

static void dump_regs(c3x_state *c)
{
    int k;
    printf("pc=%06X ciclo=%llu", (unsigned)c->pc, (unsigned long long)c->cycles);
    for (k = 0; k < C3X_NUM_REGS; k++)
        printf("%s%02d=%08X", k % 8 ? " " : "\n  ", k, (unsigned)c->r[k]);
    printf("\n");
}

static int break_hits, break_max = 8;
static void on_break(c3x_state *c)
{
    /* mismo formato que tools/mame/bp.lua para poder hacer diff */
    static const int order[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                                 16, 17, 18, 19, 20, 21, 22, 23, 25, 26, 27 };
    static const char *names[] = { "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
        "AR0", "AR1", "AR2", "AR3", "AR4", "AR5", "AR6", "AR7", "DP", "IR0", "IR1",
        "BK", "SP", "ST", "IE", "IF", "RS", "RE", "RC" };
    size_t k;
    printf("pc=%06X", (unsigned)c->pc);
    for (k = 0; k < sizeof(order) / sizeof(order[0]); k++)
        printf(" %s=%08X", names[k], (unsigned)c->r[order[k]]);
    printf("\n");
    if (getenv("CUSA_RING")) {
        unsigned i;
        printf("  ultimos PC:");
        for (i = 0; i < 64; i++)
            printf(" %06X", (unsigned)c3x_pc_ring[(c3x_pc_ring_pos + i) & 63]);
        printf("\n");
    }
    if (++break_hits >= break_max)
        exit(0);
}

static uint64_t get_cycles(void) { return cpu.cycles; }
static uint32_t get_pc(void) { return cpu.pc; }
static void raise_irq(int bit) { c3x_set_irq(&cpu, bit); }

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

static void dump_ppm(const char *dir, int frame)
{
    static uint16_t rgb[512 * 400];
    char path[1024];
    FILE *f;
    int i;

    vu_video_to_rgb565(rgb, 512, vu.page_control & 1);
    snprintf(path, sizeof(path), "%s/frame_%05d.ppm", dir, frame);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n512 400\n255\n");
    for (i = 0; i < 512 * 400; i++) {
        uint16_t c = rgb[i];
        unsigned char p[3];
        p[0] = (unsigned char)(((c >> 11) & 0x1F) << 3);
        p[1] = (unsigned char)(((c >> 5) & 0x3F) << 2);
        p[2] = (unsigned char)((c & 0x1F) << 3);
        fwrite(p, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *dir, *outdir;
    uint32_t *program, *gfx;
    int frames, every, frame;
    uint64_t next;

    if (argc < 2) {
        fprintf(stderr, "uso: %s <dir_generated> [frames] [cada_n_frames] [dir_salida]\n", argv[0]);
        return 1;
    }
    dir = argv[1];
    frames = argc > 2 ? atoi(argv[2]) : 600;
    every = argc > 3 ? atoi(argv[3]) : 60;
    outdir = argc > 4 ? argv[4] : ".";

    program = load_file(dir, "program.bin", NULL);
    gfx = load_file(dir, "gfx.bin", NULL);
    if (!program) {
        fprintf(stderr, "no se puede leer %s/program.bin (genera con tools/romtool.py)\n", dir);
        return 1;
    }
    if (!gfx)
        fprintf(stderr, "aviso: sin gfx.bin, las texturas saldran vacias\n");

    vu_get_cycles = get_cycles;
    vu_get_pc = get_pc;
    vu_raise_irq = raise_irq;
    vu.trace_unmapped = getenv("CUSA_TRACE") != NULL;

    if (getenv("CUSA_PCHIST"))
        pc_hist = calloc(0x1000000, sizeof(uint32_t));
    vu_reset(program, gfx);
    vu.trace_unmapped = getenv("CUSA_TRACE") != NULL;
    {
        size_t n = 0;
        uint32_t *cm = load_file(dir, "cmos.bin", &n);
        if (cm && n == sizeof(vu.cmos)) {
            memcpy(vu.cmos, cm, n);
            printf("CMOS cargada de %s/cmos.bin\n", dir);
        }
        free(cm);
    }
    c3x_reset(&cpu);
    if (getenv("CUSA_WATCH"))
        vu_watch_addr = (uint32_t)strtoul(getenv("CUSA_WATCH"), NULL, 16);
    if (getenv("CUSA_BREAK")) {
        c3x_break_pc = (uint32_t)strtoul(getenv("CUSA_BREAK"), NULL, 16);
        c3x_break_cb = on_break;
        if (getenv("CUSA_BREAKN"))
            break_max = atoi(getenv("CUSA_BREAKN"));
    }
    printf("reset: pc=%06X\n", (unsigned)cpu.pc);

    next = 0;
    for (frame = 0; frame < frames; frame++) {
        uint64_t end = (uint64_t)(frame + 1) * INSNS_PER_FRAME;
        vu.polys_frame = 0;
        if (getenv("CUSA_INPUT"))
            apply_script(getenv("CUSA_INPUT"), frame);
        while (cpu.cycles < end) {
            next = cpu.cycles + TICK;
            if (next > end)
                next = end;
            c3x_run(&cpu, next);
            vu_tick();
            if (pc_hist)
                pc_hist[cpu.pc & 0xFFFFFF]++;
        }
        c3x_set_irq(&cpu, 0);   /* INT0: interrupcion de video a 57 Hz */

        if (every > 0 && (frame + 1) % every == 0) {
            printf("frame %d: pc=%06X sp=%06X st=%04X ie=%03X polis=%u pagina=%u\n",
                   frame + 1, (unsigned)cpu.pc, (unsigned)cpu.r[C3X_SP],
                   (unsigned)cpu.r[C3X_ST], (unsigned)cpu.r[C3X_IE],
                   (unsigned)vu.polys_frame, (unsigned)(vu.page_control & 1));
            dump_ppm(outdir, frame + 1);
        }
    }

    if (pc_hist) {
        int k;
        printf("PC mas frecuentes:\n");
        for (k = 0; k < 15; k++) {
            uint32_t best = 0, a, bestn = 0;
            for (a = 0; a < 0x1000000; a++)
                if (pc_hist[a] > bestn) { bestn = pc_hist[a]; best = a; }
            if (!bestn) break;
            printf("  %06X x%u\n", (unsigned)best, (unsigned)bestn);
            pc_hist[best] = 0;
        }
    }
    dump_regs(&cpu);
    save_cmos(dir);
    printf("accesos sin mapear:\n");
    vu_report_unmapped();
    return 0;
}
