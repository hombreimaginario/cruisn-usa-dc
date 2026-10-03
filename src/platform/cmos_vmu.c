/*
 * cmos_vmu.c - La CMOS del arcade (ajustes, records, contabilidad) en la VMU.
 *
 * Al arrancar se busca CRUISNUS en cualquier VMU; si no esta se usa la CMOS
 * calibrada del CD. Despues, cada pocos segundos se mira si el juego ha
 * cambiado la CMOS y, cuando lleva un rato sin cambiar, se guarda en un hilo
 * aparte (escribir en la VMU tarda y no debe parar el juego).
 */
#include <kos.h>
#include <dc/maple.h>
#include <dc/maple/vmu.h>
#include <dc/vmu_pkg.h>
#include <dc/fs_vmu.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../vunit/mem.h"
#include "cmos_vmu.h"

#define FILE_NAME "CRUISNUS"
#define CMOS_BYTES (VU_CMOS_WORDS * 4)

static char vmu_path[32];
static int quiet_checks;
static uint32_t save_buf[VU_CMOS_WORDS];
static semaphore_t save_sem;
static volatile int saving, save_result;     /* 1 = ok, -1 = error */

/*
 * El juego lleva en la CMOS un contador de tiempo encendido que cambia cada
 * segundo. Para no escribir la VMU sin parar, las palabras "calientes" (que
 * cambian en casi todas las comprobaciones) no cuentan para decidir si hay
 * algo nuevo que guardar; se guardan igualmente cuando cambia otra cosa
 * (records, ajustes, contadores de partidas).
 */
static uint32_t prev_cmos[VU_CMOS_WORDS];
static uint8_t heat[VU_CMOS_WORDS];
#define HOT 4

/* Icono de 32x32 a 4 bits: franjas rojas y blancas con un cuadro azul */
static void make_icon(vmu_pkg_t *pkg, uint8_t *icon)
{
    int x, y;
    pkg->icon_pal[0] = 0xFFFF;              /* blanco */
    pkg->icon_pal[1] = 0xFD22;              /* rojo */
    pkg->icon_pal[2] = 0xF22C;              /* azul */
    pkg->icon_pal[3] = 0xF000;              /* negro */
    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x += 2) {
            int c[2], k;
            for (k = 0; k < 2; k++) {
                int px = x + k;
                if (px == 0 || px == 31 || y == 0 || y == 31)
                    c[k] = 3;
                else if (px < 14 && y < 14)
                    c[k] = 2;
                else
                    c[k] = (y / 4) & 1;
            }
            icon[y * 16 + x / 2] = (uint8_t)(c[0] << 4 | c[1]);
        }
    pkg->icon_cnt = 1;
    pkg->icon_anim_speed = 0;
    pkg->icon_data = icon;
}

/* fs_vmu pone y quita la cabecera (icono, descripcion) por su cuenta */
static int write_vmu(const uint32_t *data)
{
    vmu_pkg_t pkg;
    static uint8_t icon[512];
    int ok = 0;
    file_t f;

    memset(&pkg, 0, sizeof(pkg));
    strcpy(pkg.desc_short, "CRUISN USA");
    strcpy(pkg.desc_long, "Ajustes y records (CMOS)");
    strcpy(pkg.app_id, "CRUISNUSADC");
    make_icon(&pkg, icon);
    f = fs_open(vmu_path, O_WRONLY | O_TRUNC);
    if (f != FILEHND_INVALID) {
        fs_vmu_set_header(f, &pkg);
        ok = fs_write(f, data, CMOS_BYTES) == CMOS_BYTES;
        fs_close(f);
    }
    return ok;
}

static void *save_thread(void *arg)
{
    (void)arg;
    for (;;) {
        sem_wait(&save_sem);
        /* sin printf aqui: desde este hilo puede colgar el puerto serie */
        save_result = write_vmu(save_buf) ? 1 : -1;
        saving = 0;
    }
    return NULL;
}

/* Primera VMU conectada (para guardar) */
static int find_vmu(void)
{
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
    if (!dev)
        return 0;
    snprintf(vmu_path, sizeof(vmu_path), "/vmu/%c%d/" FILE_NAME, 'a' + dev->port, dev->unit);
    return 1;
}

static int read_vmu(void)
{
    file_t f;
    int ok;

    f = fs_open(vmu_path, O_RDONLY);
    if (f == FILEHND_INVALID)
        return 0;
    ok = fs_total(f) >= CMOS_BYTES && fs_read(f, vu.cmos, CMOS_BYTES) == CMOS_BYTES;
    fs_close(f);
    return ok;
}

int cmos_init(const uint32_t *cd_cmos, size_t cd_size)
{
    int from_vmu = 0;

    if (find_vmu())
        from_vmu = read_vmu();
    if (from_vmu)
        printf("cmos: cargada de %s\n", vmu_path);
    else if (cd_cmos && cd_size == CMOS_BYTES) {
        memcpy(vu.cmos, cd_cmos, CMOS_BYTES);
        printf("cmos: calibrada del CD\n");
    }
    memcpy(prev_cmos, vu.cmos, CMOS_BYTES);
    memcpy(save_buf, vu.cmos, CMOS_BYTES);
    sem_init(&save_sem, 0);
    thd_create(1, save_thread, NULL);
    return from_vmu;
}

void cmos_check(void)
{
    int i, changed_now = 0, dirty = 0;

    if (saving)
        return;
    if (save_result) {
        printf(save_result > 0 ? "cmos: guardada en %s\n" : "cmos: no se pudo guardar en %s\n", vmu_path);
        save_result = 0;
    }
    for (i = 0; i < VU_CMOS_WORDS; i++) {
        uint32_t w = vu.cmos[i];
        if (w != prev_cmos[i]) {
            prev_cmos[i] = w;
            heat[i] = heat[i] < 8 ? heat[i] + 2 : 10;
            if (heat[i] < HOT)
                changed_now = 1;
        } else if (heat[i]) {
            heat[i]--;
        }
        if (heat[i] < HOT && w != save_buf[i])
            dirty = 1;
    }
    /* guardar cuando haya cambios y lleven dos comprobaciones quietos */
    if (!dirty || changed_now) {
        quiet_checks = 0;
        return;
    }
    if (++quiet_checks < 2)
        return;
    if (!vmu_path[0] && !find_vmu())
        return;
    quiet_checks = 0;
    memcpy(save_buf, vu.cmos, CMOS_BYTES);
    saving = 1;
    sem_signal(&save_sem);
}
