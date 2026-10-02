/*
 * main_dc.c - Punto de entrada para Dreamcast (KallistiOS).
 *
 * Fase actual: arranca el sistema, carga la imagen de programa generada por
 * tools/romtool.py desde el CD (/cd/program.bin) o por dcload (/pc/), prepara
 * la memoria del V-Unit y muestra el estado en la consola de framebuffer.
 * El codigo del juego recompilado se enganchara en el bucle principal cuando
 * exista (ver docs/PLAN.md).
 */
#include <kos.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

#include <stdio.h>
#include <stdlib.h>

#include "../c3x/c3x.h"
#include "../vunit/mem.h"

static uint32_t *load_program(void)
{
    static const char *paths[] = { "/cd/program.bin", "/pc/program.bin" };
    size_t i;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        file_t f = fs_open(paths[i], O_RDONLY);
        size_t size;
        uint32_t *buf;

        if (f == FILEHND_INVALID)
            continue;
        size = fs_total(f);
        buf = malloc(size);
        if (buf && fs_read(f, buf, size) == (ssize_t)size) {
            fs_close(f);
            printf("programa: %s (%u bytes)\n", paths[i], (unsigned)size);
            return buf;
        }
        free(buf);
        fs_close(f);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    uint32_t *program;

    (void)argc; (void)argv;

    vid_set_mode(DM_640x480, PM_RGB565);
    dbgio_dev_select("fb");

    printf("Cruis'n USA DC - port en desarrollo\n\n");

    program = load_program();
    if (!program) {
        printf("No se encuentra program.bin.\n");
        printf("Generalo con tools/romtool.py a partir de tu ROM.\n");
    } else {
        vu_reset(program);
        printf("vector de reset: %06X\n", (unsigned)vu.fastram[0]);
        printf("prueba float C3x: 0x00400000 = %d/10\n",
               (int)(c3x_to_float(0x00400000u) * 10.0f));
    }

    printf("\nPulsa START para salir.\n");
    for (;;) {
        maple_device_t *cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        if (cont) {
            cont_state_t *st = (cont_state_t *)maple_dev_status(cont);
            if (st && (st->buttons & CONT_START))
                break;
        }
        thd_sleep(16);
    }

    free(program);
    return 0;
}
