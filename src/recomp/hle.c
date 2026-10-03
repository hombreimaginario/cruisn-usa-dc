/*
 * hle.c - Rutinas del juego sustituidas por versiones nativas.
 *
 * Cada rutina reproduce exactamente el efecto del codigo original sobre
 * registros y memoria (lo verifica cusa_difftest contra el interprete), pero
 * sin pasar por la emulacion instruccion a instruccion.
 *
 * Descompresor LZW de texturas (COMP.ASM): consume ~75% del tiempo de CPU
 * del juego porque las secciones de textura se descomprimen continuamente
 * mientras se conduce.
 */
#include <stddef.h>

#include "rt.h"

/* Direcciones en la ROM L4.1 (ver docs/ANALISIS.md) */
#define LZW_TOPLP        0x00A317u   /* DECOMPRESS_TOPLP: tras FLUSH_CODE */
#define LZW_DONE         0x00A36Eu   /* DECOMPRESSX: fin del flujo */
#define LZW_LINEBUFFERI  0x00A2AFu   /* puntero a LINEBUFFER */
#define LZW_DECODESTKI   0x00A2AEu   /* puntero a DECODE_STACK */
#define LZW_PACIFY       0x00E5CDu   /* PACIFY_COUNT */
#define LZW_NEXT_BUMP    0x00E5CEu   /* NEXT_BUMP_CODE */
#define LZW_RPTB_RS      0x00A362u
#define LZW_RPTB_RE      0x00A367u

enum { R0, R1, R2, R3, R4, R5, R6, R7, AR0, AR1, AR2, AR3, AR4, AR5, AR6, AR7,
       DP, IR0, IR1, BK };

static inline uint32_t lsh(uint32_t v, uint32_t cnt)
{
    return rt_shift(v, cnt, 0, 0);
}

/* INPUT_BITS: siguiente codigo de AR6 bits desde *AR0 / AR7. */
static inline uint32_t input_bits(uint32_t *r)
{
    uint32_t r0 = r[AR6] + r[AR7], r1, r2;
    if ((int32_t)r0 <= 31) {
        r0 = lsh(RD(r[AR0]), r[AR7]);
        r1 = r[AR6] - 32;
        r0 = lsh(r0, r1);
        r[AR7] += r[AR6];
        r[R1] = r1;
        return r0;
    }
    r1 = lsh(RD(r[AR0]), r[AR7]);
    r[AR0]++;
    r0 = 32 - r[AR7];
    r2 = r[AR6] - r0;
    r[AR7] = r2;
    r0 = RD(r[AR0]);
    r2 -= 32;
    r0 = lsh(r0, r2);
    r2 = r[AR6] - 32;
    r1 = lsh(r1, r2);
    r[R1] = r1;
    r[R2] = r2;
    return r0 | r1;
}

/* PUTC: guarda la palabra en LINEBUFFER y vuelca 64 palabras a WAVERAM. */
static int putc_flush(uint32_t *r)
{
    uint32_t buf = RD(LZW_LINEBUFFERI), k, w;
    r[AR2] = buf + r[R7];
    r[R7]++;
    WR(r[AR2], r[IR1]);
    r[IR1] = 0;
    r[R3] = 0;
    if ((int32_t)r[R7] < 64)
        return 0;
    WR(LZW_PACIFY, RD(LZW_PACIFY) + 64);
    r[R7] = 0;
    for (k = 0; k < 64; k++) {
        w = RD(buf + k);
        WR(r[AR1]++, w);
        WR(r[AR1]++, w >> 16);
    }
    r[AR2] = 0;
    r[R2] = RD(buf + 64);           /* lectura final "dummy" de LDI *AR4,R2 */
    return 1;
}

static inline int put_byte(uint32_t *r, uint32_t v)
{
    v = lsh(v, r[R3]);
    r[R0] = v;
    r[IR1] |= v;
    r[R3] += 8;
    if ((int32_t)r[R3] >= 32)
        return putc_flush(r);
    return 0;
}

uint32_t hle_lzw_segment(void)
{
    uint32_t *r = C.r;
    uint32_t next = 0;
    int gie = 0, n;
    uint64_t work = 0;

    for (n = 0; n < 8; n++)
        SYNC_I(n);

    /* DECOMPRESS_TOPLP3 */
    r[BK] = 259;
    r[AR6] = 9;
    r[R0] = 511;
    WR(LZW_NEXT_BUMP, 511);
    r[R0] = input_bits(r);
    r[R5] = r[R0];
    if (r[R5] == 256) {
        next = LZW_DONE;
        goto out;
    }
    r[R6] = r[R5];
    gie |= put_byte(r, r[R0]);

    for (;;) {
        uint32_t stk;
        work++;
        r[R0] = input_bits(r);
        r[R4] = r[R0];
        if (r[R4] == 256) { next = LZW_DONE; break; }
        if (r[R4] == 258) { next = LZW_TOPLP; break; }
        if (r[R4] == 257) { r[AR6]++; continue; }

        r[DP] = 0;
        stk = RD(LZW_DECODESTKI);
        r[AR4] = stk;
        r[27] = 0;                                  /* RC */
        if ((int32_t)(r[R4] - r[BK]) < 0) {
            r[AR5] = r[R4];
        } else {
            WR(r[AR4]++, r[R6]);
            r[27] = 1;
            r[AR5] = r[R5];
        }
        while ((int32_t)(r[AR5] - 255) > 0) {
            r[AR5] += r[AR3];
            r[R0] = RD(r[AR5] + r[IR0]);
            WR(r[AR4]++, r[R0]);
            r[27]++;
            r[AR5] = RD(r[AR5]);
            work++;
        }
        r[R6] = r[AR5];
        r[R0] = r[AR5];
        /* RPTB: RC+1 iteraciones */
        for (;;) {
            gie |= put_byte(r, r[R0]);
            r[AR4]--;
            r[R0] = RD(r[AR4]);
            if ((int32_t)--r[27] < 0)
                break;
        }
        r[25] = LZW_RPTB_RS;
        r[26] = LZW_RPTB_RE;
        r[AR2] = r[BK] + r[AR3];
        WR(r[AR2], r[R5]);
        WR(r[AR2] + r[IR0], r[R6]);
        r[BK]++;
        r[R5] = r[R4];
    }
out:
    for (n = 0; n < 8; n++)
        C.rk[n] = 2;
    C.r[C3X_ST] &= ~C3X_ST_RM;
    if (gie) {
        C.r[C3X_ST] |= C3X_ST_GIE;    /* PUTC termina con RETI (ENABLEGIE) */
        RT_FORCE_CHECK();
    }
    /* Tiempo aproximado del original: ~40 ciclos por codigo */
    C.cyc += (uint32_t)(work * 40);
    return next;
}

/* ------------------------------------------------------------------------ */
/* Transformacion de vertices (DIRQ.ASM)                                     */
/* ------------------------------------------------------------------------ */

/* Vistas flotante/entera de R0-R7 dentro de las rutinas nativas: se leen
 * una vez al entrar y se escriben al salir. */
static inline float getf(int n)
{
    SYNC_F(n);
    return C.f[n];
}

static inline void setf(int n, float x)
{
    C.f[n] = x;
    C.rk[n] = 1;
}

#define MF(a)   c3x_to_float(RD(a))
#define SF(a, x) WR((a), float_to_c3x(x))

enum { RC_ = 27, RS_ = 25, RE_ = 26 };

/* Puntero nativo a un rango de memoria emulada (FASTRAM o ROM de
 * programa), o NULL si no esta entero en una de ellas. */
static inline const uint32_t *mem_ptr(uint32_t a, uint32_t words)
{
    a &= 0xFFFFFFu;
    if (a + words <= VU_FASTRAM_WORDS)
        return &vu.fastram[a];
    if (a - VU_PROGROM_BASE < VU_PROGRAM_WORDS && a + words - VU_PROGROM_BASE <= VU_PROGRAM_WORDS && vu.program)
        return &vu.program[a - VU_PROGROM_BASE];
    return NULL;
}

/* INVTAB (1/Z) convertida a flotante nativo; es una tabla constante. */
static float inv_cache[80 + 5000];
static uint32_t inv_cache_base = 0xFFFFFFFFu;

static const float *inv_table(uint32_t ar2)
{
    if (ar2 != inv_cache_base) {
        int i;
        for (i = 0; i < 80 + 5000; i++)
            inv_cache[i] = c3x_to_float(RD(ar2 - 80 + i));
        inv_cache_base = ar2;
    }
    return inv_cache + 80;
}

/*
 * Bucle RPTB 0x0141-0x0162 (NEXTOBJ): desempaqueta cada vertice (x,y de 16
 * bits con signo, z de 32), lo transforma con la matriz de camara, lo
 * proyecta con la tabla 1/Z (INVTAB) y guarda X, Y de pantalla y Z.
 * Mismas operaciones y en el mismo orden que el original; la matriz y la
 * tabla se leen una vez (el bucle no las modifica) y la zona temporal *AR4
 * solo se escribe con los valores de la ultima iteracion.
 */
void hle_vtx_dirq(void)
{
    uint32_t *r = C.r;
    float R0 = getf(0), R1 = getf(1), R2 = getf(2), R3 = getf(3);
    float R4 = getf(4), R5 = getf(5), R6 = getf(6), R7 = getf(7);
    uint32_t ar1 = r[AR1], ar2 = r[AR2], ar3 = r[AR3], ar4 = r[AR4], ar5 = r[AR5], ar6 = r[AR6];
    uint32_t bk = r[BK], ir1 = r[IR1];
    int32_t n = (int32_t)r[RC_];
    uint32_t iters = (uint32_t)(n + 1), k;
    const uint32_t *src = mem_ptr(ar1, iters * 2);
    const float *inv = inv_table(ar2);
    float m[9], tz = MF(ar6 + 1), tx = 0, ty = 0, tzv = 0;
    int sh = (int32_t)(bk << 25) >> 25;

    for (k = 0; k < 9; k++)
        m[k] = MF(ar5 + k);

    for (k = 0; k < iters; k++) {
        uint32_t w0 = src ? src[2 * k] : RD(ar1 + 2 * k);
        uint32_t w1 = src ? src[2 * k + 1] : RD(ar1 + 2 * k + 1);
        float x, y, z, sx;
        int32_t iz;

        if (sh == -16) {
            y = (float)((int32_t)w0 >> 16);
            x = (float)((int32_t)(w0 << 16) >> 16);
        } else {
            y = (float)(int32_t)rt_shift(w0, bk, 1, 0);
            x = (float)(int32_t)rt_shift(w0 << 16, bk, 1, 0);
        }
        z = (float)(int32_t)w1;
        tx = x; ty = y; tzv = z;
        {
            /* x = parte baja (R3), y = parte alta (R2), como en 0x142-0x146 */
            float X = (m[0] * x + m[1] * y) + m[2] * z;
            float Y = (m[3] * x + m[4] * y) + m[5] * z;
            float Z = ((m[6] * x + m[7] * y) + m[8] * z) + tz;
            float inv_z;
            iz = (int32_t)rt_fix(Z, 0) >> 4;
            if (iz >= 4999) iz = 4999;
            if (iz < -80) iz = -80;
            inv_z = inv[iz];
            R1 = X + R4;
            R3 = Y + R5;
            sx = inv_z * R1 + R6;
            R0 = (inv_z * R3) * 1.0400390625f + R7;
            R2 = Z;
            WR(ar3, float_to_c3x(sx));
            WR(ar3 + 1, float_to_c3x(R0));
            WR(ar3 + 2, float_to_c3x(Z));
            ar3 += 3;
            ir1 = (uint32_t)iz;
        }
    }
    /* zona temporal: valores de la ultima iteracion */
    SF(ar4 - 1, tx);
    SF(ar4, ty);
    SF(ar4 + 1, tzv);

    setf(0, R0); setf(1, R1); setf(2, R2); setf(3, R3);
    r[AR1] = ar1 + 2 * iters; r[AR3] = ar3; r[AR5] = ar5; r[IR1] = ir1;
    r[RC_] = 0xFFFFFFFFu;
    r[RS_] = 0x000141u;
    r[RE_] = 0x000162u;
    C.r[C3X_ST] &= ~C3X_ST_RM;
    FL_FLT(R0);
    C.cyc += iters * 34 - 34;           /* el bloque ya sumo una iteracion */
}

/*
 * Bucle RPTB 0x211D-0x2133: transformacion de vertices de modelo (mismo
 * empaquetado) mas traslacion; guarda X, Y, Z de camara.
 */
void hle_vtx_model(void)
{
    uint32_t *r = C.r;
    float R0 = getf(0), R1 = getf(1), R2 = getf(2), R3 = getf(3), R4 = getf(4);
    uint32_t ar3 = r[AR3], ar4 = r[AR4], ar5 = r[AR5], ar6 = r[AR6], ar7 = r[AR7];
    int32_t n = (int32_t)r[RC_];
    uint32_t iters = (uint32_t)(n + 1), k;
    const uint32_t *src = mem_ptr(ar4, iters * 2);
    float m[9], t0 = MF(ar6 - 1), t1 = MF(ar6), t2 = MF(ar6 + 1), x = 0, y = 0, z = 0;

    for (k = 0; k < 9; k++)
        m[k] = MF(ar5 + k);

    for (k = 0; k < iters; k++) {
        uint32_t w0 = src ? src[2 * k] : RD(ar4 + 2 * k);
        uint32_t w1 = src ? src[2 * k + 1] : RD(ar4 + 2 * k + 1);
        float ox, oy, oz;
        x = (float)((int32_t)(w0 << 16) >> 16);
        y = (float)((int32_t)w0 >> 16);
        z = (float)(int32_t)w1;
        R0 = x * m[0];
        R1 = y * m[1];
        ox = ((R1 + R0) + z * m[2]) + t0;
        R0 = x * m[3];
        R1 = y * m[4];
        oy = ((R1 + R0) + z * m[5]) + t1;
        R0 = x * m[6];
        R1 = y * m[7];
        R2 = R1 + R0;
        R1 = z * m[8];
        oz = (R2 + R1) + t2;
        WR(ar3, float_to_c3x(ox));
        WR(ar3 + 1, float_to_c3x(oy));
        WR(ar3 + 2, float_to_c3x(oz));
        ar3 += 3;
        R2 = oz;
        R3 = oy;
    }
    SF(ar7 - 1, x);
    SF(ar7, y);
    SF(ar7 + 1, z);
    R4 = y;

    setf(0, R0); setf(1, R1); setf(2, R2); setf(3, R3); setf(4, R4);
    r[AR3] = ar3; r[AR4] = ar4 + 2 * iters; r[AR5] = ar5;
    r[RC_] = 0xFFFFFFFFu;
    r[RS_] = 0x00211Du;
    r[RE_] = 0x002133u;
    C.r[C3X_ST] &= ~C3X_ST_RM;
    FL_FLT(R2);
    C.cyc += iters * 23 - 23;
}
