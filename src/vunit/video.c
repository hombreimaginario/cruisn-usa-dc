/*
 * video.c - Motor de poligonos del V-Unit en software.
 *
 * Es la implementacion de referencia: dibuja en el framebuffer emulado de
 * 512x512x2 paginas con pixeles de 16 bits (paleta en 15-8, color en 7-0).
 * En Dreamcast estos mismos paquetes se traduciran a poligonos del PVR; esta
 * version sirve para el arranque en host y para comparar resultados.
 *
 * Paquete de la FIFO (15 palabras, ver DIRQ.ASM:PLOTPOLYLP):
 *   0      control: TM/CC (bits 9-8), NZR (10), ZS (11), CLIPEN (12),
 *          DITHER (13), color (7-0)
 *   1      paleta << 8
 *   2..9   X/Y de los 4 vertices (enteros con signo de 16 bits)
 *   10..13 coordenadas de textura de cada vertice: V (15-8), U (7-0)
 *   14     direccion de la textura en WAVERAM, en unidades de 256 texeles
 */
#include <string.h>

#include "mem.h"

#define SCREEN_W 512
#define SCREEN_H 512
#define VISIBLE_H 400

typedef struct {
    float x, y, u, v;
} vtx;

static int edge_setup(const vtx *a, const vtx *b, const vtx *c, float *area)
{
    *area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    return *area != 0.0f;
}

static void draw_triangle(uint16_t *page, const vtx *a, const vtx *b, const vtx *c,
                          uint32_t ctrl, uint32_t pixdata, const uint8_t *tex)
{
    float area, minx, maxx, miny, maxy;
    int x0, x1, y0, y1, x, y;
    int textured = (ctrl & 0x300) == 0x100;
    int zs = (ctrl & 0x800) != 0;
    int nzr = (ctrl & 0x400) != 0;

    if (!edge_setup(a, b, c, &area))
        return;

    minx = a->x < b->x ? a->x : b->x; if (c->x < minx) minx = c->x;
    maxx = a->x > b->x ? a->x : b->x; if (c->x > maxx) maxx = c->x;
    miny = a->y < b->y ? a->y : b->y; if (c->y < miny) miny = c->y;
    maxy = a->y > b->y ? a->y : b->y; if (c->y > maxy) maxy = c->y;

    x0 = (int)minx; x1 = (int)maxx;
    y0 = (int)miny; y1 = (int)maxy;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCREEN_W - 1) x1 = SCREEN_W - 1;
    if (y1 > SCREEN_H - 1) y1 = SCREEN_H - 1;

    for (y = y0; y <= y1; y++) {
        float py = y + 0.5f;
        for (x = x0; x <= x1; x++) {
            float px = x + 0.5f;
            float w0 = (b->x - px) * (c->y - py) - (b->y - py) * (c->x - px);
            float w1 = (c->x - px) * (a->y - py) - (c->y - py) * (a->x - px);
            float w2 = (a->x - px) * (b->y - py) - (a->y - py) * (b->x - px);
            uint16_t pix;

            if (area > 0) {
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            } else {
                if (w0 > 0 || w1 > 0 || w2 > 0) continue;
            }
            if (textured) {
                float l0 = w0 / area, l1 = w1 / area, l2 = w2 / area;
                int u = (int)(a->u * l0 + b->u * l1 + c->u * l2) & 0xFF;
                int v = (int)(a->v * l0 + b->v * l1 + c->v * l2) & 0xFF;
                uint8_t t = tex[(v << 8) | u];
                if (t == 0 && zs)
                    continue;
                pix = nzr && t ? (uint16_t)pixdata : (uint16_t)((pixdata & 0xFF00) | t);
            } else {
                pix = (uint16_t)pixdata;
            }
            page[y * SCREEN_W + x] = pix;
        }
    }
}

void (*vu_poly_hook)(const uint32_t *pkt, int page);

void vu_poly_packet(const uint32_t *d)
{
    vtx q[4];
    int i;
    uint16_t *page;
    uint32_t pixdata;
    const uint8_t *tex;
    uint32_t texoff;

    vu.polys_frame++;
    if (vu_poly_hook) {
        vu_poly_hook(d, (vu.page_control & 4) ? 1 : 0);
        return;
    }

    for (i = 0; i < 4; i++) {
        q[i].x = (float)(int16_t)d[2 + i * 2];
        q[i].y = (float)(int16_t)d[3 + i * 2];
        q[i].u = (float)(d[10 + i] & 0xFF) + 0.5f;
        q[i].v = (float)((d[10 + i] >> 8) & 0xFF) + 0.5f;
    }
    page = &vu.video[(vu.page_control & 4) ? 0x40000 : 0];
    pixdata = (d[1] & 0xFF00) | (d[0] & 0xFF);
    texoff = (d[14] & 0x7FFF) * 256;
    if (texoff > sizeof(vu.texram) - 0x10000)
        texoff = sizeof(vu.texram) - 0x10000;
    tex = vu.texram + texoff;

#ifndef VU_NO_RASTER
    draw_triangle(page, &q[0], &q[1], &q[2], d[0], pixdata, tex);
    draw_triangle(page, &q[0], &q[2], &q[3], d[0], pixdata, tex);
#else
    (void)page; (void)pixdata; (void)tex;
#endif
}

void vu_dma_process(void)
{
    if (vu.fifo_count < 15) {
        vu.fifo_count = 0;
        return;
    }
    vu.fifo_count = 0;
    vu_poly_packet(vu.fifo);
}

void vu_video_to_rgb565(uint16_t *dst, int pitch, int page)
{
    const uint16_t *src = &vu.video[page ? 0x40000 : 0];
    int x, y;

    for (y = 0; y < VISIBLE_H; y++) {
        for (x = 0; x < SCREEN_W; x++) {
            uint16_t c = vu.coloram[src[y * SCREEN_W + x] & 0x7FFF];
            /* xRGB555 -> RGB565 */
            dst[y * pitch + x] = (uint16_t)(((c & 0x7FE0) << 1) | (c & 0x1F));
        }
    }
}
