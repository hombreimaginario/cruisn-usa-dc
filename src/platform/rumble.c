/*
 * rumble.c - Vibracion con el Puru Puru (Jump Pack).
 *
 * El mueble del arcade tenia un volante con motor (WHEEL.ASM): una fuerza de
 * centrado continua que da tirones al subir un bordillo y tiembla fuera de
 * la carretera. Un Jump Pack no puede hacer fuerza de centrado, asi que se
 * usan los mismos momentos del juego, tomados de los sonidos que pide:
 *   choques con coches (SCOLLA-H, 00C0-00CB)       golpe medio
 *   paredes, golpes fuertes (WALLHIT, MHIT, ...)   golpe fuerte
 *   grava / fuera de pista (GRAVELA, 010A-010B)    temblor suave mientras dure
 * Un hilo envia los efectos (escribir en el bus maple tarda) y solo cuando
 * cambia lo que hay que hacer.
 */
#include <kos.h>
#include <dc/maple.h>
#include <dc/maple/purupuru.h>

#include "../sound/dcs_proto.h"
#include "rumble.h"

enum { R_NONE, R_LIGHT, R_MEDIUM, R_STRONG };

static volatile int pending;            /* golpe pendiente de enviar */
static volatile uint64_t offroad_until; /* temblor suave hasta este instante (ms) */
static void (*prev_hook)(int code);

static void on_code(int code)
{
    if (code >= 0xC0 && code <= 0xCB) {
        if (pending < R_MEDIUM)
            pending = R_MEDIUM;
    } else if ((code >= 0xAC && code <= 0xB1) || code == 0x206 || code == 0x97 || code == 0x1AA) {
        pending = R_STRONG;
    } else if (code == 0x10A || code == 0x10B) {
        offroad_until = timer_ms_gettime64() + 600;
    }
    if (prev_hook)
        prev_hook(code);
}

static void rumble_send(int kind)
{
    purupuru_effect_t e;
    maple_device_t *dev;
    int n;

    e.raw = 0;
    e.motor = 1;
    switch (kind) {
    case R_STRONG:                      /* golpe seco de medio segundo */
        e.fpow = 7; e.freq = 26; e.inc = 1;
        break;
    case R_MEDIUM:
        e.fpow = 4; e.conv = true; e.freq = 30; e.inc = 20;
        break;
    case R_LIGHT:                       /* temblor continuo de la grava */
        e.cont = true; e.fpow = 1; e.freq = 40; e.inc = 0;
        break;
    default:                            /* parar */
        break;
    }
    for (n = 0; (dev = maple_enum_type(n, MAPLE_FUNC_PURUPURU)); n++)
        purupuru_rumble(dev, &e);
}

static void *rumble_thread(void *arg)
{
    int light_on = 0;
    uint64_t busy_until = 0;
    (void)arg;
    for (;;) {
        uint64_t now = timer_ms_gettime64();
        int p = pending;
        if (p != R_NONE && now >= busy_until) {
            pending = R_NONE;
            rumble_send(p);
            light_on = 0;
            busy_until = now + (p == R_STRONG ? 500 : 300);
        } else if (now >= busy_until) {
            int want = now < offroad_until;
            if (want != light_on) {
                rumble_send(want ? R_LIGHT : R_NONE);
                light_on = want;
            }
        }
        thd_sleep(50);
    }
    return NULL;
}

void rumble_start(void)
{
    prev_hook = dcs_code_hook;
    dcs_code_hook = on_code;
    thd_create(1, rumble_thread, NULL);
}
