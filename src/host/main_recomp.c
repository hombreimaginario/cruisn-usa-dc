/*
 * main_recomp.c - Ejecuta el codigo recompilado en el ordenador.
 *
 * Mismo uso y mismas variables de entorno (CUSA_INPUT, CUSA_ANALOG) que
 * cusa_host, para poder comparar fotogramas con el interprete de referencia.
 *
 * Uso: cusa_recomp <dir_generated> [frames] [cada_n_frames] [dir_salida]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../recomp/rt.h"
#include "sound_preview.h"

#define INSNS_PER_FRAME (25000000 / 57)

static const char *gdir, *outdir;
static int frames, every, frame;
static uint64_t frame_end = INSNS_PER_FRAME;
static clock_t t_start;

/* CUSA_SOUNDLOG: decodifica el protocolo del puerto de sonido (bytes con
 * estrobo 0xFDxx) y registra los codigos de 16 bits enviados. */
static int sound_log, sound_wav;
static void sound_hook(uint32_t v)
{
    if (sound_wav)
        sound_preview_byte(v);
    if (!sound_log)
        return;
    if (v & 0x100)
        printf("SND frame %d reset %u\n", frame, (unsigned)(v & 1));
    else
        printf("SND frame %d byte %02X\n", frame, (unsigned)v);
}

/* CUSA_GFXSIM=paginas: simula la cache de la ROM grafica de Dreamcast
 * (paginas de 32 KB) y cuenta lecturas de CD por segundo */
static const uint32_t *gfx_all;
static int gfxsim_pages, gfxsim_lru;
static int32_t gfxsim_tag[1024];
static uint32_t gfxsim_age[1024], gfxsim_clock, gfxsim_misses, gfxsim_reads;
static uint32_t gfxsim_fetch(uint32_t word)
{
    uint32_t page = word / 8192;
    int i, victim = 0;
    gfxsim_reads++;
    if (gfxsim_lru) {
        for (i = 0; i < gfxsim_pages; i++) {
            if (gfxsim_tag[i] == (int32_t)page) {
                gfxsim_age[i] = ++gfxsim_clock;
                return gfx_all[word];
            }
            if (gfxsim_age[i] < gfxsim_age[victim])
                victim = i;
        }
    } else {
        victim = page % gfxsim_pages;
        if (gfxsim_tag[victim] == (int32_t)page)
            return gfx_all[word];
    }
    gfxsim_tag[victim] = (int32_t)page;
    gfxsim_age[victim] = ++gfxsim_clock;
    gfxsim_misses++;
    return gfx_all[word];
}

/* CUSA_TILESIM: poligonos por frame y por tile de 32x32 del PowerVR
 * (cuenta por caja envolvente, a 640x480) */
static uint16_t tile_cnt[15][20];
static unsigned tile_max_s, polys_max_s, offscreen_s, polys_s, frames_s, tile_sum, tile_sum_max;
static void tilesim_poly(const uint32_t *d)
{
    int k, x0 = 99999, x1 = -99999, y0 = 99999, y1 = -99999, tx, ty;
    for (k = 0; k < 4; k++) {
        int x = (int)((int16_t)d[2 + 2 * k] * 1.25f), y = (int)((int16_t)d[3 + 2 * k] * 1.2f);
        if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
    }
    polys_s++;
    if (x1 < 0 || y1 < 0 || x0 >= 640 || y0 >= 480) { offscreen_s++; return; }
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > 639) x1 = 639; if (y1 > 479) y1 = 479;
    for (ty = y0 / 32; ty <= y1 / 32; ty++)
        for (tx = x0 / 32; tx <= x1 / 32; tx++)
            tile_cnt[ty][tx]++, tile_sum++;
}
static void tilesim_frame(void)
{
    int tx, ty;
    unsigned m = 0;
    for (ty = 0; ty < 15; ty++)
        for (tx = 0; tx < 20; tx++)
            if (tile_cnt[ty][tx] > m) m = tile_cnt[ty][tx];
    if (m > tile_max_s) tile_max_s = m;
    if (tile_sum > tile_sum_max) tile_sum_max = tile_sum;
    tile_sum = 0;
    memset(tile_cnt, 0, sizeof(tile_cnt));
}

static unsigned poly_ctrl_hist[256], poly_base_max, poly_pal_max;
static void stats_hook(const uint32_t *d, int page)
{
    (void)page;
    tilesim_poly(d);
    poly_ctrl_hist[(d[0] >> 8) & 0xFF]++;
    if (frame == 1299 && (int16_t)d[3] > 300 && (int16_t)d[2] > 150 && (int16_t)d[2] < 360)
        printf("  poli ctrl=%04X pal=%02X base=%04X uv=%04X %04X %04X %04X xy=%d,%d\n", (unsigned)d[0], (unsigned)(d[1] >> 8),
               (unsigned)d[14], (unsigned)d[10], (unsigned)d[11], (unsigned)d[12], (unsigned)d[13], (int16_t)d[2], (int16_t)d[3]);
    if ((d[14] & 0x7FFF) > poly_base_max) poly_base_max = d[14] & 0x7FFF;
    if ((d[1] >> 8) > poly_pal_max) poly_pal_max = d[1] >> 8;
    (void)page;
}

static uint64_t get_cycles(void) { return rt_cycles(); }
static uint32_t get_pc(void) { return 0; }
static void raise_irq(int bit)
{
    C.r[C3X_IF] |= 1u << bit;
    RT_FORCE_CHECK();
}

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

static void dump_ppm(int fr)
{
    static uint16_t rgb[512 * 400];
    char path[1024];
    FILE *f;
    int i;

    vu_video_to_rgb565(rgb, 512, vu.page_control & 1);
    snprintf(path, sizeof(path), "%s/frame_%05d.ppm", outdir, fr);
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

static void apply_inputs(int fr)
{
    const char *p = getenv("CUSA_INPUT");
    vu.in.switches = 0;
    while (p && *p) {
        int f0 = 0, dur = 0;
        unsigned bits = 0;
        if (sscanf(p, "%d:%x:%d", &f0, &bits, &dur) == 3 && fr >= f0 && fr < f0 + dur)
            vu.in.switches |= bits;
        p = strchr(p, ',');
        if (p)
            p++;
    }
    p = getenv("CUSA_ANALOG");
    while (p && *p) {
        int f0 = 0;
        unsigned w = 0, g = 0, b = 0;
        if (sscanf(p, "%d:%x:%x:%x", &f0, &w, &g, &b) == 4 && fr >= f0) {
            vu.in.wheel = (uint8_t)w;
            vu.in.gas = (uint8_t)g;
            vu.in.brake = (uint8_t)b;
        }
        p = strchr(p, ',');
        if (p)
            p++;
    }
}

#ifdef RT_TRACE_ON
/* Perfil: ciclos emulados por bloque (CUSA_PROFILE=1) */
static uint64_t *prof_cycles, prof_last_cyc;
static int prof_from;
static uint32_t prof_last_pc;
static uint32_t blk_ring[64];
static int ring_done;
static unsigned blk_pos;
static void prof_hook(uint32_t pc)
{
    blk_ring[blk_pos++ & 63] = pc;
    if (pc == 0x4AD5 || pc == 0x4B2B || (getenv("CUSA_RING_AT") && frame == atoi(getenv("CUSA_RING_AT")) && !ring_done++)) {
        unsigned k;
        printf("reinicio en frame %d (pc %06X). bloques previos:", frame, (unsigned)pc);
        for (k = 0; k < 64; k++)
            printf(" %06X", (unsigned)blk_ring[(blk_pos + k) & 63]);
        printf("\n");
    }
    uint64_t d = rt_cycles() - prof_last_cyc;
    if (d < 100000 && frame >= prof_from)   /* sin esperas activas */
        prof_cycles[prof_last_pc & 0xFFFFFF] += d;
    prof_last_cyc = rt_cycles();
    prof_last_pc = pc;
}
static void prof_report(void)
{
    int k;
    uint64_t total = 0;
    uint32_t a;
    for (a = 0; a < 0x1000000; a++)
        total += prof_cycles[a];
    printf("perfil (ciclos emulados por bloque):\n");
    for (k = 0; k < 25; k++) {
        uint32_t best = 0;
        for (a = 0; a < 0x1000000; a++)
            if (prof_cycles[a] > prof_cycles[best]) best = a;
        printf("  %06X %5.1f%%\n", (unsigned)best, 100.0 * prof_cycles[best] / total);
        prof_cycles[best] = 0;
    }
}
#endif

static unsigned long idle_calls;
void rt_platform_idle(void)
{
    idle_calls++;
    if (rt_cycles() < frame_end)
        rt_set_cycles(frame_end);
}

void rt_platform_event(void)
{
    vu_tick();
    if (rt_cycles() < frame_end)
        return;
    frame++;
    if (sound_wav)
        sound_preview_frame();
    frame_end += INSNS_PER_FRAME;
    {
        static int last_page = -1, flips, polys_acc;
        int pg = vu.page_control & 1;
        if (pg != last_page) { flips++; last_page = pg; }
        polys_acc += vu.polys_frame;
        if (getenv("CUSA_MEMDUMP") && frame % 100 == 0) {
            /* depuracion: volcado de la FASTRAM cada 100 frames */
            char path[512];
            FILE *f;
            snprintf(path, sizeof(path), "%s/mem_%05d.bin", getenv("CUSA_MEMDUMP"), frame);
            if ((f = fopen(path, "wb"))) {
                fwrite(vu.fastram, 4, VU_FASTRAM_WORDS, f);
                fwrite(vu.c31ram, 4, VU_C31_RAM_WORDS, f);
                fclose(f);
            }
        }
        if (getenv("CUSA_TILESIM")) {
            if (vu.polys_frame > polys_max_s) polys_max_s = vu.polys_frame;
            tilesim_frame();
            if (frame % 57 == 0) {
                printf("frame %d: max %u polis/frame, max %u en un tile, %u entradas tile/frame, %u de %u fuera de pantalla\n",
                       frame, polys_max_s, tile_max_s, tile_sum_max, offscreen_s, polys_s);
                tile_max_s = polys_max_s = offscreen_s = polys_s = tile_sum_max = 0;
            }
        }
        if (gfxsim_pages && frame % 57 == 0) {
            if (gfxsim_misses)
                printf("frame %d: %u lecturas de pagina de CD (%u accesos)\n", frame, gfxsim_misses, gfxsim_reads);
            gfxsim_misses = gfxsim_reads = 0;
        }
        if (getenv("CUSA_FLIPS") && frame % 57 == 0) {
            printf("frame %d flips=%d polis=%d\n", frame, flips, polys_acc);
            flips = 0;
            polys_acc = 0;
        }
    }
    if (every > 0 && frame % every == 0) {
        printf("frame %d: sp=%06X polis=%u pagina=%u  (%.1f s)\n", frame,
               (unsigned)C.r[C3X_SP], (unsigned)vu.polys_frame,
               (unsigned)(vu.page_control & 1),
               (double)(clock() - t_start) / CLOCKS_PER_SEC);
        dump_ppm(frame);
    }
    vu.polys_frame = 0;
    if (frame >= frames) {
        if (c3x_cov) {
            /* CUSA_COVERAGE: el codigo que hizo falta interpretar se suma a la
             * cobertura (como en cusa_host) para la siguiente recompilacion */
            FILE *f = fopen(getenv("CUSA_COVERAGE"), "r+b");
            uint8_t *old = calloc(0x1000000, 1);
            uint32_t a, n = 0;
            if (f) {
                if (fread(old, 1, 0x1000000, f) != 0x1000000)
                    memset(old, 0, 0x1000000);
                fclose(f);
            }
            for (a = 0; a < 0x1000000; a++) {
                n += c3x_cov[a] && !old[a];
                c3x_cov[a] |= old[a];
            }
            f = fopen(getenv("CUSA_COVERAGE"), "wb");
            if (f) {
                fwrite(c3x_cov, 1, 0x1000000, f);
                fclose(f);
            }
            printf("cobertura: %u instrucciones nuevas\n", (unsigned)n);
        }
        printf("%d frames en %.2f s (%lu esperas saltadas)\n", frames, (double)(clock() - t_start) / CLOCKS_PER_SEC, idle_calls);
        if (getenv("CUSA_POLYSTATS")) {
            int k;
            for (k = 0; k < 256; k++)
                if (poly_ctrl_hist[k])
                    printf("  ctrl %02X00: %u\n", k, poly_ctrl_hist[k]);
            printf("  base max %X, paleta max %X\n", poly_base_max, poly_pal_max);
        }
#ifdef RT_COUNT_SLOW
        {
            extern uint32_t rt_slow_count[2][256];
            int k, rw;
            for (rw = 0; rw < 2; rw++)
                for (k = 0; k < 256; k++)
                    if (rt_slow_count[rw][k] > 100000)
                        printf("  %s %02X0000: %u\n", rw ? "W" : "R", k, (unsigned)rt_slow_count[rw][k]);
        }
#endif
#ifdef RT_TRACE_ON
        if (prof_cycles)
            prof_report();
#endif
        if (sound_wav)
            sound_preview_close();
        exit(0);
    }
    apply_inputs(frame);
    raise_irq(0);
}

int main(int argc, char **argv)
{
    uint32_t *program, *gfx, *cm;
    size_t n = 0;

    if (argc < 2) {
        fprintf(stderr, "uso: %s <dir_generated> [frames] [cada_n_frames] [dir_salida]\n", argv[0]);
        return 1;
    }
    gdir = argv[1];
    frames = argc > 2 ? atoi(argv[2]) : 600;
    every = argc > 3 ? atoi(argv[3]) : 60;
    outdir = argc > 4 ? argv[4] : ".";

    program = load_file(gdir, "program.bin", NULL);
    gfx = load_file(gdir, "gfx.bin", NULL);
    if (!program) {
        fprintf(stderr, "no se puede leer %s/program.bin\n", gdir);
        return 1;
    }
    vu_get_cycles = get_cycles;
    vu_get_pc = get_pc;
    vu_raise_irq = raise_irq;
    vu_reset(program, gfx);
    if (getenv("CUSA_GFXSIM")) {
        gfxsim_pages = atoi(getenv("CUSA_GFXSIM"));
        gfxsim_lru = getenv("CUSA_GFXSIM_LRU") != NULL;
        memset(gfxsim_tag, 0xFF, sizeof(gfxsim_tag));
        gfx_all = gfx;
        vu.gfx = NULL;
        vu_gfx_fetch = gfxsim_fetch;
    }
    vu_skip_memtests();
    cm = load_file(gdir, "cmos.bin", &n);
    if (cm && n == sizeof(vu.cmos))
        memcpy(vu.cmos, cm, n);
    free(cm);

    if (getenv("CUSA_POLYSTATS") || getenv("CUSA_TILESIM"))
        vu_poly_hook = stats_hook;
    /* CUSA_SOUNDLOG: registra los bytes; CUSA_SOUNDWAV=fichero.wav: mezcla lo
     * que sonaria en Dreamcast con generated/sound.bin */
    sound_log = getenv("CUSA_SOUNDLOG") != NULL;
    if (getenv("CUSA_SOUNDWAV")) {
        char path[512];
        snprintf(path, sizeof(path), "%s/sound.bin", gdir);
        sound_wav = sound_preview_init(path, getenv("CUSA_SOUNDWAV")) == 0;
    }
    if (sound_log || sound_wav)
        vu_sound_hook = sound_hook;
    if (getenv("CUSA_COVERAGE"))
        c3x_cov = calloc(0x1000000, 1);
    rt_reset();
#ifdef RT_TRACE_ON
    prof_cycles = calloc(0x1000000, sizeof(uint64_t));
    prof_from = getenv("CUSA_PROF_FROM") ? atoi(getenv("CUSA_PROF_FROM")) : 0;
    rt_trace_hook = prof_hook;
#endif
    apply_inputs(0);
    t_start = clock();
    rt_run();
    return 0;
}
