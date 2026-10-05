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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../vunit/mem.h"
#include "pvr_render.h"
#include "watchdog.h"

#define MAX_POLYS   4096
#define VERTEX_BUF  (640 * 1024)
#define TEX_SLOTS   1024
#define SX          1.25f       /* 512 -> 640 */
#define SY          1.2f        /* 400 -> 480 */

typedef struct {
    uint32_t d[15];
} packet;

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
static unsigned conv_this_frame;
#define MAX_CONV_PER_FRAME 400
#define MAX_TEXELS_PER_FRAME (1024 * 1024)
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

static int evict_one(void)
{
    int i, best = -1;
    for (i = 0; i < TEX_SLOTS; i++) {
        if (texs[i].key && texs[i].ptr && texs[i].last_used != frame_no &&
            (best < 0 || texs[i].last_used < texs[best].last_used))
            best = i;
    }
    if (best >= 0) {
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

static int convert(tex_entry *t, const tex_req *r, uint32_t pixdata)
{
    const uint8_t *src = vu.texram;
    const uint16_t *cram = &vu.coloram[(r->pal & 0x7F) << 8];
    uint16_t nzr_col = vu.coloram[pixdata & 0x7FFF];
    uint32_t x, y, n = t->w * t->h;
    uint16_t *o = conv_buf;

    uint16_t lut[256];
    uint32_t i;

    /* tabla de los 256 valores de texel ya en ARGB1555 */
    for (i = 0; i < 256; i++) {
        if (r->nzr)
            lut[i] = i ? argb1555(nzr_col, 1) : argb1555(0, !r->zs);
        else if (r->zs && i == 0)
            lut[i] = 0;
        else
            lut[i] = argb1555(cram[i], 1);
    }
    for (y = 0; y < t->h; y++) {
        const uint8_t *row = src + ((r->base * 256 + ((r->v0 + y) & 0xFF) * 256) & (sizeof(vu.texram) - 1));
        if (r->u0 + t->w <= 256) {
            const uint8_t *p = row + r->u0;
            for (x = 0; x < t->w; x += 4) {
                o[0] = lut[p[0]]; o[1] = lut[p[1]];
                o[2] = lut[p[2]]; o[3] = lut[p[3]];
                o += 4; p += 4;
            }
        } else {
            for (x = 0; x < t->w; x++)
                *o++ = lut[row[(r->u0 + x) & 0xFF]];
        }
    }
    if (!t->ptr) {
        while (!(t->ptr = pool_alloc(n * 2))) {
            if (!evict_one()) {
                conv_this_frame = MAX_CONV_PER_FRAME;   /* VRAM llena: basta por hoy */
                return 0;
            }
        }
    }
    vram_guard();
    pvr_txr_load(conv_buf, t->ptr, n * 2);
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

static tex_entry *get_texture_slow(const uint32_t *d);

static inline tex_entry *get_texture(const uint32_t *d)
{
    uint32_t a = (d[14] & 0x7FFF) | ((d[1] & 0x7F00) << 8);
    uint32_t b = d[0] & 0xCFF;
    uint32_t c = (d[10] & 0xFFFF) | (d[11] << 16);
    uint32_t e = (d[12] & 0xFFFF) | (d[13] << 16);
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

static tex_entry *get_texture_slow(const uint32_t *d)
{
    tex_req r;
    uint32_t umin = 255, umax = 0, vmin = 255, vmax = 0, slot, k, g, pg;
    uint64_t key;
    tex_entry *t;

    for (k = 0; k < 4; k++) {
        uint32_t u = d[10 + k] & 0xFF, v = (d[10 + k] >> 8) & 0xFF;
        if (u < umin) umin = u;
        if (u > umax) umax = u;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    r.base = d[14] & 0x7FFF;
    r.pal = (d[1] >> 8) & 0x7F;
    r.zs = (d[0] & 0x800) != 0;
    r.nzr = (d[0] & 0x400) != 0;
    r.color = r.nzr ? (d[0] & 0xFF) : 0;
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
        uint32_t pixdata = (d[1] & 0xFF00) | (d[0] & 0xFF);
        /* limite de conversiones por frame: se reutiliza la version anterior */
        if ((conv_this_frame >= MAX_CONV_PER_FRAME || texels_this_frame >= MAX_TEXELS_PER_FRAME) && t->ptr) {
            t->last_used = frame_no;
            return t;
        }
        if (conv_this_frame >= MAX_CONV_PER_FRAME || texels_this_frame >= MAX_TEXELS_PER_FRAME ||
            !convert(t, &r, pixdata)) {
            stat_why[conv_this_frame >= MAX_CONV_PER_FRAME ? 1 : texels_this_frame >= MAX_TEXELS_PER_FRAME ? 2 : 3]++;
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
    if (page_npolys[page] < MAX_POLYS)
        memcpy(page_polys[page][page_npolys[page]++].d, d, sizeof(packet));
}

/* ---- envio al PVR ---- */

typedef struct {
    const uint32_t *d;
    tex_entry *t;
    float z;
} draw_item;

/* Poligonos del frame ordenados por textura (orden de primer uso), por
 * separado para la lista opaca y la punch-through. El orden de dibujo del
 * original lo mantiene la Z. */
static draw_item items[3][MAX_POLYS];   /* opaca, punch-through, tramada */
static draw_item unsorted[MAX_POLYS];
static uint16_t seq_count[3][MAX_POLYS + 1];

static uint32_t vbytes;                /* bytes enviados al TA este frame */
static unsigned stat_vfull;

static void submit_list(pvr_list_t list, const draw_item *it, int n)
{
    int i, k;
    tex_entry *cur = (tex_entry *)1;
    uint32_t cur_col = 0xFFFFFFFFu;
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    static const int order[4] = { 0, 1, 3, 2 };

    for (i = 0; i < n; i++, it++) {
        const uint32_t *d = it->d;
        tex_entry *t = it->t;
        /* no pasarse del bufer de vertices (cabecera + 4 vertices) */
        if (vbytes + 5 * 32 > VERTEX_BUF - 4096) {
            stat_vfull++;
            wd_counters[WD_VBUF_FULL]++;
            break;
        }
        uint32_t argb = 0xFFFFFFFFu;
        float uo = 0, vo = 0, iw = 0, ih = 0, z = it->z;

        if (!t) {
            uint16_t c = vu.coloram[((d[1] & 0xFF00) | (d[0] & 0xFF)) & 0x7FFF];
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
        for (k = 0; k < 4; k++) {
            int vi = order[k];
            pvr_vertex_t *v = pvr_dr_target();
            v->flags = k == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            v->x = ((float)(int16_t)d[2 + vi * 2] + 0.5f) * SX;
            v->y = ((float)(int16_t)d[3 + vi * 2] + 0.5f) * SY;
            v->z = z;
            v->u = (float)(d[10 + vi] & 0xFF) * iw + uo;
            v->v = (float)((d[10 + vi] >> 8) & 0xFF) * ih + vo;
            v->argb = argb;
            v->oargb = 0;
            pvr_dr_commit(v);
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

static int render_page(int page, int shot)
{
    unsigned skipped_now = 0;
    int n = page_npolys[page], i, m = 0, n_op = 0, n_pt = 0, n_tr = 0;

    uint64_t ta = timer_us_gettime64(), tb, tc;
    frame_no++;
    conv_this_frame = 0;
    texels_this_frame = 0;
    {
        /* textura de cada poligono y numero de orden por textura */
        uint16_t nseq[3] = { 1, 1, 1 };
        int l, total[3] = { 0, 0, 0 };
        memset(seq_count, 0, sizeof(seq_count));
        for (i = 0; i < n; i++) {
            const uint32_t *d = page_polys[page][i].d;
            tex_entry *t = NULL;
            uint16_t sq = 0;
            l = 0;
            if ((d[0] & 0x300) == 0x100) {
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
            if (d[0] & 0x2000) {
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
        /* ordenacion por cuentas (estable) */
        for (l = 0; l < 3; l++)
            for (i = 1; i <= nseq[l]; i++)
                seq_count[l][i] += seq_count[l][i - 1];
        for (i = 0; i < m; i++) {
            tex_entry *t = unsorted[i].t;
            if (unsorted[i].d[0] & 0x2000)
                items[2][seq_count[2][0]++] = unsorted[i];
            else {
                l = t ? t->pt : 0;
                items[l][seq_count[l][t ? t->seq : 0]++] = unsorted[i];
            }
        }
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
    if (shot && shot_tex) {
        pvr_scene_begin_rtt(shot_tex, 640, 480, 640);
    } else {
        pvr_scene_begin();
    }
    vbytes = 0;
    cpu_fb_ready = n == 0 && upload_cpu_framebuffer(page);
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
#ifdef CUSA_SHOT_FULL
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

void pvrr_init(void)
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
    pvr_init_params_t params = {
        { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16 },
        VERTEX_BUF, 0, 0, 0, 4, 0
    };
    pvr_init(&params);
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    printf("VRAM libre para texturas: %u KB\n", (unsigned)(pvr_mem_available() / 1024));
    page_polys[0] = malloc(sizeof(packet) * MAX_POLYS);
    page_polys[1] = malloc(sizeof(packet) * MAX_POLYS);
    pool_init();
    printf("texturas: bloque de %u KB\n", (unsigned)(pool_bytes / 1024));
    vu_poly_hook = on_poly;
    vu_fifo_reset_hook = on_fifo_reset;
}

/* Reserva la textura de captura antes de que el bloque se fragmente */
void pvrr_reserve_shot(void)
{
    if (!shot_tex)
        shot_tex = pool_alloc(640 * 480 * 2);
}

void pvrr_frame(int shot)
{
    static int still, seen_polys;
    int shown = vu.page_control & 1;
    pending_shot |= shot;
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
    printf("  render: texturas %u ms, espera TA %u ms (%u agotadas), espera dibujo %u ms, envio %u ms, omitidos %u (huecos %u, limite %u/%u, vram %u)\n",
           (unsigned)(t_tex / 1000), (unsigned)(t_wait / 1000), stat_ta_timeouts, (unsigned)(t_rwait / 1000),
           (unsigned)(t_sub / 1000), stat_skipped, stat_why[0], stat_why[1], stat_why[2], stat_why[3]);
    if (stat_vfull)
        printf("  render: bufer de vertices lleno %u veces\n", stat_vfull);
    stat_ta_timeouts = stat_vfull = 0;
    t_rwait = 0;
    memset(stat_why, 0, sizeof(stat_why));
    stat_skipped = 0;
    t_tex = t_wait = t_sub = 0;
    *conv = stat_conv;
    *polys = stat_polys;
    stat_conv = stat_polys = 0;
}
