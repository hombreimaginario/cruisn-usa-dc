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
#include <math.h>
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

/* Conversion C3x -> IEEE en linea (la de c3x.h no siempre se expande). */
static inline __attribute__((always_inline)) float c2f(uint32_t v)
{
    union { uint32_t u; float f; } o;
    int32_t e = (int8_t)(v >> 24);
    uint32_t frac = v & 0x7FFFFFu;
    if (e + 127 <= 0)
        return 0.0f;
    if (!(v & 0x800000u)) {
        o.u = ((uint32_t)(e + 127) << 23) | frac;
    } else if (frac == 0) {
        if (e + 128 >= 255)
            o.u = 0xFF7FFFFFu;
        else
            o.u = 0x80000000u | ((uint32_t)(e + 128) << 23);
    } else {
        o.u = 0x80000000u | ((uint32_t)(e + 127) << 23) | (0x800000u - frac);
    }
    return o.f;
}

static inline __attribute__((always_inline)) float mf(uint32_t a)
{
    a &= 0xFFFFFFu;
    return c2f(LIKELY(a < VU_FASTRAM_WORDS) ? vu.fastram[a] : RD(a));
}

static inline __attribute__((always_inline)) uint32_t fixf(float x)
{
    int32_t i;
    if (UNLIKELY(x >= 2147483648.0f)) return 0x7FFFFFFFu;
    if (UNLIKELY(x < -2147483648.0f)) return 0x80000000u;
    i = (int32_t)x;
    if ((float)i > x)
        i--;
    return (uint32_t)i;
}

/* Lectura con la ROM de programa en linea: los modelos y sus poligonos
 * estan en ROM y RD() los mandaria por el camino lento. */
static inline __attribute__((always_inline)) uint32_t rdm(uint32_t a)
{
    a &= 0xFFFFFFu;
    if (LIKELY(a - VU_PROGROM_BASE < VU_PROGRAM_WORDS))
        return vu.program[a - VU_PROGROM_BASE];
    return RD(a);
}

#define MF(a)   mf(a)
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
/* Nucleo comun de los bucles 0x0141 y 0x0221: (x,y,z) del vertice, menos un
 * origen opcional, por la matriz de camara, mas traslacion en Z, proyeccion
 * con INVTAB. tz_addr: direccion de la traslacion Z; org: origen o NULL. */
static void vtx_project(uint32_t rs, uint32_t re, uint32_t tz_addr, const float *org, int imm_shift)
{
    uint32_t *r = C.r;
    float R0 = getf(0), R1 = getf(1), R2 = getf(2), R3 = getf(3);
    float R4 = getf(4), R5 = getf(5), R6 = getf(6), R7 = getf(7);
    uint32_t ar1 = r[AR1], ar2 = r[AR2], ar3 = r[AR3], ar4 = r[AR4], ar5 = r[AR5];
    uint32_t bk = r[BK], ir1 = r[IR1];
    int32_t n = (int32_t)r[RC_];
    uint32_t iters = (uint32_t)(n + 1), k;
    const uint32_t *src = mem_ptr(ar1, iters * 2);
    const float *inv = inv_table(ar2);
    float m[9], tz = MF(tz_addr), tx = 0, ty = 0, tzv = 0;
    int sh = imm_shift ? -16 : (int32_t)(bk << 25) >> 25;

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
        if (org) {
            x = x - org[0];
            y = y - org[1];
            z = z - org[2];
        }
        tx = x; ty = y; tzv = z;
        {
            /* x = parte baja (R3), y = parte alta (R2) */
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
    r[RS_] = rs;
    r[RE_] = re;
    C.r[C3X_ST] &= ~C3X_ST_RM;
    FL_FLT(R0);
    C.cyc += (iters - 1) * (re - rs + 1);  /* el bloque ya sumo una iteracion */
}

/* Bucle RPTB 0x0141-0x0162 (NEXTOBJ) */
void hle_vtx_dirq(void)
{
    vtx_project(0x000141u, 0x000162u, C.r[AR6] + 1, NULL, 0);
}

/* Bucle RPTB 0x0221-0x0246: igual pero restando el origen *+AR0(1..3) y con
 * la traslacion Z en *+AR7(1). */
void hle_vtx_world(void)
{
    float org[3];
    uint32_t ar0 = C.r[AR0];
    org[0] = MF(ar0 + 1);
    org[1] = MF(ar0 + 2);
    org[2] = MF(ar0 + 3);
    vtx_project(0x000221u, 0x000246u, C.r[AR7] + 1, org, 1);
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

/* ------------------------------------------------------------------------ */
/* Envio de poligonos a la FIFO (DIRQ.ASM, bucle RPTB 0x0521-0x054E)        */
/* ------------------------------------------------------------------------ */

static inline void seti(int n, uint32_t v)
{
    SYNC_I(n);
    C.r[n] = v;
    C.rk[n] = 2;
}

/*
 * Para cada poligono del modelo: lee los 4 indices de vertice, descarta
 * los que miran hacia atras y envia el paquete de 15 palabras con las
 * coordenadas de pantalla (FIX de los flotantes ya proyectados), las
 * coordenadas de textura y la direccion de textura. Se entra en 0x0521
 * (primera instruccion del RPTB) y se sale por uno de los dos RETSU.
 */
uint32_t hle_poly_emit(void)
{
    uint32_t *r = C.r;
    uint32_t ar1 = r[AR1], ir0 = r[IR0], ir1 = r[IR1], r7, r6;
    uint32_t r3 = 0, ar2 = r[AR2], ar3 = r[AR3], ar4 = 0, ar5 = r[AR5];
    float f0 = 0, f1 = 0, f2 = 0, x2, y2, x4, y4, x5, y5;
    uint32_t i0 = 0, i1 = 0, i2 = 0;
    uint32_t rc = r[RC_];
    uint64_t work = 0;
    int entered_skip = 0;

    SYNC_I(7);
    r7 = r[R7];
    SYNC_I(6);
    r6 = r[R6];
    {
        /* los registros de poligonos estan en ROM o FASTRAM: acceso directo */
    }
    for (;;) {
        /* 0521-0529 (en la ruta de descarte 0521-0522 ya se hicieron) */
        if (!entered_skip) {
            r3 = rdm(ar1 + 1);
            ar4 = r3 & r7;
        }
        entered_skip = 0;
        ar4 *= 3;
        r3 >>= 8;
        ar5 = (r3 & r7) * 3;
        r3 >>= 8;
        ar2 = (r3 & r7) * 3;
        /* 052A-052C: espera de FIFO (nunca llena en la emulacion) */
        x4 = MF(ar4 + ir0); y4 = MF(ar4 + ir1);
        x5 = MF(ar5 + ir0); y5 = MF(ar5 + ir1);
        x2 = MF(ar2 + ir0); y2 = MF(ar2 + ir1);
        f1 = x5 - x4;                                       /* 052D */
        f2 = y5 - y4;                                       /* 052E */
        f0 = x5 - x2;                                       /* 052F */
        {
            float a = y5 - y2;                              /* 0530 MPYF3 || SUBF3 */
            f0 = f2 * f0;
            f2 = a;
        }
        f2 = f2 * f1;                                       /* 0531 */
        f2 = f2 - f0;                                       /* 0532 */
        r3 >>= 8;                                           /* 0534 (ranura) */
        i1 = 3;                                             /* 0535 */
        work += 20;
        if (f2 > 0.0f) {
            /* 0533 BGTD 0550: cara trasera, se descarta */
            rc--;                                           /* 0550 */
            i0 = rc;                                        /* 0551 */
            ar1 += 6;                                       /* 0553 */
            r3 = rdm(ar1);
            ar1 -= 1;                                       /* 0554 */
            ar4 = r3 & r7;                                  /* 0555 */
            work += 6;
            if ((int32_t)rc >= 0) {                         /* 0552 BGED 0523 */
                entered_skip = 1;
                continue;
            }
            /* 0556 RETSU con RM aun activo (como el original) */
            r[RC_] = rc;
            seti(0, i0); setf(1, 0); seti(1, i1); setf(2, f2);
            seti(3, r3);
            r[AR1] = ar1; r[AR2] = ar2; r[AR3] = ar3; r[AR4] = ar4; r[AR5] = ar5;
            C.r[C3X_ST] = (C.r[C3X_ST] & ~0x1Fu) | rt_nz(i0);
            C.cyc += (uint32_t)work;
            return POP() & 0xFFFFFFu;
        }
        /* 0537-054E: paquete de 15 palabras, directo al motor de poligonos
         * (equivale a las 15 escrituras en la FIFO y FIFO_INC) */
        {
            uint32_t pkt[15];
            ar3 = r3 * 3;
            pkt[0] = rdm(ar1);
            ar1 += 2;
            pkt[1] = r6;                                    /* 053C ... || STI R6 */
            pkt[2] = fixf(x4);
            pkt[3] = fixf(y4);
            pkt[4] = fixf(x5);
            pkt[5] = fixf(y5);
            pkt[6] = fixf(x2);
            pkt[7] = fixf(y2);
            pkt[8] = fixf(MF(ar3 + ir0));
            pkt[9] = fixf(MF(ar3 + ir1));
            i0 = rdm(ar1); i1 = rdm(ar1 + 1); i2 = rdm(ar1 + 2);
            ar1 += 3;
            pkt[10] = i0;
            pkt[11] = i0 >> 16;
            pkt[12] = i1;
            pkt[13] = i1 >> 16;
            pkt[14] = i2;
            i0 >>= 16;
            i1 >>= 16;
            vu.fifo_count = 0;
            vu_poly_packet(pkt);                            /* 054E LDI @FIFO_INC */
            i0 = 0;
            work += 24;
        }
        if ((int32_t)--rc < 0)                              /* fin del RPTB */
            break;
    }
    /* 054F RETSU */
    r[RC_] = 0xFFFFFFFFu;
    C.r[C3X_ST] &= ~C3X_ST_RM;
    seti(0, 0); seti(1, i1); seti(2, i2); seti(3, r3);
    r[AR1] = ar1; r[AR2] = ar2; r[AR3] = ar3; r[AR4] = ar4; r[AR5] = ar5;
    C.r[C3X_ST] = (C.r[C3X_ST] & ~0x1Fu) | F_Z;
    C.cyc += (uint32_t)work;
    return POP() & 0xFFFFFFu;
}

/*
 * Variante de 0x0461-0x048F: igual que hle_poly_emit pero la palabra 1 sale
 * de una tabla de paletas: AR6 = BK + (w0 LSH R6), palabra = (*AR6 LSH R6)
 * << 8. El descarte de caras traseras sigue en 0x0491.
 */
uint32_t hle_poly_emit_pal(void)
{
    uint32_t *r = C.r;
    uint32_t ar1 = r[AR1], ir0 = r[IR0], ir1 = r[IR1], r7, r6, bk = r[BK];
    uint32_t r3 = 0, ar2 = r[AR2], ar3 = r[AR3], ar4 = 0, ar5 = r[AR5], ar6 = r[AR6];
    float f0 = 0, f1 = 0, f2 = 0, x2, y2, x4, y4, x5, y5;
    uint32_t i1 = 0, i2 = 0;
    uint32_t rc = r[RC_];
    uint32_t work = 0;
    int entered_skip = 0;

    SYNC_I(7);
    r7 = r[R7];
    SYNC_I(6);
    r6 = r[R6];
    for (;;) {
        if (!entered_skip) {                                /* 0461-0462 */
            r3 = rdm(ar1 + 1);
            ar4 = r3 & r7;
        }
        entered_skip = 0;
        ar4 *= 3;                                           /* 0463 */
        r3 >>= 8;
        ar5 = (r3 & r7) * 3;
        r3 >>= 8;
        ar2 = (r3 & r7) * 3;                                /* 0469 */
        x4 = MF(ar4 + ir0); y4 = MF(ar4 + ir1);
        x5 = MF(ar5 + ir0); y5 = MF(ar5 + ir1);
        x2 = MF(ar2 + ir0); y2 = MF(ar2 + ir1);
        f1 = x5 - x4;                                       /* 046D */
        f2 = y5 - y4;                                       /* 046E */
        f0 = x5 - x2;                                       /* 046F */
        {
            float a = y5 - y2;                              /* 0470 MPYF3 || SUBF3 */
            f0 = f2 * f0;
            f2 = a;
        }
        f2 = f2 * f1;                                       /* 0471 */
        f2 = f2 - f0;                                       /* 0472 */
        r3 >>= 8;                                           /* 0474 (ranura) */
        i1 = 3;                                             /* 0475 */
        work += 22;
        if (f2 > 0.0f) {
            /* 0473 BGTD 0491: cara trasera */
            rc--;                                           /* 0491 */
            ar1 += 6;                                       /* 0494 */
            r3 = rdm(ar1);
            ar1 -= 1;                                       /* 0495 */
            ar4 = r3 & r7;                                  /* 0496 */
            work += 6;
            if ((int32_t)rc >= 0) {                         /* 0493 BGED 0463 */
                entered_skip = 1;
                continue;
            }
            /* 0497 RETSU con RM aun activo */
            r[RC_] = rc;
            seti(0, rc); setf(1, f1); seti(1, i1); setf(2, f2);
            seti(3, r3);
            r[AR1] = ar1; r[AR2] = ar2; r[AR3] = ar3; r[AR4] = ar4; r[AR5] = ar5; r[AR6] = ar6;
            C.r[C3X_ST] = (C.r[C3X_ST] & ~0x1Fu) | rt_nz(rc);
            C.cyc += work;
            return POP() & 0xFFFFFFu;
        }
        {
            uint32_t pkt[15], w0, i0;
            w0 = rdm(ar1);                                   /* 0477 */
            ar1 += 2;
            ar3 = r3 * 3;                                   /* 0479 */
            ar6 = rt_shift(w0, r6, 0, 0) + bk;              /* 0478, 047A */
            pkt[0] = w0;
            pkt[1] = rt_shift(rdm(ar6), r6, 0, 0) << 8;      /* 047B-047C */
            pkt[2] = fixf(x4);
            pkt[3] = fixf(y4);
            pkt[4] = fixf(x5);
            pkt[5] = fixf(y5);
            pkt[6] = fixf(x2);
            pkt[7] = fixf(y2);
            pkt[8] = fixf(MF(ar3 + ir0));
            pkt[9] = fixf(MF(ar3 + ir1));
            i0 = rdm(ar1); i1 = rdm(ar1 + 1); i2 = rdm(ar1 + 2);
            ar1 += 3;
            pkt[10] = i0;
            pkt[11] = i0 >> 16;
            pkt[12] = i1;
            pkt[13] = i1 >> 16;
            pkt[14] = i2;
            i1 >>= 16;
            vu.fifo_count = 0;
            vu_poly_packet(pkt);                            /* 048F LDI @FIFO_INC */
            work += 25;
        }
        if ((int32_t)--rc < 0)                              /* fin del RPTB */
            break;
    }
    /* 0490 RETSU */
    r[RC_] = 0xFFFFFFFFu;
    C.r[C3X_ST] &= ~C3X_ST_RM;
    seti(0, 0); seti(1, i1); seti(2, i2); seti(3, r3);
    r[AR1] = ar1; r[AR2] = ar2; r[AR3] = ar3; r[AR4] = ar4; r[AR5] = ar5; r[AR6] = ar6;
    C.r[C3X_ST] = (C.r[C3X_ST] & ~0x1Fu) | F_Z;
    C.cyc += work;
    return POP() & 0xFFFFFFu;
}

/*
 * Variante de 0x0561-0x0595 para cuadrilateros: 4 indices de vertice y una
 * prueba de caras traseras doble. La segunda prueba hace un OR entero de
 * dos flotantes (0x057C) y mira el signo y si el primero es cero (LDF):
 * se descarta si ninguno de los dos productos es negativo y el primero no
 * es cero. Si AR3 == AR2 (triangulo) solo cuenta la primera prueba.
 */
uint32_t hle_poly_emit_quad(void)
{
    uint32_t *r = C.r;
    uint32_t ar1 = r[AR1], ir0 = r[IR0], ir1 = r[IR1], r7, r6;
    uint32_t r3 = 0, ar2 = r[AR2], ar3 = r[AR3], ar4 = 0, ar5 = r[AR5];
    float f0 = 0, f1 = 0, f2 = 0, f3 = 0, x2, y2, x3, y3, x4, y4, x5, y5;
    uint32_t i1 = 0, i2 = 0;
    uint32_t rc = r[RC_];
    uint32_t work = 0;
    int entered_skip = 0, r3_float = 0;

    SYNC_I(7);
    r7 = r[R7];
    SYNC_I(6);
    r6 = r[R6];
    for (;;) {
        int back;
        if (!entered_skip) {                                /* 0561-0562 */
            r3 = rdm(ar1 + 1);
            ar4 = r3 & r7;
        }
        entered_skip = 0;
        ar4 *= 3;                                           /* 0563 */
        r3 >>= 8;
        ar5 = (r3 & r7) * 3;
        r3 >>= 8;
        ar2 = (r3 & r7) * 3;
        r3 >>= 8;
        ar3 = (r3 & r7) * 3;                                /* 056C */
        x4 = MF(ar4 + ir0); y4 = MF(ar4 + ir1);
        x5 = MF(ar5 + ir0); y5 = MF(ar5 + ir1);
        x2 = MF(ar2 + ir0); y2 = MF(ar2 + ir1);
        y3 = MF(ar3 + ir1);
        f1 = x5 - x4;                                       /* 0570 */
        f3 = y5 - y4;                                       /* 0571 */
        f0 = x2 - x5;                                       /* 0572 */
        f0 = f3 * f0;                                       /* 0573 MPYF3 || SUBF3 */
        f2 = y2 - y5;
        f1 = f2 * f1;                                       /* 0574 */
        f0 = f0 - f1;                                       /* 0575 */
        back = f0 > 0.0f;                                   /* 0576 BGTD 0597 */
        {
            float a = y3 - y2;                              /* 0577 */
            float b = y4 - y3;                              /* 0578 MPYF3 || SUBF3 */
            f0 = f3 * a;
            f3 = b;
            r3_float = 1;
        }
        work += 26;
        if (!back && ar3 != ar2) {                          /* 0579-057A CMPI, BEQD 057F */
            f2 = f2 * f3;                                   /* 057B */
            /* 057C-057E: OR de las dos representaciones, LDF, BGT */
            back = !(f0 < 0.0f || f2 < 0.0f) && f0 != 0.0f;
            work += 1;
        } else if (!back) {
            f2 = f2 * f3;                                   /* ranura 057B */
        }
        if (back) {
            rc--;                                           /* 0597 */
            ar1 += 6;                                       /* 059A */
            r3 = rdm(ar1);
            r3_float = 0;
            ar1 -= 1;                                       /* 059B */
            ar4 = r3 & r7;                                  /* 059C */
            work += 6;
            if ((int32_t)rc >= 0) {                         /* 0599 BGED 0563 */
                entered_skip = 1;
                continue;
            }
            /* 059D RETSU con RM aun activo */
            r[RC_] = rc;
            seti(0, rc); setf(1, f1); setf(2, f2);
            seti(3, r3);
            r[AR1] = ar1; r[AR2] = ar2; r[AR3] = ar3; r[AR4] = ar4; r[AR5] = ar5;
            C.r[C3X_ST] = (C.r[C3X_ST] & ~0x1Fu) | rt_nz(rc);
            C.cyc += work;
            return POP() & 0xFFFFFFu;
        }
        {
            uint32_t pkt[15], i0;
            x3 = MF(ar3 + ir0);
            pkt[0] = rdm(ar1);                               /* 057F */
            ar1 += 2;
            pkt[1] = r6;                                    /* 0583 ... || STI R6 */
            pkt[2] = fixf(x4);
            pkt[3] = fixf(y4);
            pkt[4] = fixf(x5);
            pkt[5] = fixf(y5);
            pkt[6] = fixf(x2);
            pkt[7] = fixf(y2);
            pkt[8] = fixf(x3);
            pkt[9] = fixf(y3);
            i0 = rdm(ar1); i1 = rdm(ar1 + 1); i2 = rdm(ar1 + 2);
            ar1 += 3;
            pkt[10] = i0;
            pkt[11] = i0 >> 16;
            pkt[12] = i1;
            pkt[13] = i1 >> 16;
            pkt[14] = i2;
            i1 >>= 16;
            vu.fifo_count = 0;
            vu_poly_packet(pkt);                            /* 0595 LDI @FIFO_INC */
            work += 23;
        }
        if ((int32_t)--rc < 0)                              /* fin del RPTB */
            break;
    }
    /* 0596 RETSU */
    r[RC_] = 0xFFFFFFFFu;
    C.r[C3X_ST] &= ~C3X_ST_RM;
    seti(0, 0); seti(1, i1); seti(2, i2);
    if (r3_float)
        setf(3, f3);
    else
        seti(3, r3);
    r[AR1] = ar1; r[AR2] = ar2; r[AR3] = ar3; r[AR4] = ar4; r[AR5] = ar5;
    C.r[C3X_ST] = (C.r[C3X_ST] & ~0x1Fu) | F_Z;
    C.cyc += work;
    return POP() & 0xFFFFFFu;
}

/* ------------------------------------------------------------------------ */
/* Prueba de visibilidad de modelo (bucle RPTB 0x213E-0x216A)                */
/* ------------------------------------------------------------------------ */

/*
 * Recorre los poligonos del modelo (indices empaquetados en *AR4, de 5 en 5
 * palabras) y para cada uno comprueba sus aristas en pantalla; si alguno las
 * pasa todas sale por 0x2175 (modelo visible), y si no queda ninguno sale
 * por 0x216B. Reproduce el flujo original con sus ranuras de retardo.
 */
uint32_t hle_model_visible(void)
{
    uint32_t *r = C.r;
    uint32_t ar1 = 0, ar3 = 0, ar5 = 0, ar6 = 0, ar4 = r[AR4];
    uint32_t ir0 = r[IR0], ir1 = r[IR1], r4i, r5i, ri1;
    float R0 = getf(0), R1 = 0, R2 = getf(2), R3 = getf(3);
    float x1, y1, x3, y3, x5, y5, x6, y6;
    uint32_t rc = r[RC_];
    uint32_t work = 0;
    int r1_is_float = 0;

    SYNC_I(4); r4i = r[4];
    SYNC_I(5); r5i = r[5];
    SYNC_I(1); ri1 = r[1];

#define X(a) x##a
#define Y(a) y##a
    for (;;) {
        int exit_found = 0;
        ar1 = (ri1 & r4i) * 3;                         /* 213E-213F */
        ri1 >>= 8;                                     /* 2140 */
        ar3 = (ri1 & r4i) * 3;                         /* 2141-2142 */
        ri1 >>= 8;                                     /* 2143 */
        ar5 = (ri1 & r4i) * 3;                         /* 2144-2145 */
        ar6 = rt_shift(ri1, r5i, 0, 0) * 3;            /* 2146-2147 */
        /* coordenadas de los 4 vertices, convertidas una sola vez */
        x1 = MF(ar1 + ir0); y1 = MF(ar1 + ir1);
        x3 = MF(ar3 + ir0); y3 = MF(ar3 + ir1);
        x5 = MF(ar5 + ir0); y5 = MF(ar5 + ir1);
        x6 = MF(ar6 + ir0); y6 = MF(ar6 + ir1);
        /* 2149 BEQD 2152, ranuras 214A-214C */
        R0 = X(6) - X(5);
        R1 = Y(5) - Y(6);
        R2 = R0 * Y(5);
        r1_is_float = 1;
        work += 12;
        do {
            if (ar6 != ar5) {
                R3 = R1 * X(5);                      /* 214D */
                R2 = R2 + R3;
                R1 = fabsf(R1);
                {
                    int gt = R2 > R1;                  /* 2150-2151 BGTD 216A */
                    R0 = X(1) - X(6);              /* ranuras 2152-2154 */
                    R1 = Y(6) - Y(1);
                    R2 = R0 * Y(6);
                    work += 8;
                    if (gt)
                        break;
                }
            } else {
                R0 = X(1) - X(6);                  /* 2152-2154 */
                R1 = Y(6) - Y(1);
                R2 = R0 * Y(6);
            }
            R3 = R1 * X(6);                          /* 2155 */
            R2 = R2 + R3;
            R1 = fabsf(R1);
            {
                int gt = R2 > R1;                      /* 2159 BGTD 216A */
                R0 = X(3) - X(1);                  /* 215A-215C */
                R1 = Y(1) - Y(3);
                R2 = R0 * Y(1);
                work += 8;
                if (gt)
                    break;
            }
            R3 = R1 * X(1);                          /* 215D */
            R2 = R2 + R3;
            R1 = fabsf(R1);
            {
                int gt = R2 > R1;                      /* 2161 BGTD 216A */
                R0 = X(5) - X(3);                  /* 2162-2164 */
                R1 = Y(3) - Y(5);
                R2 = R0 * Y(3);
                work += 8;
                if (gt)
                    break;
            }
            R3 = R1 * X(3);                          /* 2165 */
            R2 = R2 + R3;
            R1 = fabsf(R1);
            work += 5;
            if (R2 <= R1)                              /* 2168-2169 BLE 2175 */
                exit_found = 1;
        } while (0);

        if (exit_found) {
            /* sale con el RPTB aun activo, como el original */
            r[RC_] = rc;
            setf(0, R0); setf(1, R1); setf(2, R2); setf(3, R3);
            r[AR1] = ar1; r[AR3] = ar3; r[AR4] = ar4; r[AR5] = ar5; r[AR6] = ar6;
            C.r[C3X_ST] = (C.r[C3X_ST] & ~0x1Fu) | (R2 - R1 == 0.0f ? F_Z : 0) |
                          (R2 - R1 < 0.0f ? F_N : 0);
            C.cyc += work;
            return 0x002175u;
        }
        ri1 = rdm(ar4);                                 /* 216A */
        ar4 += 5;
        r1_is_float = 0;
        work += 1;
        if ((int32_t)--rc < 0)
            break;
    }
#undef X
#undef Y
    (void)r1_is_float;
    r[RC_] = 0xFFFFFFFFu;
    C.r[C3X_ST] &= ~C3X_ST_RM;
    setf(0, R0); setf(2, R2); setf(3, R3);
    seti(1, ri1);
    r[AR1] = ar1; r[AR3] = ar3; r[AR4] = ar4; r[AR5] = ar5; r[AR6] = ar6;
    C.r[C3X_ST] = (C.r[C3X_ST] & ~0x1Fu) | rt_nz(ri1);
    r[RS_] = 0x00213Eu;
    r[RE_] = 0x00216Au;
    C.cyc += work;
    return 0x00216Bu;
}

/* ------------------------------------------------------------------------ */
/* ZSORTWT (OBJ.ASM): ordenacion por distancia mientras se espera al video  */
/* ------------------------------------------------------------------------ */

#define ZS_CLEARRDY  0x00C93Fu
#define ZS_ODIST     28u
#define ZS_EXIT      0x0071A9u      /* ZSWTX / ZSWTXX */
#define ZS_IDLE      0x0071A8u      /* "BR ZSORTWL" tras una pasada */

/*
 * El original hace pasadas de burbuja sobre la lista enlazada de objetos
 * hasta que la interrupcion de video borra CLEARRDY. Aqui se ordena del todo
 * de una vez (mismo criterio y mismos intercambios) y se vuelve al punto de
 * espera, donde el recompilador adelanta el reloj hasta la interrupcion.
 */
uint32_t hle_zsort(void)
{
    uint32_t *r = C.r;
    uint32_t head = RD(((r[DP] & 0xFF) << 16) | 0x40);
    uint32_t ar0 = head, ar1, ar2, nxt;
    int swapped, passes = 0;
    uint32_t work = 0;

    r[R6] = 0;
    for (;;) {
        swapped = 0;
        ar0 = head;
        ar1 = RD(ar0);
        if (!ar1)
            goto out_exit;
        ar2 = RD(ar1);
        if (!ar2)
            goto out_exit;
        if (RD(ZS_CLEARRDY) == 0)
            goto out_exit;
        for (;;) {
            int32_t d1 = (int32_t)RD(ar1 + ZS_ODIST), d2 = (int32_t)RD(ar2 + ZS_ODIST);
            work += 8;
            if ((int32_t)(d1 - d2) >= 0) {           /* ZWPRIOK */
                nxt = RD(ar2);
                ar0 = ar1;
                ar1 = ar2;
                ar2 = nxt;
                if (!nxt)
                    break;
            } else {                                 /* DOSWAP */
                swapped = 1;
                WR(ar0, ar2);
                nxt = RD(ar2);
                WR(ar1, nxt);
                WR(ar2, ar1);
                ar0 = ar2;
                ar2 = nxt;
                if (!nxt)
                    break;
            }
        }
        passes++;
        if (!swapped || passes > 64)
            break;
    }
    /* lista ordenada: punto de espera con R6 = 0 */
    r[R6] = 0;
    SYNC_I(0);
    r[R0] = RD(ZS_CLEARRDY); C.rk[0] = 2;
    SYNC_I(1);
    r[R1] = 0; C.rk[1] = 2;
    r[AR0] = ar0; r[AR1] = ar1; r[AR2] = ar2;
    C.cyc += work;
    return ZS_IDLE;

out_exit:
    SYNC_I(0);
    r[R0] = RD(ZS_CLEARRDY); C.rk[0] = 2;
    C.r[C3X_ST] = (C.r[C3X_ST] & ~0x1Fu) | rt_nz(r[R0]);
    C.cyc += work;
    return ZS_EXIT;
}
