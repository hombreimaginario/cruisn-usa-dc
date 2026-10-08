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
#include "sound_dc.h"
#include "cmos_vmu.h"
#include "watchdog.h"
#include "textfont.h"
#include "vmu_lcd.h"
#include "rumble.h"
#include "version.h"

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
static uint32_t gfx_cache[GFX_PAGES][GFX_PAGE_WORDS] __attribute__((aligned(32)));
static int32_t gfx_tag[GFX_PAGES];

static uint32_t gfx_fetch(uint32_t word)
{
    uint32_t page = word / GFX_PAGE_WORDS;
    uint32_t slot = page % GFX_PAGES;

    if (gfx_tag[slot] != (int32_t)page) {
        const char *prev = (const char *)wd_phase;
        wd_phase = "lectura de CD (ROM grafica)";
        wd_counters[WD_CD_GFX]++;
        {
            /* si la lectura falla se reintenta (en consola real el lector
             * puede devolver error puntual); datos a cero harian que el
             * descompresor del juego se perdiera */
            int tries;
            for (tries = 0; tries < 4; tries++) {
                fs_seek(gfx_file, (off_t)page * GFX_PAGE_WORDS * 4, SEEK_SET);
                if (fs_read(gfx_file, gfx_cache[slot], GFX_PAGE_WORDS * 4) == GFX_PAGE_WORDS * 4)
                    break;
                wd_counters[WD_CD_ERR]++;
            }
            if (tries == 4)
                memset(gfx_cache[slot], 0, sizeof(gfx_cache[slot]));
        }
        gfx_tag[slot] = (int32_t)page;
        wd_phase = prev;
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
    /* buffer alineado a 32: el lector de CD de KOS usa DMA y es unas 100
     * veces mas rapido (sin alinear, 2 MB tardaban 17 s) */
    buf = memalign(32, size);
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
    /*
     * Mando de Dreamcast (y teclado de Flycast con su mapeo por defecto):
     *   stick o cruceta izquierda/derecha  volante
     *   gatillo R o A                      acelerador
     *   gatillo L o B                      freno
     *   cruceta arriba / abajo             subir / bajar marcha (cambio manual)
     *   Y                                  cambiar de vista
     *   X                                  cambiar de emisora
     *   START                              empezar;  START+Y: servicio
     */
    if ((b & CONT_START) && (b & CONT_Y))
        sw |= SW_DIAG;
    else if (b & CONT_START)
        sw |= SW_START;
    if ((b & CONT_DPAD_UP) && !(prev & CONT_DPAD_UP) && gear < 3)
        gear++;
    if ((b & CONT_DPAD_DOWN) && !(prev & CONT_DPAD_DOWN) && gear > 0)
        gear--;
    sw |= gears[gear];
    {
        /* las vistas son tres botones en el arcade; Y las recorre */
        static const uint32_t views[3] = { SW_VIEW1, SW_VIEW2, SW_VIEW3 };
        static int view, view_hold;
        if ((b & CONT_Y) && !(prev & CONT_Y) && !(b & CONT_START)) {
            view = (view + 1) % 3;
            view_hold = 6;
        }
        if (view_hold) {
            view_hold--;
            sw |= views[view];
        }
    }
    if (b & CONT_X)
        sw |= SW_RADIO;
    prev = b;

    vu.in.switches = sw;
    {
        /* volante: el stick si se mueve; si no, la cruceta con un giro
         * progresivo (la CMOS esta calibrada de 0x10 a 0xF0) */
        static int dwheel = 0x80;
        int target = 0x80;
        if (b & CONT_DPAD_LEFT)
            target = 0x10;
        else if (b & CONT_DPAD_RIGHT)
            target = 0xF0;
        if (dwheel < target)
            dwheel = dwheel + 12 > target ? target : dwheel + 12;
        else if (dwheel > target)
            dwheel = dwheel - 12 < target ? target : dwheel - 12;
        if (st->joyx < -8 || st->joyx > 8)
            vu.in.wheel = (uint8_t)(0x80 + st->joyx);   /* joyx: -128..127 */
        else
            vu.in.wheel = (uint8_t)dwheel;
    }
    vu.in.gas = (b & CONT_A) ? 0xFF : (uint8_t)st->rtrig;
    vu.in.brake = (b & CONT_B) ? 0xFF : (uint8_t)st->ltrig;
#ifdef CUSA_AUTOPLAY
    {
        /* Prueba sin mando: moneda, START y una carrera acelerando (igual que
         * el guion CUSA_INPUT/CUSA_ANALOG del ordenador). */
        extern int cusa_frame;
        int f = cusa_frame;
        sw = gears[gear];
        if (f == 1200 || f == 1220 || f == 1240)
            sw |= SW_COIN1;
        if (f >= 1300 && f <= 2300 && (f - 1300) % 200 < 5)
            sw |= SW_START;
        vu.in.switches = sw;
        vu.in.wheel = 0x80;
        vu.in.gas = f >= 2400 ? 0xE0 : 0;
        vu.in.brake = 0;
        if (f >= 2600)                  /* zigzag entre el trafico */
            vu.in.wheel = (f / 150) % 2 ? 0x50 : 0xB0;
    }
#endif
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
int cusa_frame;
static uint64_t t0;
#if defined(CUSA_FIXED_CYCLES) || defined(CUSA_NO_THROTTLE)
static uint64_t frame_end = INSNS_PER_FRAME;
#endif

static uint64_t rc_cycles(void) { return rt_cycles(); }
static uint32_t rc_pc(void) { return 0; }
static void rc_irq(int bit)
{
    C.r[C3X_IF] |= 1u << bit;
    RT_FORCE_CHECK();
}

/* Lo que pasa en cada interrupcion de video (INT0, 57 por segundo) */
static void vblank(void)
{
    cusa_frame = ++frame;
    wd_heartbeat++;
    wd_phase = "render";
#ifdef CUSA_WD_TEST
    if (frame == 600) {
        wd_phase = "prueba de cuelgue";
        for (;;)
            ;
    }
#endif
#if defined(CUSA_SHOT) && defined(CUSA_SHOT_EVERY)
    pvrr_frame(frame >= CUSA_SHOT && (frame - CUSA_SHOT) % CUSA_SHOT_EVERY == 0);
#elif defined(CUSA_SHOT)
    pvrr_frame(frame == CUSA_SHOT);
#else
    pvrr_frame(0);
#endif
    read_inputs();
    sound_frame();
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
#ifdef CUSA_CMOS_TEST
        if (frame == 570)
            vu.cmos[7] ^= 0x5A;         /* fuerza un guardado (prueba) */
#endif
        cmos_check();
        unsigned conv, drawn, pages = pvrr_pages();
        pvrr_stats(&conv, &drawn);
        printf("frame %d polis=%u dibujados=%u texturas=%u imagenes=%u  %u ms por segundo de juego\n",
               frame, (unsigned)vu.polys_frame, drawn, conv, pages, (unsigned)(t - t0));
        t0 = t;
    }
    vu.polys_frame = 0;
    wd_phase = "juego";
    rc_irq(0);
}

#if defined(CUSA_FIXED_CYCLES) || defined(CUSA_NO_THROTTLE)
/*
 * Interrupcion de video cada INSNS_PER_FRAME ciclos emulados: reproducible
 * (perfiles, capturas en un frame concreto), pero si el SH-4 no llega el
 * juego entero va a camara lenta.
 */
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
#ifndef CUSA_NO_THROTTLE
    {
        static uint64_t next_us;
        uint64_t now = timer_us_gettime64();
        next_us += 1000000 / 57;
        if (next_us + 100000 < now)
            next_us = now;
        while (timer_us_gettime64() < next_us)
            thd_pass();
    }
#endif
    vblank();
}
#else
/*
 * Interrupcion de video a 57 Hz de tiempo real, como en la placa: si el
 * SH-4 no termina un frame del juego a tiempo, el juego ve pasar mas
 * interrupciones (NFRAMES) y mueve todo en proporcion, igual que el original
 * cuando una escena le cuesta. Se pierden imagenes por segundo pero el juego
 * va a su velocidad.
 */
#define VBL_US 17544                    /* 1/57 s */
static uint64_t next_vbl_us;

/*
 * Como mucho NFRAMES_MAX interrupciones por frame del juego (el arcade va a
 * 2 en las escenas cargadas). Con mas, cada frame mueve los coches mas
 * distancia y los choques de frente o contra conos se pueden saltar. Si el
 * SH-4 no llega, la interrupcion que sobra espera a que el juego termine su
 * frame: el juego va algo mas lento en vez de saltarse colisiones. Con 2 el
 * juego iba al 55-90 % de velocidad en carretera; con 3 va casi siempre a
 * tiempo real (en la carrera de prueba, 22-28 imagenes por segundo).
 */
#define NFRAMES_MAX 3
static int vbl_since_flip, last_flip_page = -1;

static void vblank_due(uint64_t now)
{
    next_vbl_us += VBL_US;
    if (now > next_vbl_us)
        next_vbl_us = now + VBL_US;     /* atrasados: no acumular */
    vblank();
    if ((vu.page_control & 1) != last_flip_page) {
        last_flip_page = vu.page_control & 1;
        vbl_since_flip = 0;
    } else {
        vbl_since_flip++;
    }
}

void rt_platform_idle(void)
{
    uint64_t now;

    /* Esperando la conversion del ADC: no hace falta esperar a la imagen */
    if (vu.adc_irq_delay) {
        while (vu.adc_irq_delay)
            vu_tick();
        return;
    }
    /* El juego espera a la siguiente interrupcion de video */
    now = timer_us_gettime64();
    if (!next_vbl_us)
        next_vbl_us = now + VBL_US;
    while (now < next_vbl_us) {
        thd_pass();
        now = timer_us_gettime64();
    }
    vblank_due(now);
}

void rt_platform_event(void)
{
    uint64_t now;

    vu_tick();
    now = timer_us_gettime64();
    if (!next_vbl_us)
        next_vbl_us = now + VBL_US;
    if (now < next_vbl_us)
        return;
    /* con el juego ocupado, la ultima interrupcion permitida se guarda hasta
     * que espere (salvo que lleve mucho sin esperar: bucles no detectados) */
    if (vbl_since_flip >= NFRAMES_MAX - 1 && now < next_vbl_us + 4 * VBL_US)
        return;
    vblank_due(now);
}
#endif
#endif

static uint64_t t_boot;
static int boot_line;

/* Pasos del arranque en la pantalla de carga: si algo se cuelga antes de
 * que el PowerVR tome la pantalla, en la consola se ve donde. */
static void boot_step(const char *s)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%-30s", s);
    tf_draw((uint16_t *)vram_s + (290 + boot_line * 26) * 640 + 120, 640, buf, 0xFFFF, 0x0000);
    if (boot_line < 5)
        boot_line++;
    printf("arranque: %s (%u ms)\n", s, (unsigned)(timer_ms_gettime64() - t_boot));
}

static void boot_fail(const char *s)
{
    boot_step(s);
    for (;;)
        thd_sleep(1000);
}

#ifdef CUSA_RECOMP
static void count_interp(void)
{
    wd_counters[WD_INTERP]++;
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
    tf_init();                          /* antes de cualquier lectura del CD */
    /* Pantalla de carga: leer el programa, el sonido y arrancar el juego
     * lleva unos segundos y en negro parece colgado. */
    tf_draw((uint16_t *)vram_s + 220 * 640 + 236, 640, "CRUIS'N USA", 0xFFFF, 0x0000);
    tf_draw((uint16_t *)vram_s + 250 * 640 + 248, 640, "Cargando...", 0xFFFF, 0x0000);
#ifdef CUSA_VERSION
    tf_draw((uint16_t *)vram_s + 440 * 640 + 24, 640, "version " CUSA_VERSION, 0xFFFF, 0x0000);
#endif
    t_boot = timer_ms_gettime64();
#ifdef CUSA_RECOMP
    printf("Cruis'n USA DC - codigo recompilado\n");
#else
    printf("Cruis'n USA DC - interprete de referencia\n");
#endif

    boot_step("leyendo programa");
    program = load_file("/cd/program.bin", &n);
    if (!program || n != VU_PROGRAM_WORDS * 4)
        boot_fail(program ? "program.bin: tamano incorrecto" : "no se pudo leer program.bin");
    gfx_file = fs_open("/cd/gfx.bin", O_RDONLY);
    if (gfx_file == FILEHND_INVALID)
        printf("aviso: falta /cd/gfx.bin, no habra texturas\n");
    memset(gfx_tag, 0xFF, sizeof(gfx_tag));

    vu_gfx_fetch = gfx_file != FILEHND_INVALID ? gfx_fetch : NULL;
    vu_reset(program, NULL);

    boot_step("leyendo CMOS / VMU");
    cmos = load_file("/cd/cmos.bin", &n);
    cmos_init(cmos, cmos ? n : 0);
    free(cmos);

#ifdef CUSA_RECOMP
    /* el sonido antes que el PowerVR: mientras no se inicia el PowerVR la
     * pantalla de carga sigue visible */
    boot_step("cargando sonido");
    if (sound_init("/cd/sound.bin") == 0)
        vu_sound_hook = sound_dcs_write;
    boot_step("iniciando PowerVR");
    pvrr_init();
#ifdef CUSA_SHOT
    pvrr_reserve_shot();
#endif
    vu_get_cycles = rc_cycles;
    vu_get_pc = rc_pc;
    vu_raise_irq = rc_irq;
    vu_skip_memtests();
    rt_reset();
#ifdef CUSA_PROF
    prof_start();
#endif
    t0 = timer_ms_gettime64();
#ifndef CUSA_PROF
    watchdog_start();
#endif
    rt_interp_hook = count_interp;
#ifndef CUSA_MINIMAL
    vmu_lcd_start();
    rumble_start();
#endif
    wd_phase = "juego";
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
