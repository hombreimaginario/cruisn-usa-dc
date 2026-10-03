/*
 * main_dc.c - Punto de entrada para Dreamcast (KallistiOS).
 *
 * Ejecuta el codigo del arcade con el interprete de referencia y el motor de
 * poligonos por software, y copia la pagina visible al framebuffer. Es la
 * primera version funcional en consola: correcta pero lenta. El recompilador
 * y el render por PowerVR (docs/PLAN.md) la sustituiran.
 *
 * Ficheros en el CD (generados con tools/romtool.py a partir de tu ROM):
 *   /cd/program.bin   imagen de programa (2 MB, se carga en RAM)
 *   /cd/gfx.bin       ROM grafica (8 MB, se lee bajo demanda con cache)
 *   /cd/cmos.bin      opcional: CMOS ya calibrada (generated/cmos.bin)
 */
#include <kos.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/video.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../c3x/c3x.h"
#include "../vunit/mem.h"
#include "pvr_render.h"

#define INSNS_PER_FRAME (25000000 / 57)
#define TICK 500

static c3x_state cpu;

static uint64_t get_cycles(void) { return cpu.cycles; }
static uint32_t get_pc(void) { return cpu.pc; }
static void raise_irq(int bit) { c3x_set_irq(&cpu, bit); }

/* ---- ROM grafica bajo demanda ---- */

#define GFX_PAGE_WORDS 8192          /* 32 KB por pagina */
#define GFX_PAGES      32            /* 1 MB de cache */

static file_t gfx_file = FILEHND_INVALID;
static uint32_t gfx_cache[GFX_PAGES][GFX_PAGE_WORDS];
static int32_t gfx_tag[GFX_PAGES];

static uint32_t gfx_fetch(uint32_t word)
{
    uint32_t page = word / GFX_PAGE_WORDS;
    uint32_t slot = page % GFX_PAGES;

    if (gfx_tag[slot] != (int32_t)page) {
        fs_seek(gfx_file, (off_t)page * GFX_PAGE_WORDS * 4, SEEK_SET);
        if (fs_read(gfx_file, gfx_cache[slot], GFX_PAGE_WORDS * 4) != GFX_PAGE_WORDS * 4)
            memset(gfx_cache[slot], 0, sizeof(gfx_cache[slot]));
        gfx_tag[slot] = (int32_t)page;
    }
    return gfx_cache[slot][word % GFX_PAGE_WORDS];
}

static void *load_file(const char *path, size_t *size_out)
{
    file_t f = fs_open(path, O_RDONLY);
    size_t size;
    void *buf;

    if (f == FILEHND_INVALID)
        return NULL;
    size = fs_total(f);
    buf = malloc(size);
    if (buf && fs_read(f, buf, size) != (ssize_t)size) {
        free(buf);
        buf = NULL;
    }
    fs_close(f);
    if (size_out)
        *size_out = size;
    return buf;
}

/* ---- entradas ---- */

/* Bits de SW_* en VUNIT.EQU */
#define SW_COIN1   0x00000001u
#define SW_START   0x00000004u
#define SW_DIAG    0x00000010u
#define SW_4TH     0x00000400u
#define SW_3RD     0x00000800u
#define SW_2ND     0x00001000u
#define SW_1ST     0x00002000u
#define SW_VIEW1   0x00200000u
#define SW_VIEW2   0x00400000u
#define SW_VIEW3   0x00800000u
#define SW_RADIO   0x00020000u

static int gear;

static void read_inputs(void)
{
    static uint32_t prev;
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t *st;
    uint32_t b, sw = 0;
    static const uint32_t gears[4] = { SW_1ST, SW_2ND, SW_3RD, SW_4TH };

    if (!dev || !(st = (cont_state_t *)maple_dev_status(dev)))
        return;
    b = st->buttons;
    if ((b & CONT_START) && (b & CONT_Y))
        sw |= SW_DIAG;                       /* START+Y: boton de servicio */
    else if (b & CONT_START)
        sw |= SW_START;
    if ((b & CONT_Y) && !(b & CONT_START))
        sw |= SW_COIN1;
    if ((b & CONT_A) && !(prev & CONT_A) && gear < 3)
        gear++;
    if ((b & CONT_B) && !(prev & CONT_B) && gear > 0)
        gear--;
    sw |= gears[gear];
    if (b & CONT_DPAD_LEFT)  sw |= SW_VIEW1;
    if (b & CONT_DPAD_UP)    sw |= SW_VIEW2;
    if (b & CONT_DPAD_RIGHT) sw |= SW_VIEW3;
    if (b & CONT_X)          sw |= SW_RADIO;
    prev = b;

    vu.in.switches = sw;
    vu.in.wheel = (uint8_t)(0x80 + st->joyx);   /* joyx: -128..127 */
    vu.in.gas = (uint8_t)st->rtrig;
    vu.in.brake = (uint8_t)st->ltrig;
}

/* ---- video ---- */

static uint16_t line_rgb[512 * 400];

static void present(void)
{
    int y;
    vu_video_to_rgb565(line_rgb, 512, vu.page_control & 1);
    for (y = 0; y < 400; y++)
        memcpy(vram_s + (y + 40) * 640 + 64, line_rgb + y * 512, 512 * 2);
}

/* ---- perfil por muestreo (compilar con -DCUSA_PROF) ---- */

#ifdef CUSA_PROF
#define PROF_BUCKETS 65536
static uint32_t prof_hist[PROF_BUCKETS];

static void prof_tick(irq_t code, irq_context_t *ctx, void *data)
{
    uint32_t i = (ctx->pc - 0x8c010000u) >> 6;
    (void)code; (void)data;
    timer_clear(TMU1);
    if (i < PROF_BUCKETS)
        prof_hist[i]++;
}

static void prof_start(void)
{
    irq_set_handler(EXC_TMU1_TUNI1, prof_tick, NULL);
    timer_prime(TMU1, 2000, 1);
    timer_start(TMU1);
    timer_enable_ints(TMU1);
}

static void prof_report(void)
{
    int k;
    uint32_t total = 0, i;
    timer_stop(TMU1);
    for (i = 0; i < PROF_BUCKETS; i++)
        total += prof_hist[i];
    printf("PERFIL %u muestras\n", (unsigned)total);
    for (k = 0; k < 400; k++) {
        uint32_t best = 0;
        for (i = 0; i < PROF_BUCKETS; i++)
            if (prof_hist[i] > prof_hist[best])
                best = i;
        if (!prof_hist[best])
            break;
        printf("PERFIL %08X %u\n", (unsigned)(0x8c010000u + (best << 6)), (unsigned)prof_hist[best]);
        prof_hist[best] = 0;
    }
}
#endif

/* ---- bucle principal ---- */

#ifdef CUSA_RECOMP
#include "../recomp/rt.h"

static int frame;
static uint64_t frame_end = INSNS_PER_FRAME, t0;

static uint64_t rc_cycles(void) { return rt_cycles(); }
static uint32_t rc_pc(void) { return 0; }
static void rc_irq(int bit)
{
    C.r[C3X_IF] |= 1u << bit;
    RT_FORCE_CHECK();
}

void rt_platform_idle(void)
{
    if (rt_cycles() < frame_end)
        rt_set_cycles(frame_end);
}

void rt_platform_event(void)
{
    vu_tick();
    if (rt_cycles() < frame_end)
        return;
    frame_end += INSNS_PER_FRAME;
    frame++;
#if defined(CUSA_SHOT) && defined(CUSA_SHOT_EVERY)
    pvrr_frame(frame >= CUSA_SHOT && (frame - CUSA_SHOT) % CUSA_SHOT_EVERY == 0);
#elif defined(CUSA_SHOT)
    pvrr_frame(frame == CUSA_SHOT);
#else
    pvrr_frame(0);
#endif
    read_inputs();
#ifdef CUSA_PROF
#ifndef PROF_FROM
#define PROF_FROM 1000
#define PROF_TO 1400
#endif
    if (frame == PROF_FROM)
        memset(prof_hist, 0, sizeof(prof_hist));
    if (frame == PROF_TO)
        prof_report();
#endif
    if (frame % 57 == 0) {
        uint64_t t = timer_ms_gettime64();
        unsigned conv, drawn;
        pvrr_stats(&conv, &drawn);
        printf("frame %d polis=%u dibujados=%u texturas=%u  %u ms por segundo de juego\n",
               frame, (unsigned)vu.polys_frame, drawn, conv, (unsigned)(t - t0));
        t0 = t;
    }
    vu.polys_frame = 0;
    rc_irq(0);
}
#endif

int main(int argc, char **argv)
{
    uint32_t *program;
    uint32_t *cmos;
    size_t n;

    (void)argc; (void)argv;

    vid_set_mode(DM_640x480, PM_RGB565);
    vid_clear(0, 0, 0);
#ifdef CUSA_RECOMP
    printf("Cruis'n USA DC - codigo recompilado\n");
#else
    printf("Cruis'n USA DC - interprete de referencia\n");
#endif

    program = load_file("/cd/program.bin", &n);
    if (!program || n != VU_PROGRAM_WORDS * 4) {
        printf("falta /cd/program.bin (genera con tools/romtool.py)\n");
        return 1;
    }
    gfx_file = fs_open("/cd/gfx.bin", O_RDONLY);
    if (gfx_file == FILEHND_INVALID)
        printf("aviso: falta /cd/gfx.bin, no habra texturas\n");
    memset(gfx_tag, 0xFF, sizeof(gfx_tag));

    vu_gfx_fetch = gfx_file != FILEHND_INVALID ? gfx_fetch : NULL;
    vu_reset(program, NULL);

    cmos = load_file("/cd/cmos.bin", &n);
    if (cmos && n == sizeof(vu.cmos)) {
        memcpy(vu.cmos, cmos, n);
        printf("CMOS calibrada cargada\n");
    }
    free(cmos);

#ifdef CUSA_RECOMP
    pvrr_init();
    vu_get_cycles = rc_cycles;
    vu_get_pc = rc_pc;
    vu_raise_irq = rc_irq;
    vu_skip_memtests();
    rt_reset();
#ifdef CUSA_PROF
    prof_start();
#endif
    t0 = timer_ms_gettime64();
    rt_run();
#else
    vu_get_cycles = get_cycles;
    vu_get_pc = get_pc;
    vu_raise_irq = raise_irq;
    c3x_reset(&cpu);
    {
        int frame;
        uint64_t t0 = timer_ms_gettime64();
        for (frame = 0;; frame++) {
            uint64_t end = (uint64_t)(frame + 1) * INSNS_PER_FRAME;
            read_inputs();
            while (cpu.cycles < end) {
                uint64_t next = cpu.cycles + TICK;
                c3x_run(&cpu, next < end ? next : end);
                vu_tick();
            }
            c3x_set_irq(&cpu, 0);
            present();
            if ((frame + 1) % 57 == 0) {
                uint64_t t = timer_ms_gettime64();
                printf("frame %d pc=%06X polis=%u  %u ms por segundo de juego\n",
                       frame + 1, (unsigned)cpu.pc, (unsigned)vu.polys_frame,
                       (unsigned)(t - t0));
                t0 = t;
            }
        }
    }
#endif
    return 0;
}
