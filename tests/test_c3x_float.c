/* Pruebas de host para la conversion de flotantes C3x <-> IEEE.
 * Compilar y ejecutar con: make -f Makefile.host test */
#include <math.h>
#include <stdio.h>
#include <stdint.h>

#include "../src/c3x/c3x.h"

static int failures;

static void check_pair(uint32_t c3x, float expected)
{
    float got = c3x_to_float(c3x);
    uint32_t back = float_to_c3x(expected);
    if (got != expected) {
        printf("FALLO c3x_to_float(%08X) = %g, esperado %g\n", c3x, got, expected);
        failures++;
    }
    if (back != c3x) {
        printf("FALLO float_to_c3x(%g) = %08X, esperado %08X\n", expected, back, c3x);
        failures++;
    }
}

static void check_short(uint16_t v, float expected)
{
    float got = c3x_short_float(v);
    if (got != expected) {
        printf("FALLO short_float(%04X) = %g, esperado %g\n", v, got, expected);
        failures++;
    }
}

int main(void)
{
    int i;

    /* Valores de la tabla del manual del TMS320C3x. */
    check_pair(0x80000000u, 0.0f);
    check_pair(0x00000000u, 1.0f);
    check_pair(0x00800000u, -2.0f);
    check_pair(0xFF800000u, -1.0f);
    check_pair(0x01000000u, 2.0f);
    check_pair(0x00400000u, 1.5f);
    check_pair(0x00C00000u, -1.5f);
    check_pair(0xFF000000u, 0.5f);
    check_pair(0x7F7FFFFFu, 3.40282347e38f);

    check_short(0x0000, 1.0f);
    check_short(0x8000, 0.0f);
    check_short(0x1000, 2.0f);
    check_short(0x0800, -2.0f);
    check_short(0xF000, 0.5f);
    check_short(0x0400, 1.5f);

    /* Ida y vuelta sobre muchos valores. */
    for (i = -100000; i <= 100000; i += 7) {
        float x = (float)i * 0.731f;
        float y = c3x_to_float(float_to_c3x(x));
        if (x != y) {
            printf("FALLO ida y vuelta %g -> %g\n", x, y);
            failures++;
            break;
        }
    }

    if (failures == 0)
        printf("c3x float: todas las pruebas OK\n");
    return failures != 0;
}
