/*
 * pvr_render.c - Motor de poligonos del V-Unit sobre el PowerVR de Dreamcast.
 *
 * Los paquetes de 15 palabras que el juego envia a la FIFO (ver video.c) se
 * guardan por pagina de video. Cuando el juego muestra una pagina, sus
 * poligonos se envian al PVR:
 *   - el orden de dibujo del original (algoritmo del pintor) se conserva con
 *     la Z: cada poligono posterior tiene una Z mayor y gana el test GEQUAL,
 *     asi que se pueden agrupar por textura sin alterar el resultado;
 *   - las texturas de la WAVERAM (8 bits + paleta de 32K colores) se
 *     convierten a ARGB1555 y se cachean en VRAM por (base, paleta, modo,
 *     alto); se invalidan cuando cambia la WAVERAM o la paleta;
 *   - el texel 0 transparente (ZS) usa la lista punch-through.
 * Si una pagina no tiene poligonos (pantallas dibujadas por la CPU: arranque,
 * tests) se muestra el framebuffer emulado como una textura.
 */
#include <kos.h>
#include <dc/pvr.h>
#include <dc/sq.h>
#include <dc/biosfont.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../vunit/mem.h"
#include "pvr_render.h"
#include "watchdog.h"
#include "textfont.h"

#define MAX_POLYS   4096
#define VERTEX_BUF  (640 * 1024)
#define TEX_SLOTS   1024
#define SX          1.25f       /* 512 -> 640 */
#define SY          1.2f        /* 400 -> 480 */

/* Lo que se usa del paquete de 15 palabras, en 32 bytes (una linea de
 * cache): coordenadas, UV, control (ZS/NZR/DITHER/color), paleta y base
 * de textura. */
typedef struct {
    int16_t x[4], y[4];
    uint16_t uv[4];             /* u en el byte bajo, v en el alto */
    uint16_t c0, c1, tb, pad;   /* palabras 0, 1 (byte alto) y 14 (16 bits bajos);
                                 * niebla de los 4 vertices, 6 bits cada uno, en
                                 * el byte bajo de c1 y en pad */
} __attribute__((aligned(32))) packet;

static packet *page_polys[2];
static int page_npolys[2];
static int last_shown = -1;
static int pending_shot;

/* ---- cache de texturas ---- */

typedef struct {
    uint64_t key;               /* 0 = libre */
    pvr_ptr_t ptr;
    uint32_t blk_sum, pal_gen;
    uint32_t last_used;
    uint16_t w, h;              /* tamano (potencias de 2) */
    uint8_t  u0, v0;            /* esquina en la pagina de 256x256 */
    uint8_t  pt;                /* lista punch-through */
    uint8_t  half;              /* guardada a media resolucion */
    uint16_t seq;               /* orden de primer uso en el frame actual */
    uint32_t seq_frame;
    float iw, ih;               /* 1/w, 1/h */
} tex_entry;

/* ---- asignador "buddy" para texturas dentro de un bloque fijo de VRAM ----
 * pvr_mem_malloc es lento y se fragmenta con miles de texturas pequenas. */

#define POOL_ORDER  23                  /* hasta 8 MB */
#define MIN_ORDER   8                   /* bloques de 256 bytes */
#define NBLK        (1 << (POOL_ORDER - MIN_ORDER))
#define NORD        (POOL_ORDER - MIN_ORDER + 1)

static uint8_t *pool;
static uint32_t pool_bytes;
static int16_t fl_next[NBLK], fl_prev[NBLK];
static int16_t fl_head[NORD];
static int8_t blk_free_ord[NBLK];       /* orden si esta libre y es cabeza, -1 si no */

static void fl_push(int ord, int b)
{
    fl_prev[b] = -1;
    fl_next[b] = fl_head[ord];
    if (fl_head[ord] >= 0)
        fl_prev[fl_head[ord]] = (int16_t)b;
    fl_head[ord] = (int16_t)b;
    blk_free_ord[b] = (int8_t)ord;
}

static void fl_remove(int ord, int b)
{
    if (fl_prev[b] >= 0) fl_next[fl_prev[b]] = fl_next[b];
    else fl_head[ord] = fl_next[b];
    if (fl_next[b] >= 0) fl_prev[fl_next[b]] = fl_prev[b];
    blk_free_ord[b] = -1;
}

/* El bloque no tiene por que ser potencia de 2: se reparte en trozos
 * alineados (los buddies fuera del rango nunca estan libres). */
static void pool_init(void)
{
    int i, b = 0;
    uint32_t avail = (uint32_t)pvr_mem_available();
    pool_bytes = (avail - 64 * 1024) & ~((1u << MIN_ORDER) - 1);
    if (pool_bytes > (1u << POOL_ORDER))
        pool_bytes = 1u << POOL_ORDER;
    pool = pvr_mem_malloc(pool_bytes);
    for (i = 0; i < NORD; i++)
        fl_head[i] = -1;
    for (i = 0; i < NBLK; i++)
        blk_free_ord[i] = -1;
    for (i = NORD - 1; i >= 0; i--) {
        uint32_t nblk = pool_bytes >> MIN_ORDER;
        while ((uint32_t)b + (1u << i) <= nblk && (b & ((1 << i) - 1)) == 0) {
            fl_push(i, b);
            b += 1 << i;
        }
    }
}

static int order_for(uint32_t bytes)
{
    int o = 0;
    while ((1u << (o + MIN_ORDER)) < bytes)
        o++;
    return o;
}

static void *pool_alloc(uint32_t bytes)
{
    int o = order_for(bytes), k = o, b;
    while (k < NORD && fl_head[k] < 0)
        k++;
    if (k >= NORD)
        return NULL;
    b = fl_head[k];
    fl_remove(k, b);
    while (k > o) {
        k--;
        fl_push(k, b + (1 << k));
    }
    return pool + ((uint32_t)b << MIN_ORDER);
}

static void pool_free(void *p, uint32_t bytes)
{
    int o = order_for(bytes);
    int b = (int)(((uint8_t *)p - pool) >> MIN_ORDER);
    while (o < NORD - 1) {
        int buddy = b ^ (1 << o);
        if (buddy >= (int)(pool_bytes >> MIN_ORDER) || blk_free_ord[buddy] != o)
            break;
        fl_remove(o, buddy);
        b &= ~(1 << o);
        o++;
    }
    fl_push(o, b);
}

/* Rectangulo de textura que usa un poligono. */
typedef struct {
    uint32_t base, pal, color;
    int zs, nzr;
    uint32_t u0, v0, w, h, wc, hc;
} tex_req;

static tex_entry texs[TEX_SLOTS];
static uint32_t frame_no;
static uint16_t conv_buf[256 * 256] __attribute__((aligned(32)));
static pvr_ptr_t cpu_fb_tex, shot_tex;

static unsigned stat_conv, stat_polys, stat_skipped, stat_pages, stat_why[4], stat_ta_timeouts;
static uint64_t t_tex, t_wait, t_sub;
static unsigned conv_this_frame, stat_tiny, stat_thin;
#ifdef CUSA_OPBDBG
static int dbg_opb_max;
#endif
#ifdef CUSA_DIAG
static unsigned diag_conv;
#endif
#ifdef CUSA_POLYDBG
static unsigned dbg_big, dbg_huge;
static int dbg_maxc;
#endif
#ifdef CUSA_TEXDBG
static unsigned dbg_new, dbg_blk, dbg_pal, dbg_evict, dbg_ktex;
#endif
#define MAX_CONV_PER_FRAME 400
/* Texturas de este tamano (en texeles del arcade) o mayores se guardan a
 * media resolucion: una cuarta parte de VRAM y de conversion. Son las del
 * suelo, el cielo y los decorados grandes; se ven algo mas borrosas. */
#ifndef TEX_HALF_TEXELS
#define TEX_HALF_TEXELS (128 * 128)
#endif
#define MAX_TEXELS_PER_FRAME (1024 * 1024)
/* Despues de retener la imagen anterior HOLD_UNLIMIT frames seguidos por
 * texturas pendientes, se convierten todas de una vez (un frame lento) en
 * vez de acabar mostrando la escena sin marcador ni coches. */
#define HOLD_UNLIMIT 8
static int conv_unlimited, vram_full, frame_is_menu;
#define CONV_LIMIT() (vram_full || (!conv_unlimited && \
    (conv_this_frame >= MAX_CONV_PER_FRAME || texels_this_frame >= MAX_TEXELS_PER_FRAME)))
static uint32_t texels_this_frame;

static inline uint16_t argb1555(uint16_t c, int opaque)
{
    return (uint16_t)((c & 0x7FFF) | (opaque ? 0x8000 : 0));
}

static uint64_t tex_key(const tex_req *r)
{
    return (1ULL << 63) | ((uint64_t)r->base << 40) | ((uint64_t)r->pal << 33) |
           ((uint64_t)((r->zs ? 1 : 0) | (r->nzr ? 2 : 0)) << 31) | ((uint64_t)r->color << 23) |
           ((uint64_t)(r->u0 >> 4) << 19) | ((uint64_t)(r->v0 >> 4) << 15) |
           ((uint64_t)r->wc << 12) | ((uint64_t)r->hc << 9);
}

/* Expulsa una textura de la VRAM: recorre la tabla en circulo desde donde
 * lo dejo la vez anterior y se queda con la primera que no se ha usado en
 * los ultimos 30 frames (antes se buscaba la mas antigua de las 1024 cada
 * vez, y en circuitos con muchas texturas eso se notaba). Si no hay
 * ninguna tan vieja, la mas antigua de la vuelta. */
static int evict_one(void)
{
    static int hand;
    int i, n, best = -1;
    for (n = 0; n < TEX_SLOTS; n++) {
        i = hand;
        hand = (hand + 1) % TEX_SLOTS;
        if (!texs[i].key || !texs[i].ptr || texs[i].last_used == frame_no)
            continue;
        if (frame_no - texs[i].last_used >= 30) {
            best = i;
            break;
        }
        if (best < 0 || texs[i].last_used < texs[best].last_used)
            best = i;
    }
    if (best >= 0) {
#ifdef CUSA_TEXDBG
        dbg_evict++;
#endif
        pool_free(texs[best].ptr, (uint32_t)texs[best].w * texs[best].h * 2);
        texs[best].ptr = NULL;
        texs[best].key = 0;
        return 1;
    }
    return 0;
}

/*
 * El PowerVR dibuja el frame anterior mientras la CPU prepara el siguiente.
 * Antes de escribir en la VRAM de texturas (texturas nuevas, reconvertidas o
 * en el hueco de otra expulsada) hay que esperar a que termine de dibujar:
 * si no, el frame en curso sale con texturas de otro objeto (en hardware
 * real; Flycast dibuja al instante y no se nota).
 */
static uint32_t guarded_frame;
static uint64_t t_rwait;

static void vram_guard(void)
{
    if (guarded_frame != frame_no) {
        uint64_t t = timer_us_gettime64();
        wd_phase = "PowerVR: esperando fin de dibujo";
        pvr_wait_render_done();
        wd_phase = "render";
        t_rwait += timer_us_gettime64() - t;
        guarded_frame = frame_no;
    }
}

/*
 * Convierte la textura y la escribe directamente en la VRAM con las store
 * queues, linea a linea: antes se convertia entera a un bufer de 128 KB y
 * despues se copiaba, dos pasadas que ademas vaciaban la cache de datos
 * (16 KB). Al empezar cada carrera se convierten miles.
 */
static int convert(tex_entry *t, const tex_req *r, uint32_t pixdata)
{
    const uint8_t *src = vu.texram;
    const uint16_t *cram = &vu.coloram[(r->pal & 0x7F) << 8];
    uint16_t nzr_col = vu.coloram[pixdata & 0x7FFF];
    uint32_t x, y, n = t->w * t->h, w = t->w, st = t->half ? 2 : 1;
    static uint16_t rowbuf[256] __attribute__((aligned(32)));
    uint16_t lut[256];
    uint32_t i, *d;

    if (!t->ptr) {
        while (!(t->ptr = pool_alloc(n * 2))) {
            if (!evict_one()) {
                vram_full = 1;                          /* VRAM llena: basta por hoy */
                return 0;
            }
        }
    }
    /* tabla de los 256 valores de texel ya en ARGB1555 */
    for (i = 0; i < 256; i++) {
        if (r->nzr)
            lut[i] = i ? argb1555(nzr_col, 1) : argb1555(0, !r->zs);
        else if (r->zs && i == 0)
            lut[i] = 0;
        else
            lut[i] = argb1555(cram[i], 1);
    }
    vram_guard();
    d = sq_lock((void *)(((uintptr_t)t->ptr & 0xFFFFFF) | PVR_TA_TEX_MEM));
    for (y = 0; y < t->h; y++) {
        const uint8_t *row = src + ((r->base * 256 + ((r->v0 + y * st) & 0xFF) * 256) & (sizeof(vu.texram) - 1));
        /* con 8 de ancho caben dos lineas en cada bloque de 32 bytes */
        uint16_t *o = rowbuf + (w == 8 ? (y & 1) * 8 : 0);
        const uint32_t *q = (const uint32_t *)rowbuf;
        uint32_t chunks;
        if (st == 1 && r->u0 + w <= 256) {
            const uint8_t *p = row + r->u0;
            for (x = 0; x < w; x += 4) {
                o[0] = lut[p[0]]; o[1] = lut[p[1]];
                o[2] = lut[p[2]]; o[3] = lut[p[3]];
                o += 4; p += 4;
            }
        } else {
            for (x = 0; x < w; x++)
                *o++ = lut[row[(r->u0 + x * st) & 0xFF]];
        }
        if (w == 8 && !(y & 1))
            continue;
        for (chunks = w == 8 ? 1 : w / 16; chunks; chunks--) {
            d[0] = q[0]; d[1] = q[1]; d[2] = q[2]; d[3] = q[3];
            d[4] = q[4]; d[5] = q[5]; d[6] = q[6]; d[7] = q[7];
            sq_flush(d);
            d += 8;
            q += 8;
        }
    }
    sq_unlock();
#ifdef CUSA_TEXDBG
    dbg_ktex += n / 1024;
#endif
#ifdef CUSA_DIAG
    diag_conv++;
#endif
    stat_conv++;
    return 1;
}

static void pow2(uint32_t span, uint32_t *size, uint32_t *cls)
{
    uint32_t s = 8, c = 0;
    while (s < span) {
        s <<= 1;
        c++;
    }
    *size = s;
    *cls = c;
}

/* Cache directa de las busquedas del frame actual: muchos poligonos usan el
 * mismo rectangulo de textura y ya se valido en este frame. */
#define LC_SIZE 2048
typedef struct {
    uint32_t w[5];              /* base|pal, control, uv0..uv3 combinados */
    uint32_t frame;
    tex_entry *t;
} lookup_entry;
static lookup_entry lcache[LC_SIZE];

static tex_entry *get_texture_slow(const packet *d);

static inline tex_entry *get_texture(const packet *d)
{
    uint32_t a = (d->tb & 0x7FFF) | ((uint32_t)(d->c1 & 0x7F00) << 8);
    uint32_t b = d->c0 & 0xCFF;
    uint32_t c = d->uv[0] | ((uint32_t)d->uv[1] << 16);
    uint32_t e = d->uv[2] | ((uint32_t)d->uv[3] << 16);
    uint32_t h = (a * 2654435761u) ^ (b * 0x9E3779B1u) ^ (c * 0x85EBCA77u) ^ (e * 0xC2B2AE3Du);
    lookup_entry *l = &lcache[(h >> 16) & (LC_SIZE - 1)];
    tex_entry *t;

    if (l->frame == frame_no && l->w[0] == a && l->w[1] == b && l->w[2] == c && l->w[3] == e)
        return l->t;
    t = get_texture_slow(d);
    l->w[0] = a; l->w[1] = b; l->w[2] = c; l->w[3] = e;
    l->frame = frame_no;
    l->t = t;
    return t;
}

static tex_entry *get_texture_slow(const packet *d)
{
    tex_req r;
    uint32_t umin = 255, umax = 0, vmin = 255, vmax = 0, slot, k, g, pg;
    uint64_t key;
    tex_entry *t;
    int flat2d;

    for (k = 0; k < 4; k++) {
        uint32_t u = d->uv[k] & 0xFF, v = d->uv[k] >> 8;
        if (u < umin) umin = u;
        if (u > umax) umax = u;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    r.base = d->tb & 0x7FFF;
    r.pal = (d->c1 >> 8) & 0x7F;
    r.zs = (d->c0 & 0x800) != 0;
    r.nzr = (d->c0 & 0x400) != 0;
    r.color = r.nzr ? (d->c0 & 0xFF) : 0;
#ifdef PVR_FULL_TEX
    umin = 0; vmin = 0; umax = 255;
#endif
    r.u0 = umin & ~15u;
    r.v0 = vmin & ~15u;
    pow2(umax - r.u0 + 1, &r.w, &r.wc);
    pow2(vmax - r.v0 + 1, &r.h, &r.hc);
    if (r.u0 + r.w > 256) r.u0 = 256 - r.w;
    if (r.v0 + r.h > 256) r.v0 = 256 - r.h;
    key = tex_key(&r);
    /* en pantallas con pocos poligonos (menus) los rectangulos rectos son
     * imagenes 2D: esas se guardan a resolucion completa (a media se veian
     * borrosas); en carrera, el cielo y los fondos siguen a media */
    flat2d = (d->x[0] == d->x[3] && d->x[1] == d->x[2] && d->y[0] == d->y[1] && d->y[2] == d->y[3]) ||
             (d->x[0] == d->x[1] && d->x[2] == d->x[3] && d->y[0] == d->y[3] && d->y[1] == d->y[2]);

    g = 0;
    for (k = (r.base + r.v0) >> 4; k <= ((r.base + r.v0 + r.h - 1) >> 4); k++)
        g += vu.texblk_gen[k & 1023];
    pg = vu.pal_gen[r.pal];

    slot = (uint32_t)((key ^ (key >> 29)) * 2654435761u) % TEX_SLOTS;
    for (k = 0; k < TEX_SLOTS; k++) {
        t = &texs[(slot + k) % TEX_SLOTS];
        if (t->key == key)
            break;
        if (t->key == 0 && !t->ptr) {
            memset(t, 0, sizeof(*t));
            t->key = key;
            t->w = (uint16_t)r.w;
            t->h = (uint16_t)r.h;
            if (r.w * r.h >= TEX_HALF_TEXELS && !(flat2d && frame_is_menu)) {
                t->w >>= 1;
                t->h >>= 1;
                t->half = 1;
            }
            t->u0 = (uint8_t)r.u0;
            t->v0 = (uint8_t)r.v0;
            t->pt = (r.zs || r.nzr) ? 1 : 0;
            t->iw = 1.0f / (float)r.w;
            t->ih = 1.0f / (float)r.h;
            t->blk_sum = ~g;         /* fuerza la conversion */
            break;
        }
    }
    if (k == TEX_SLOTS) {
        stat_why[0]++;
        return NULL;
    }

    if (t->blk_sum != g || t->pal_gen != pg || !t->ptr) {
#ifdef CUSA_TEXDBG
        if (!t->ptr) dbg_new++; else if (t->blk_sum != g) dbg_blk++; else dbg_pal++;
#endif
        uint32_t pixdata = (d->c1 & 0xFF00) | (d->c0 & 0xFF);
        /* limite de conversiones por frame: se reutiliza la version anterior */
        if (CONV_LIMIT() && t->ptr) {
            t->last_used = frame_no;
            return t;
        }
        if (CONV_LIMIT() || !convert(t, &r, pixdata)) {
            stat_why[vram_full ? 3 : conv_this_frame >= MAX_CONV_PER_FRAME ? 1 : 2]++;
            if (!t->ptr)
                t->key = 0;
            return NULL;
        }
        conv_this_frame++;
        texels_this_frame += (uint32_t)t->w * t->h;
        t->blk_sum = g;
        t->pal_gen = pg;
    }
    t->last_used = frame_no;
    return t;
}

/* ---- captura de poligonos ---- */

/* El juego reinicia la FIFO al empezar cada frame (MAINLOOP): se descarta
 * lo que hubiera en la pagina de dibujo. */
static int restart_pending[2];

/* ---- aparicion gradual ----
 * El juego deja de dibujar los objetos mas alla de Z ~80000 y aparecen de
 * golpe al cruzar ese limite (igual en el arcade). Las rutinas HLE marcan
 * cada objeto que aparece al fondo (vu_poly_fade, 0-63) y sus poligonos van
 * a la lista translucida con ese alfa durante unos 0,3 s. */
#define POLY_FADED(d) ((d)->pad != 0)
static unsigned fade_polys;

static void on_fifo_reset(void)
{
    /* La FIFO se reinicia al terminar un frame; el siguiente poligono que
     * llegue a esta pagina empieza un frame nuevo. */
    restart_pending[(vu.page_control & 4) ? 1 : 0] = 1;
}

static void on_poly(const uint32_t *d, int page)
{
    if (restart_pending[page]) {
        restart_pending[page] = 0;
        page_npolys[page] = 0;
    }
    {
        /* fuera de la pantalla de 512x400 por completo: no se guarda (entre
         * un 5 y un 10 % en carretera: cielo y suelo en tiras de 256) */
        int16_t x0 = (int16_t)d[2], x1 = (int16_t)d[4], x2 = (int16_t)d[6], x3 = (int16_t)d[8];
        int16_t y0 = (int16_t)d[3], y1 = (int16_t)d[5], y2 = (int16_t)d[7], y3 = (int16_t)d[9];
        packet *o;
        if ((x0 < 0 && x1 < 0 && x2 < 0 && x3 < 0) || (x0 >= 512 && x1 >= 512 && x2 >= 512 && x3 >= 512) ||
            (y0 < 0 && y1 < 0 && y2 < 0 && y3 < 0) || (y0 >= 400 && y1 >= 400 && y2 >= 400 && y3 >= 400))
            return;
#ifdef CUSA_POLYDBG
        {
            int m = 0, k;
            int16_t c[8] = { x0, x1, x2, x3, y0, y1, y2, y3 };
            for (k = 0; k < 8; k++) {
                int a = c[k] < 0 ? -c[k] : c[k];
                if (a > m) m = a;
            }
            if (m > dbg_maxc) dbg_maxc = m;
            if (m > 2048) dbg_big++;
            if (m > 8192) dbg_huge++;
        }
#endif
        /* los cuatro vertices en el mismo pixel: no se veria nada (objetos
         * lejanos con muchos poligonos) */
        if (x0 == x1 && x0 == x2 && x0 == x3 && y0 == y1 && y0 == y2 && y0 == y3) {
            stat_tiny++;
            return;
        }
#ifdef CUSA_POLYDUMP
        {
            extern int cusa_frame;
            if (cusa_frame >= CUSA_POLYDUMP && cusa_frame < CUSA_POLYDUMP + 3)
                printf("POLY c0=%04X c1=%04X tb=%04X xy=%d,%d %d,%d %d,%d %d,%d uv=%04X %04X %04X %04X\n",
                       (unsigned)(d[0] & 0xFFFF), (unsigned)(d[1] & 0xFFFF), (unsigned)(d[14] & 0xFFFF),
                       x0, y0, x1, y1, x2, y2, x3, y3, (unsigned)(d[10] & 0xFFFF), (unsigned)(d[11] & 0xFFFF),
                       (unsigned)(d[12] & 0xFFFF), (unsigned)(d[13] & 0xFFFF));
        }
#endif
        if (page_npolys[page] >= MAX_POLYS)
            return;
        o = &page_polys[page][page_npolys[page]++];
        o->x[0] = x0; o->x[1] = x1; o->x[2] = x2; o->x[3] = x3;
        o->y[0] = y0; o->y[1] = y1; o->y[2] = y2; o->y[3] = y3;
        o->uv[0] = (uint16_t)d[10]; o->uv[1] = (uint16_t)d[11];
        o->uv[2] = (uint16_t)d[12]; o->uv[3] = (uint16_t)d[13];
        o->c0 = (uint16_t)d[0]; o->c1 = (uint16_t)(d[1] & 0xFF00); o->tb = (uint16_t)d[14];
        o->pad = 0;
#ifndef CUSA_NO_FADE
        if (vu_poly_fade < 63) {
            o->pad = (uint16_t)(64 - vu_poly_fade);     /* 1..64: transparencia */
            fade_polys++;
        }
#endif
    }
}

/* ---- envio al PVR ---- */

typedef struct {
    const packet *d;
    tex_entry *t;
    float z;
} draw_item;

/* Poligonos del frame ordenados por textura (orden de primer uso), por
 * separado para la lista opaca y la punch-through. El orden de dibujo del
 * original lo mantiene la Z. */
static draw_item items_buf[MAX_POLYS];
static draw_item *items[3];             /* opaca, punch-through, tramada (seguidas) */
static draw_item unsorted[MAX_POLYS];
static uint16_t seq_count[3][MAX_POLYS + 1];

static uint32_t vbytes;                /* bytes enviados al TA este frame */
static unsigned stat_vfull;

/* alto del triangulo (pixeles del arcade) respecto a su lado mas largo */
static int thin_tri(const packet *d, int a, int b, int c)
{
    float ax = d->x[a], ay = d->y[a], bx = d->x[b], by = d->y[b], cx = d->x[c], cy = d->y[c];
    float cr = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
    float l1 = (bx - ax) * (bx - ax) + (by - ay) * (by - ay);
    float l2 = (cx - bx) * (cx - bx) + (cy - by) * (cy - by);
    float l3 = (ax - cx) * (ax - cx) + (ay - cy) * (ay - cy);
    float lm = l1 > l2 ? (l1 > l3 ? l1 : l3) : (l2 > l3 ? l2 : l3);
    /* alto = |cr| / lado; alto < 0,5  <=>  cr^2 < 0,25 * lado^2. Con <= entran
     * tambien los triangulos reducidos a un punto (lm = 0): los coches
     * lejanos traen decenas por frame y con < se enviaban al PVR. */
    return cr * cr <= 0.25f * lm;
}

static void submit_list(pvr_list_t list, const draw_item *it, int n)
{
    int i, k;
    tex_entry *cur = (tex_entry *)1;
    uint32_t cur_col = 0xFFFFFFFFu;
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;

    for (i = 0; i < n; i++, it++) {
        const packet *d = it->d;
        tex_entry *t = it->t;
        if (i + 4 < n)
            __builtin_prefetch(it[4].d);
        /* no pasarse del bufer de vertices (cabecera + 4 vertices) */
        if (vbytes + 5 * 32 > VERTEX_BUF - 4096) {
            stat_vfull++;
            wd_counters[WD_VBUF_FULL]++;
            break;
        }
        uint32_t argb = 0xFFFFFFFFu;
        float uo = 0, vo = 0, iw = 0, ih = 0, z = it->z;

        if (!t) {
            uint16_t c = vu.coloram[((d->c1 & 0xFF00) | (d->c0 & 0xFF)) & 0x7FFF];
            argb = 0xFF000000u | ((c & 0x7C00) << 9) | ((c & 0x03E0) << 6) | ((c & 0x1F) << 3);
        }
        if (list == PVR_LIST_TR_POLY)
            argb = (argb & 0x00FFFFFFu) | 0x80000000u;
        if (t != cur || (!t && argb != cur_col)) {
            pvr_poly_hdr_t *hp;
            if (t) {
                pvr_poly_cxt_txr(&cxt, list, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED,
                                 t->w, t->h, t->ptr, PVR_FILTER_NONE);
                cxt.txr.env = list == PVR_LIST_TR_POLY ? PVR_TXRENV_MODULATEALPHA : PVR_TXRENV_REPLACE;
            } else {
                pvr_poly_cxt_col(&cxt, list);
            }
            cxt.gen.culling = PVR_CULLING_NONE;
            cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
            pvr_poly_compile(&hdr, &cxt);
            hp = pvr_dr_target();
            *hp = hdr;
            pvr_dr_commit(hp);
            vbytes += 32;
            cur = t;
            cur_col = argb;
        }
        if (t) {
            iw = t->iw;
            ih = t->ih;
            uo = (0.5f - (float)t->u0) * iw;
            vo = (0.5f - (float)t->v0) * ih;
        }
        /*
         * Triangulos casi planos (menos de medio pixel de alto respecto a su
         * lado mas largo): no se ven, pero el PowerVR de la consola pinta con
         * ellos rayas horizontales mas largas que el poligono (los coches
         * lejanos salian con una raya de su color). Flycast no lo hace. Se
         * descartan; si solo lo es uno de los dos triangulos de la tira, se
         * envia el otro solo.
         */
        int seq[4], nv = 0;
        {
#ifdef CUSA_NO_THINCULL
            int ka = 1, kb = 1;
#else
            /* caso comun: triangulo enviado como cuadrilatero repitiendo el
             * ultimo vertice (el segundo triangulo de la tira es plano) */
            int dup = d->x[3] == d->x[2] && d->y[3] == d->y[2];
            int ka = !thin_tri(d, 0, 1, 3), kb = !dup && !thin_tri(d, 1, 3, 2);
#endif
            if (ka && kb) {
                seq[0] = 0; seq[1] = 1; seq[2] = 3; seq[3] = 2; nv = 4;
            } else if (ka) {
                seq[0] = 0; seq[1] = 1; seq[2] = 3; nv = 3;
            } else if (kb) {
                seq[0] = 1; seq[1] = 3; seq[2] = 2; nv = 3;
            }
            if (nv < 4)
                stat_thin++;
        }
        /*
         * El V-Unit pinta los pixeles de los bordes enteros (y el poligono
         * posterior pisa la fila o columna compartida); el PowerVR corta en
         * el centro del pixel del vertice. Cada vertice se aleja medio pixel
         * del centro del poligono (con las mismas UV): sin esto, entre dos
         * poligonos asomaba media fila del borde de la textura de arriba
         * (linea negra en el mapa de "elegir carrera"). Comparaciones enteras
         * con la suma de las coordenadas (4 veces el centro).
         */
        {
        int sx4 = d->x[0] + d->x[1] + d->x[2] + d->x[3];
        int sy4 = d->y[0] + d->y[1] + d->y[2] + d->y[3];
        for (k = 0; k < nv; k++) {
            int vi = seq[k];
            int x4 = d->x[vi] * 4, y4 = d->y[vi] * 4;
#ifdef CUSA_NO_EXPAND
            float ex = 0.5f, ey = 0.5f;
#else
            float ex = x4 > sx4 ? 1.0f : x4 < sx4 ? 0.0f : 0.5f;
            float ey = y4 > sy4 ? 1.0f : y4 < sy4 ? 0.0f : 0.5f;
#endif
            pvr_vertex_t *v = pvr_dr_target();
            v->flags = k == nv - 1 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            v->x = ((float)d->x[vi] + ex) * SX;
            v->y = ((float)d->y[vi] + ey) * SY;
            v->z = z;
            v->u = (float)(d->uv[vi] & 0xFF) * iw + uo;
            v->v = (float)(d->uv[vi] >> 8) * ih + vo;
            v->argb = argb;
            if (list == PVR_LIST_TR_POLY && d->pad && !(d->c0 & 0x2000))
                v->argb = (argb & 0x00FFFFFFu) | ((uint32_t)(64 - d->pad) * 4 << 24);
            v->oargb = 0;
            pvr_dr_commit(v);
        }
        }
        vbytes += 4 * 32;
        stat_polys++;
    }
}

/* Sube la pantalla dibujada por la CPU a una textura. Va antes de abrir las
 * listas: pvr_txr_load usa las store queues, que dentro de una lista estan
 * apuntando al TA. */
static int upload_cpu_framebuffer(int page)
{
    const uint16_t *src = &vu.video[page ? 0x40000 : 0];
    int y, x, k;


    /* textura de 512x512 tomada del bloque de texturas mientras se usa */
    if (!cpu_fb_tex) {
        while (!(cpu_fb_tex = pool_alloc(512 * 512 * 2)))
            if (!evict_one())
                return 0;
    }

    /* conversion por bloques de 128 lineas (conv_buf es de 128 KB) */
    for (y = 0; y < 400; y += 128) {
        int rows = 400 - y < 128 ? 400 - y : 128;
        for (k = 0; k < rows; k++)
            for (x = 0; x < 512; x++) {
                uint16_t c = vu.coloram[src[(y + k) * 512 + x] & 0x7FFF];
                conv_buf[k * 512 + x] = (uint16_t)(((c & 0x7FE0) << 1) | (c & 0x1F));
            }
#ifdef CUSA_DIAG
        if (y == 256) {
            /* estado en vivo sobre la pantalla de arranque (diagnostico en
             * consola real: si los numeros avanzan, el juego no esta parado) */
            char line[64];
            snprintf(line, sizeof(line), "f%lu cd%lu err%lu %s", (unsigned long)wd_heartbeat,
                     (unsigned long)wd_counters[WD_CD_GFX], (unsigned long)wd_counters[WD_CD_ERR],
                     (const char *)wd_phase);
            memset(conv_buf + (352 - 256) * 512, 0, 24 * 512 * 2);
            tf_draw((uint16_t *)conv_buf + (352 - 256) * 512 + 8, 512, line, 0xFFFF, 0x0000);
        }
#endif
        vram_guard();
        pvr_txr_load(conv_buf, (uint8_t *)cpu_fb_tex + y * 1024, rows * 1024);
    }
    return 1;
}

static void draw_cpu_framebuffer(void)
{
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_vertex_t v;
    int k;
    static const float xs[4] = { 0, 640, 0, 640 }, ys[4] = { 0, 0, 480, 480 };
    static const float us[4] = { 0, 1, 0, 1 };

    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                     512, 512, cpu_fb_tex, PVR_FILTER_NONE);
    (void)0;
    cxt.txr.env = PVR_TXRENV_REPLACE;
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));
    for (k = 0; k < 4; k++) {
        v.flags = k == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v.x = xs[k];
        v.y = ys[k];
        v.z = 1.0f;
        v.u = us[k] * 1.0f;
        v.v = (ys[k] / 480.0f) * (400.0f / 512.0f);
        v.argb = 0xFFFFFFFFu;
        v.oargb = 0;
        pvr_prim(&v, sizeof(v));
    }
}


static int hold_frames, cpu_fb_ready;

/* ---- marcador de diagnostico para la consola real ----
 * Una linea abajo a la izquierda, actualizada cada segundo. En Flycast el
 * PowerVR no falla nunca, asi que hace falta ver los numeros en la tele:
 *   I imagenes/s  H imagenes retenidas/s  C texturas convertidas/s
 *   T esperas del TA agotadas (total)  V KB maximos del bufer de vertices
 *   R ms del ultimo dibujado  E errores del PowerVR (ISP sin memoria, strip,
 *   OPB sin memoria, entrada TA, desbordamiento TA, parametros sin memoria) */
#ifdef CUSA_DIAG
#define DIAG_W 512
#define DIAG_H 64
static pvr_ptr_t diag_tex;
static uint16_t diag_buf[DIAG_W * DIAG_H] __attribute__((aligned(32)));
static int diag_dirty;
static uint64_t diag_t;
static unsigned diag_pages, diag_holds;
static volatile uint32_t pvr_errs[6];

static void pvr_err_irq(uint32_t code, void *data)
{
    (void)data;
    if (code - ASIC_EVT_PVR_ISP_OUTOFMEM < 6)
        pvr_errs[code - ASIC_EVT_PVR_ISP_OUTOFMEM]++;
}

static void diag_init(void)
{
    uint32_t c;
    for (c = ASIC_EVT_PVR_ISP_OUTOFMEM; c <= ASIC_EVT_PVR_TA_INPUT_OVERFLOW; c++) {
        asic_evt_set_handler(c, pvr_err_irq, NULL);
        asic_evt_enable(c, ASIC_IRQ_DEFAULT);
    }
    diag_tex = pool_alloc(DIAG_W * DIAG_H * 2);
}

static void diag_update(void)
{
    uint64_t now = timer_ms_gettime64();
    pvr_stats_t st;
    char line[43], line2[43];                /* 42 caracteres caben en 512 px */
    if (now - diag_t < 1000)
        return;
    diag_t = now;
    memset(&st, 0, sizeof(st));
    pvr_get_stats(&st);
    snprintf(line, sizeof(line), "I%u H%u C%u T%lu V%u R%u E%lu.%lu.%lu.%lu.%lu.%lu",
             diag_pages, diag_holds, diag_conv, (unsigned long)wd_counters[WD_TA_TIMEOUT],
             (unsigned)(st.vtx_buffer_used_max / 1024), (unsigned)(st.rnd_last_time / 1000000),
             (unsigned long)pvr_errs[0], (unsigned long)pvr_errs[1], (unsigned long)pvr_errs[2],
             (unsigned long)pvr_errs[3], (unsigned long)pvr_errs[4], (unsigned long)pvr_errs[5]);
    /* segunda linea: lecturas de la ROM grafica / errores de CD, frames y fase */
    snprintf(line2, sizeof(line2), "D%lu/%lu F%lu %s", (unsigned long)wd_counters[WD_CD_GFX],
             (unsigned long)wd_counters[WD_CD_ERR], (unsigned long)wd_heartbeat, (const char *)wd_phase);
    diag_pages = diag_holds = diag_conv = 0;
    memset(diag_buf, 0, sizeof(diag_buf));
    {
        int i;
        for (i = 0; i < DIAG_W * DIAG_H; i++)
            diag_buf[i] = 0x8000;           /* negro opaco */
    }
    tf_draw(diag_buf + 4 * DIAG_W + 4, DIAG_W, line, 0xFFFF, 0x8000);
    tf_draw(diag_buf + 34 * DIAG_W + 4, DIAG_W, line2, 0xFFFF, 0x8000);
    diag_dirty = 1;
}

static void diag_upload(void)
{
    if (diag_dirty && diag_tex) {
        vram_guard();
        pvr_txr_load(diag_buf, diag_tex, sizeof(diag_buf));
        diag_dirty = 0;
    }
}

static void diag_draw(void)
{
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_vertex_t v;
    int k;
    static const float xs[4] = { 0, DIAG_W, 0, DIAG_W }, ys[4] = { 480 - DIAG_H, 480 - DIAG_H, 480, 480 };
    static const float us[4] = { 0, 1, 0, 1 }, vs[4] = { 0, 0, 1, 1 };
    if (!diag_tex)
        return;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED,
                     DIAG_W, DIAG_H, diag_tex, PVR_FILTER_NONE);
    cxt.txr.env = PVR_TXRENV_REPLACE;
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));
    for (k = 0; k < 4; k++) {
        v.flags = k == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v.x = xs[k];
        v.y = ys[k];
        v.z = 4.0f;                         /* por encima de todo */
        v.u = us[k];
        v.v = vs[k];
        v.argb = 0xFFFFFFFFu;
        v.oargb = 0;
        pvr_prim(&v, sizeof(v));
    }
}
#else
#define diag_upload() ((void)0)
#define diag_draw() ((void)0)
#endif

static int render_page(int page, int shot)
{
    unsigned skipped_now = 0;
    int n = page_npolys[page], i, m = 0, n_op = 0, n_pt = 0, n_tr = 0;

    uint64_t ta = timer_us_gettime64(), tb, tc;
    frame_no++;
    conv_this_frame = 0;
    texels_this_frame = 0;
    vram_full = 0;
    conv_unlimited = hold_frames >= HOLD_UNLIMIT;
    frame_is_menu = n < 1200;
    {
        /* textura de cada poligono y numero de orden por textura */
        uint16_t nseq[3] = { 1, 1, 1 };
        int l, total[3] = { 0, 0, 0 };
        static uint16_t used[3];
        /* solo se limpia lo que se uso en el frame anterior */
        for (l = 0; l < 3; l++)
            memset(seq_count[l], 0, (used[l] + 1u) * sizeof(seq_count[l][0]));
        for (i = 0; i < n; i++) {
            const packet *d = &page_polys[page][i];
            if (i + 4 < n)
                __builtin_prefetch(&page_polys[page][i + 4]);
            tex_entry *t = NULL;
            uint16_t sq = 0;
            l = 0;
            if ((d->c0 & 0x300) == 0x100) {
                t = get_texture(d);
                if (!t) {
                    stat_skipped++;
                    skipped_now++;
                    continue;
                }
                l = t->pt;
                if (t->seq_frame != frame_no) {
                    t->seq_frame = frame_no;
                    t->seq = nseq[l]++;
                }
                sq = t->seq;
            }
            /* DITHER (bit 13): el V-Unit pinta uno de cada dos pixeles, una
             * sombra o velo semitransparente; aqui va a la lista translucida
             * al 50 %, en el orden original */
            if ((d->c0 & 0x2000) || POLY_FADED(d)) {
                l = 2;
                sq = 0;
            }
            unsorted[m].d = d;
            unsorted[m].t = t;
            unsorted[m].z = 1.0f + (float)i * (1.0f / 4096.0f);
            seq_count[l][sq + 1]++;
            total[l]++;
            m++;
        }
        items[0] = items_buf;
        items[1] = items[0] + total[0];
        items[2] = items[1] + total[1];
        /* ordenacion por cuentas (estable) */
        for (l = 0; l < 3; l++)
            for (i = 1; i <= nseq[l]; i++)
                seq_count[l][i] += seq_count[l][i - 1];
        for (i = 0; i < m; i++) {
            tex_entry *t = unsorted[i].t;
            if ((unsorted[i].d->c0 & 0x2000) || POLY_FADED(unsorted[i].d))
                items[2][seq_count[2][0]++] = unsorted[i];
            else {
                l = t ? t->pt : 0;
                items[l][seq_count[l][t ? t->seq : 0]++] = unsorted[i];
            }
        }
        for (l = 0; l < 3; l++)
            used[l] = nseq[l];
#ifdef CUSA_OPBDBG
        {
            /* bloques de OPB que hacen falta: cada poligono es una tira con
             * un puntero en cada tile de 32x32 que toca (caja envolvente) */
            static uint16_t cnt[3][300];
            int li, k2, need = 0;
            memset(cnt, 0, sizeof(cnt));
            for (li = 0; li < 3; li++) {
                for (k2 = 0; k2 < (li == 0 ? seq_count[0][0] : 0) + 0; k2++) ;
            }
            for (i = 0; i < m; i++) {
                const packet *d = unsorted[i].d;
                int x0 = 9999, x1 = -9999, y0 = 9999, y1 = -9999, tx, ty, kk;
                li = (d->c0 & 0x2000) || POLY_FADED(d) ? 2 : (unsorted[i].t ? unsorted[i].t->pt : 0);
                for (kk = 0; kk < 4; kk++) {
                    int xx = (int)((d->x[kk] + 0.5f) * SX), yy = (int)((d->y[kk] + 0.5f) * SY);
                    if (xx < x0) x0 = xx; if (xx > x1) x1 = xx;
                    if (yy < y0) y0 = yy; if (yy > y1) y1 = yy;
                }
                if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > 639) x1 = 639; if (y1 > 479) y1 = 479;
                for (ty = y0 / 32; ty <= y1 / 32; ty++)
                    for (tx = x0 / 32; tx <= x1 / 32; tx++)
                        cnt[li][ty * 20 + tx]++;
            }
            for (li = 0; li < 3; li++)
                for (k2 = 0; k2 < 300; k2++)
                    if (cnt[li][k2] > 15)
                        need += (cnt[li][k2] - 1) / 15;
            if (need > dbg_opb_max) dbg_opb_max = need;
        }
#endif
        n_op = total[0];
        n_pt = total[1];
        n_tr = total[2];
    }
    tb = timer_us_gettime64();
    t_tex += tb - ta;

    /* Si faltan texturas (al empezar una escena se convierten muchas y hay
     * un limite por frame), se deja en pantalla la imagen anterior en vez de
     * mostrar trozos; como mucho unos 30 frames seguidos. */
    if (skipped_now && hold_frames < 30) {
        hold_frames++;
#ifdef CUSA_DIAG
        diag_holds++;
#endif
        return 0;
    }
    hold_frames = 0;

    wd_phase = "PowerVR: esperando al TA";
    if (pvr_wait_ready() < 0) {
        stat_ta_timeouts++;
        wd_counters[WD_TA_TIMEOUT]++;
    }
    wd_phase = "PowerVR: enviando poligonos";
    tc = timer_us_gettime64();
    t_wait += tc - tb;
    if (shot && !shot_tex) {
        while (!(shot_tex = pool_alloc(640 * 480 * 2)))
            if (!evict_one())
                break;
    }
    /* la textura de la pantalla de la CPU se sube antes de empezar el frame */
    cpu_fb_ready = n == 0 && upload_cpu_framebuffer(page);
    diag_upload();
    if (shot && shot_tex) {
        pvr_scene_begin_rtt(shot_tex, 640, 480, 640);
    } else {
        pvr_scene_begin();
    }
    vbytes = 0;
    pvr_list_begin(PVR_LIST_OP_POLY);
    if (n == 0) {
        if (cpu_fb_ready)
            draw_cpu_framebuffer();
    } else {
        if (cpu_fb_tex) {           /* ya no hace falta: devolver al bloque */
            pool_free(cpu_fb_tex, 512 * 512 * 2);
            cpu_fb_tex = NULL;
        }
        submit_list(PVR_LIST_OP_POLY, items[0], n_op);
    }
    diag_draw();
    pvr_list_finish();
    pvr_list_begin(PVR_LIST_PT_POLY);
    submit_list(PVR_LIST_PT_POLY, items[1], n_pt);
    pvr_list_finish();
    pvr_list_begin(PVR_LIST_TR_POLY);
    submit_list(PVR_LIST_TR_POLY, items[2], n_tr);
    pvr_list_finish();
    pvr_scene_finish();
    t_sub += timer_us_gettime64() - tc;
    return 1;
}

/* Imprime la imagen reducida a 160x120 por el puerto serie (depuracion). */
static void dump_shot(void)
{
    const uint16_t *p;
    int x, y;
#if defined(CUSA_SHOT_CROP)
    /* recorte central de 320x192 a resolucion completa (horizonte) */
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static char line[640 / 3 * 4 + 40];

    pvr_wait_ready();
    pvr_wait_ready();
    p = (const uint16_t *)shot_tex;
    for (y = 0; y < 192; y++) {
        const uint8_t *b = (const uint8_t *)(p + (y + 112) * 640 + 160);
        char *o = line + sprintf(line, "SHOTC %03d ", y);
        for (x = 0; x + 2 < 640; x += 3) {
            uint32_t v = b[x] << 16 | b[x + 1] << 8 | b[x + 2];
            *o++ = b64[v >> 18]; *o++ = b64[(v >> 12) & 63];
            *o++ = b64[(v >> 6) & 63]; *o++ = b64[v & 63];
        }
        {   /* 640 = 3*213 + 1 */
            uint32_t v = b[639] << 16;
            *o++ = b64[v >> 18]; *o++ = b64[(v >> 12) & 63]; *o++ = '='; *o++ = '=';
        }
        *o = 0;
        puts(line);
    }
#elif defined(CUSA_SHOT_FULL)
    /* 640x480 completos, cada linea en base64 (RGB565 little endian) */
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static char line[1280 / 3 * 4 + 32];

    pvr_wait_ready();
    pvr_wait_ready();
    p = (const uint16_t *)shot_tex;
    for (y = 0; y < 480; y++) {
        const uint8_t *b = (const uint8_t *)(p + y * 640);
        char *o = line + sprintf(line, "SHOTF %03d ", y);
        for (x = 0; x + 2 < 1280; x += 3) {
            uint32_t v = b[x] << 16 | b[x + 1] << 8 | b[x + 2];
            *o++ = b64[v >> 18]; *o++ = b64[(v >> 12) & 63];
            *o++ = b64[(v >> 6) & 63]; *o++ = b64[v & 63];
        }
        {   /* 1280 = 3*426 + 2 */
            uint32_t v = b[1278] << 16 | b[1279] << 8;
            *o++ = b64[v >> 18]; *o++ = b64[(v >> 12) & 63]; *o++ = b64[(v >> 6) & 63]; *o++ = '=';
        }
        *o = 0;
        puts(line);
    }
#else
    char line[160 * 4 + 16];

    pvr_wait_ready();
    pvr_wait_ready();
    p = (const uint16_t *)shot_tex;
    for (y = 0; y < 120; y++) {
        char *o = line;
        o += sprintf(o, "SHOT %03d ", y);
        for (x = 0; x < 160; x++)
            o += sprintf(o, "%04X", p[(y * 4) * 640 + x * 4]);
        puts(line);
    }
#endif
}

int pvrr_init(void)
{
    /*
     * En carrera hay hasta ~3000 poligonos por frame y hasta varios cientos
     * en un mismo tile de 32x32. Flycast no limita nada, pero el PowerVR real
     * escribe los vertices en un bufer de tamano fijo y las listas de objetos
     * por tile (OPB) en bloques: si se desbordan, el frame sale corrupto o el
     * TA se cuelga. Bufer de vertices de 640 KB (el maximo visto en carrera
     * son ~420 KB; ademas submit_list no se pasa) y 4 bloques extra de OPB
     * (unas 72.000 entradas; en carrera hacen falta 7.000-14.000). Va doble,
     * asi que cada KB de mas se lo quita a las texturas: con 1 MB y 8 bloques
     * el bloque de texturas bajo de 5,5 a 3,9 MB y el modo demo se quedaba
     * reconvirtiendo texturas sin parar.
     */
    /* translucidos sin ordenar en el PowerVR (autosort desactivado): ya van
     * en el orden del original y la Z crece con el orden; ordenar por pixel
     * cuesta tiempo de dibujado en la consola real */
    /* Desbordamiento de los bloques de punteros por tile: 10 juegos extra
     * (antes 4). En consola real el PowerVR daba miles de errores "OPB sin
     * memoria" en carrera; Flycast no tiene ese limite. Se probo con bloques
     * de 32 palabras y la consola se quedaba en la pantalla de carga, asi
     * que se vuelve a 16 (con los que si arrancaba) con mas desbordamiento. */
    pvr_init_params_t params = {
        { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16 },
        VERTEX_BUF, 0, 0, 1, 10, 0
    };
    pvr_init(&params);
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    printf("VRAM libre para texturas: %u KB\n", (unsigned)(pvr_mem_available() / 1024));
    page_polys[0] = malloc(sizeof(packet) * MAX_POLYS);
    page_polys[1] = malloc(sizeof(packet) * MAX_POLYS);
    if (!page_polys[0] || !page_polys[1])
        return -1;
    pool_init();
#ifdef CUSA_DIAG
    diag_init();
#endif
    printf("texturas: bloque de %u KB\n", (unsigned)(pool_bytes / 1024));
    vu_poly_hook = on_poly;
    vu_fifo_reset_hook = on_fifo_reset;
    return 0;
}

/* Reserva la textura de captura antes de que el bloque se fragmente */
void pvrr_reserve_shot(void)
{
    if (!shot_tex)
        shot_tex = pool_alloc(640 * 480 * 2);
}

/* ---- pantalla de carga ----
 * Desde el arranque hasta el primer frame con poligonos, en vez de las
 * pantallas de texto del arcade (DOWNLOADING TEXTURES...) se muestra el logo
 * y "NOW LOADING" (generated/loading.bin, sacado de la ROM). */
static const uint16_t *loading_img;
static pvr_ptr_t loading_logo, loading_text;
static int loading_shown;

void pvrr_set_loading(const uint16_t *img)
{
    loading_img = img;
}

static void loading_quad(pvr_ptr_t tex, int tw, int th, float x0, float y0, float x1, float y1, float vmax)
{
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_vertex_t v;
    int k;
    const float xs[4] = { x0, x1, x0, x1 }, ys[4] = { y0, y0, y1, y1 };
    const float us[4] = { 0, 1, 0, 1 }, vs[4] = { 0, 0, vmax, vmax };
    pvr_poly_cxt_txr(&cxt, PVR_LIST_PT_POLY, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED,
                     tw, th, tex, PVR_FILTER_NONE);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));
    for (k = 0; k < 4; k++) {
        v.flags = k == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v.x = xs[k];
        v.y = ys[k];
        v.z = 1.0f;
        v.u = us[k];
        v.v = vs[k];
        v.argb = 0xFFFFFFFFu;
        v.oargb = 0;
        pvr_prim(&v, sizeof(v));
    }
}

/* 1 si se ha dibujado (o ya estaba) la pantalla de carga */
static void loading_bar(float frac)
{
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_vertex_t v;
    int q, k;
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));
    for (q = 0; q < 2; q++) {
        float x0 = 220.0f, x1 = q ? 220.0f + 200.0f * frac : 420.0f;
        float y0 = 418.0f, y1 = 424.0f;
        const float xs[4] = { x0, x1, x0, x1 }, ys[4] = { y0, y0, y1, y1 };
        for (k = 0; k < 4; k++) {
            v.flags = k == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            v.x = xs[k];
            v.y = ys[k];
            v.z = q ? 1.2f : 1.1f;
            v.u = v.v = 0;
            v.argb = q ? 0xFF6890D8u : 0xFF202838u;     /* azul de la tipografia */
            v.oargb = 0;
            pvr_prim(&v, sizeof(v));
        }
    }
}

static void loading_done(void);

static int draw_loading(void)
{
    static uint32_t frames;
    if (!loading_img)
        return 0;
    /* si en 15 s no aparece ningun poligono, se vuelven a ver las pantallas
     * de texto del arcade (por si el juego esta parado en un aviso) */
    if (++frames > 57 * 15) {
        loading_done();
        return 0;
    }
    /* se redibuja cada cuarto de segundo con la barra de progreso */
    if (loading_shown && frames % 15 && !(pending_shot && shot_tex))
        return 1;
    if (!loading_logo) {
        loading_logo = pool_alloc(256 * 256 * 2);
        loading_text = pool_alloc(512 * 64 * 2);
        if (!loading_logo || !loading_text)
            return 0;
        pvr_wait_ready();
        pvr_txr_load(loading_img, loading_logo, 256 * 256 * 2);
        pvr_txr_load(loading_img + 256 * 256, loading_text, 512 * 64 * 2);
    }
    pvr_wait_ready();
    if (pending_shot && shot_tex)
        pvr_scene_begin_rtt(shot_tex, 640, 480, 640);
    else
        pvr_scene_begin();
    /* Sin la linea de diagnostico en la pantalla de carga: con ella la
     * version DIAG del 15/10 no arrancaba en la consola real (la del 12/10,
     * sin ella, si). */
    pvr_list_begin(PVR_LIST_OP_POLY);
    {
        /* la ROM grafica que lee el arranque: unas 56 paginas de 32 KB */
        float f = (float)wd_counters[WD_CD_GFX] / 56.0f;
        loading_bar(f > 1.0f ? 1.0f : f);
    }
    pvr_list_finish();
    pvr_list_begin(PVR_LIST_PT_POLY);
    /* logo 254x204 del arcade a la escala del port (1,25 x 1,2), centrado */
    loading_quad(loading_logo, 256, 256, 160.0f, 70.0f, 480.0f, 314.8f, 204.0f / 256.0f);
    loading_quad(loading_text, 512, 64, 64.0f, 340.0f, 576.0f, 404.0f, 1.0f);
    pvr_list_finish();
    pvr_list_begin(PVR_LIST_TR_POLY);
    pvr_list_finish();
    pvr_scene_finish();
    if (pending_shot && shot_tex) {
        pending_shot = 0;
        dump_shot();
        loading_shown = 0;                  /* volver a dibujarla en pantalla */
        return 1;
    }
    loading_shown = 1;
    return 1;
}

static void loading_done(void)
{
    printf("pantalla de carga: fin, %lu lecturas de la ROM grafica\n", (unsigned long)wd_counters[WD_CD_GFX]);
    if (loading_logo) {
        pvr_wait_ready();
        pvr_wait_render_done();
        pool_free(loading_logo, 256 * 256 * 2);
        pool_free(loading_text, 512 * 64 * 2);
        loading_logo = loading_text = NULL;
    }
    free((void *)loading_img);
    loading_img = NULL;
}

void pvrr_frame(int shot)
{
#ifdef CUSA_DIAG
    diag_update();
#endif
    static int still, seen_polys;
    int shown = vu.page_control & 1;
    pending_shot |= shot;
    if (!seen_polys && loading_img) {
        if (page_npolys[0] || page_npolys[1]) {
            seen_polys = 1;
            loading_done();
        } else if (draw_loading()) {
            last_shown = shown;
            return;
        }
    }
    if (shown == last_shown) {
        /* Arranque: el juego escribe textos en la memoria de video sin
         * poligonos ni cambio de pagina; se muestran cada medio segundo hasta
         * que aparece el primer poligono. */
        if (page_npolys[0] || page_npolys[1])
            seen_polys = 1;
        if (seen_polys || ++still < 28)
            return;
    }
    still = 0;
    shot = pending_shot;
    pending_shot = 0;
    last_shown = shown;
    if (!render_page(shown, shot)) {
        pending_shot = shot;            /* imagen retenida: capturar la siguiente */
        page_npolys[shown] = 0;
        return;
    }
    stat_pages++;
#ifdef CUSA_DIAG
    diag_pages++;
#endif
    page_npolys[shown] = 0;
    if (shot && shot_tex)
        dump_shot();
}

unsigned pvrr_pages(void)
{
    unsigned n = stat_pages;
    stat_pages = 0;
    return n;
}

void pvrr_stats(unsigned *conv, unsigned *polys)
{
#ifdef CUSA_STATS
    printf("  render: texturas %u ms, espera TA %u ms (%u agotadas), espera dibujo %u ms, envio %u ms, omitidos %u (huecos %u, limite %u/%u, vram %u)\n",
           (unsigned)(t_tex / 1000), (unsigned)(t_wait / 1000), stat_ta_timeouts, (unsigned)(t_rwait / 1000),
           (unsigned)(t_sub / 1000), stat_skipped, stat_why[0], stat_why[1], stat_why[2], stat_why[3]);
#ifdef CUSA_TEXDBG
    {
        unsigned i, used = 0, nt = 0;
        for (i = 0; i < TEX_SLOTS; i++)
            if (texs[i].ptr) { used += (unsigned)texs[i].w * texs[i].h * 2 / 1024; nt++; }
        printf("  texdbg: nuevas %u bloque %u paleta %u expulsadas %u, %u Ktexels convertidos; en VRAM %u texturas %u KB de %u\n",
               dbg_new, dbg_blk, dbg_pal, dbg_evict, dbg_ktex, nt, used, (unsigned)(pool_bytes / 1024));
    }
    dbg_new = dbg_blk = dbg_pal = dbg_evict = dbg_ktex = 0;
#endif
    printf("  render: diminutos descartados %u, triangulos planos %u\n", stat_tiny, stat_thin);
    stat_thin = 0;
#ifdef CUSA_OPBDBG
    printf("  opb: bloques extra necesarios (max) %d de %d\n", dbg_opb_max, 4 * 3 * 300);
    dbg_opb_max = 0;
#endif
    stat_tiny = 0;
    if (stat_vfull)
        printf("  render: bufer de vertices lleno %u veces\n", stat_vfull);
#endif
#ifdef CUSA_POLYDBG
    printf("  polydbg: max %d, >2048 %u, >8192 %u\n", dbg_maxc, dbg_big, dbg_huge);
    dbg_maxc = 0; dbg_big = dbg_huge = 0;
#endif
    stat_ta_timeouts = stat_vfull = 0;
    t_rwait = 0;
    memset(stat_why, 0, sizeof(stat_why));
    stat_skipped = 0;
    t_tex = t_wait = t_sub = 0;
    *conv = stat_conv;
    *polys = stat_polys;
    stat_conv = stat_polys = 0;
}
