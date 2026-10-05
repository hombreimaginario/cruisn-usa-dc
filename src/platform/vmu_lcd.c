/*
 * vmu_lcd.c - Pantalla de la VMU: logo en el modo demo y, en carrera,
 * posicion y velocidad.
 *
 * (Fuente de 5x7 propia.) Lee del juego (direcciones de la ROM L4.1, buscadas comparando volcados de
 * memoria en una carrera):
 *   0x00C8C5  _MODE     (bits 3-0: 2 modo demo, 4 juego, 5 bonus)
 *   0x00E605  _MPH      velocidad en millas por hora
 *   0x00E607  POSITION  puesto (1-10...)
 * Un hilo aparte redibuja cuatro veces por segundo y solo envia a la VMU si
 * cambia algo: escribir en la VMU pasa por el bus maple y tarda.
 */
#include <kos.h>
#include <dc/maple.h>
#include <dc/maple/vmu.h>
#include <dc/vmu_fb.h>

#include <string.h>

#include "../vunit/mem.h"
#include "vmu_lcd.h"

#define ADDR_MODE     0x00C8C5u
#define ADDR_MPH      0x00E605u
#define ADDR_POSITION 0x00E607u

#define W 48
#define H 32

static uint8_t pix[H][W];

/* Fuente propia de 5x7 (columnas de bits, bit 0 arriba) */
static const struct { char c; uint8_t col[5]; } font[] = {
    { '0', { 0x3E, 0x51, 0x49, 0x45, 0x3E } }, { '1', { 0x00, 0x42, 0x7F, 0x40, 0x00 } },
    { '2', { 0x62, 0x51, 0x49, 0x49, 0x46 } }, { '3', { 0x22, 0x41, 0x49, 0x49, 0x36 } },
    { '4', { 0x18, 0x14, 0x12, 0x7F, 0x10 } }, { '5', { 0x27, 0x45, 0x45, 0x45, 0x39 } },
    { '6', { 0x3C, 0x4A, 0x49, 0x49, 0x30 } }, { '7', { 0x01, 0x71, 0x09, 0x05, 0x03 } },
    { '8', { 0x36, 0x49, 0x49, 0x49, 0x36 } }, { '9', { 0x06, 0x49, 0x49, 0x29, 0x1E } },
    { 'A', { 0x7E, 0x11, 0x11, 0x11, 0x7E } }, { 'C', { 0x3E, 0x41, 0x41, 0x41, 0x22 } },
    { 'D', { 0x7F, 0x41, 0x41, 0x22, 0x1C } }, { 'H', { 0x7F, 0x08, 0x08, 0x08, 0x7F } },
    { 'I', { 0x00, 0x41, 0x7F, 0x41, 0x00 } }, { 'M', { 0x7F, 0x02, 0x0C, 0x02, 0x7F } },
    { 'N', { 0x7F, 0x04, 0x08, 0x10, 0x7F } }, { 'P', { 0x7F, 0x09, 0x09, 0x09, 0x06 } },
    { 'R', { 0x7F, 0x09, 0x19, 0x29, 0x46 } }, { 'S', { 0x46, 0x49, 0x49, 0x49, 0x31 } },
    { 'T', { 0x01, 0x01, 0x7F, 0x01, 0x01 } }, { 'U', { 0x3F, 0x40, 0x40, 0x40, 0x3F } },
    { 'L', { 0x7F, 0x40, 0x40, 0x40, 0x40 } }, { '\'', { 0x00, 0x00, 0x07, 0x00, 0x00 } },
};

static const uint8_t *glyph(char c)
{
    unsigned i;
    for (i = 0; i < sizeof(font) / sizeof(font[0]); i++)
        if (font[i].c == c)
            return font[i].col;
    return NULL;
}

/* Texto en (x, y) con escala s; devuelve la x final */
static int text(int x, int y, const char *s, int scale)
{
    for (; *s; s++) {
        const uint8_t *g = glyph(*s);
        int cx, cy, i, j;
        if (g) {
            for (cx = 0; cx < 5; cx++)
                for (cy = 0; cy < 7; cy++)
                    if (g[cx] >> cy & 1)
                        for (i = 0; i < scale; i++)
                            for (j = 0; j < scale; j++) {
                                int px = x + cx * scale + i, py = y + cy * scale + j;
                                if (px >= 0 && px < W && py >= 0 && py < H)
                                    pix[py][px] = 1;
                            }
        }
        x += (*s == '\'' ? 3 : 6) * scale;
    }
    return x;
}

static int text_width(const char *s, int scale)
{
    int w = 0;
    for (; *s; s++)
        w += (*s == '\'' ? 3 : 6) * scale;
    return w - scale;
}

static void rect(int x0, int y0, int x1, int y1)
{
    int x, y;
    for (x = x0; x <= x1; x++) {
        pix[y0][x] = 1;
        pix[y1][x] = 1;
    }
    for (y = y0; y <= y1; y++) {
        pix[y][x0] = 1;
        pix[y][x1] = 1;
    }
}

static void draw_logo(void)
{
    memset(pix, 0, sizeof(pix));
    text((W - text_width("CRUIS'N", 1)) / 2, 2, "CRUIS'N", 1);
    rect(4, 11, 43, 30);                 /* placa de autopista */
    text((W - text_width("USA", 2)) / 2, 14, "USA", 2);
}

static void draw_race(int pos, int mph)
{
    static const char *suf[] = { "TH", "ST", "ND", "RD" };
    char buf[8];
    int x;

    memset(pix, 0, sizeof(pix));
    /* puesto grande arriba: "9TH" */
    if (pos > 99) pos = 99;
    if (pos >= 10) {
        buf[0] = (char)('0' + pos / 10);
        buf[1] = (char)('0' + pos % 10);
        buf[2] = 0;
    } else {
        buf[0] = (char)('0' + pos);
        buf[1] = 0;
    }
    x = text(1, 1, buf, 2);
    text(x, 1, (pos % 10 <= 3 && (pos < 11 || pos > 13)) ? suf[pos % 10] : "TH", 1);
    /* velocidad abajo, grande y alineada a la derecha (millas por hora) */
    if (mph > 999) mph = 999;
    if (mph < 0) mph = 0;
    buf[0] = mph >= 100 ? (char)('0' + mph / 100) : ' ';
    buf[1] = mph >= 10 ? (char)('0' + mph / 10 % 10) : ' ';
    buf[2] = (char)('0' + mph % 10);
    buf[3] = 0;
    text(W - text_width("000", 2) - 1, 17, buf, 2);
}

static void present(void)
{
    static vmufb_t fb;
    static uint8_t xbm[H * W / 8];
    int x, y, n;
    maple_device_t *dev;

    memset(xbm, 0, sizeof(xbm));
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            if (pix[y][x])
                xbm[y * (W / 8) + x / 8] |= (uint8_t)(1 << (x & 7));
    vmufb_clear(&fb);
    vmufb_paint_xbm(&fb, 0, 0, W, H, xbm);
    for (n = 0; (dev = maple_enum_type(n, MAPLE_FUNC_LCD)); n++)
        vmufb_present(&fb, dev);
}

static void *lcd_thread(void *arg)
{
    int last_kind = -1, last_pos = -1, last_mph = -1;
    (void)arg;
    for (;;) {
        uint32_t mode = vu.fastram[ADDR_MODE] & 0xF;
        int kind = (mode == 4) ? 1 : 0;
        int pos = (int)vu.fastram[ADDR_POSITION], mph = (int)vu.fastram[ADDR_MPH];

        if (kind != last_kind || (kind && (pos != last_pos || mph != last_mph))) {
            if (kind)
                draw_race(pos, mph);
            else
                draw_logo();
            present();
            last_kind = kind;
            last_pos = pos;
            last_mph = mph;
        }
        thd_sleep(250);
    }
    return NULL;
}

void vmu_lcd_start(void)
{
    thd_create(1, lcd_thread, NULL);
}
