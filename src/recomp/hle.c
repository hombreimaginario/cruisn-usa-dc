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
