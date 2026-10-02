/*
 * interp.c - Interprete de referencia del TMS320C31.
 *
 * Sirve para tres cosas: arrancar el juego en el host mientras se escribe el
 * recompilador, comparar instruccion a instruccion con el codigo recompilado
 * y ejecutar como respaldo cualquier codigo que el recompilador no haya
 * descubierto. Prioriza la claridad sobre la velocidad.
 *
 * Referencia: TMS320C3x User's Guide (SPRU031), capitulos 5, 6 y 13.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "c3x.h"

#define ST      cpu->r[C3X_ST]
#define REG(n)  cpu->r[(n)]
#define ISEXT(n) ((unsigned)(n) < 8u)

#define FLAG_MASK (C3X_ST_C | C3X_ST_V | C3X_ST_Z | C3X_ST_N | C3X_ST_UF)

/* ------------------------------------------------------------------------ */
/* Valores de 40 bits                                                         */
/* ------------------------------------------------------------------------ */

typedef struct {
    int32_t  e;     /* -128 = cero */
    uint32_t m;     /* bit 31 = signo, 30-0 = fraccion */
} ext_t;

static inline ext_t ext_from_mem(uint32_t v)
{
    ext_t x;
    x.e = (int8_t)(v >> 24);
    x.m = v << 8;
    return x;
}

static inline uint32_t ext_to_mem(ext_t x)
{
    return ((uint32_t)(x.e & 0xFF) << 24) | (x.m >> 8);
}

static inline ext_t ext_from_short(uint32_t v)
{
    ext_t x;
    x.e = ((int32_t)(v << 16)) >> 28;
    if (x.e == -8) {
        x.e = -128;
        x.m = 0;
        return x;
    }
    x.m = (((v >> 11) & 1u) << 31) | ((v & 0x7FFu) << 20);
    return x;
}

static double ext_to_double(ext_t x)
{
    double mant;
    if (x.e == -128)
        return 0.0;
    mant = (double)(x.m & 0x7FFFFFFFu) / 2147483648.0;
    mant = (x.m & 0x80000000u) ? mant - 2.0 : mant + 1.0;
    return ldexp(mant, x.e);
}

/* Convierte a 40 bits truncando la fraccion. *ovf / *unf indican saturacion. */
static ext_t ext_from_double(double d, int *ovf, int *unf)
{
    ext_t x;
    int k;
    double f, frac;

    *ovf = *unf = 0;
    if (d == 0.0 || d != d) {
        x.e = -128;
        x.m = 0;
        return x;
    }
    f = frexp(fabs(d), &k);          /* |d| = f * 2^k, 0.5 <= f < 1 */
    if (d > 0) {
        x.e = k - 1;
        frac = (2.0 * f - 1.0) * 2147483648.0;
        x.m = (uint32_t)frac & 0x7FFFFFFFu;
    } else if (f == 0.5) {
        /* -2^(k-1) = -2 * 2^(k-2) */
        x.e = k - 2;
        x.m = 0x80000000u;
    } else {
        x.e = k - 1;
        frac = (2.0 - 2.0 * f) * 2147483648.0;
        x.m = 0x80000000u | ((uint32_t)frac & 0x7FFFFFFFu);
    }
    if (x.e > 127) {
        *ovf = 1;
        x.e = 127;
        x.m = (d > 0) ? 0x7FFFFFFFu : 0x80000000u;
    } else if (x.e < -127) {
        *unf = 1;
        x.e = -128;
        x.m = 0;
    }
    return x;
}

/* ------------------------------------------------------------------------ */
/* Flags                                                                     */
/* ------------------------------------------------------------------------ */

static inline void set_flags(c3x_state *cpu, uint32_t clear, uint32_t set)
{
    if (set & C3X_ST_V)
        set |= C3X_ST_LV;
    if (set & C3X_ST_UF)
        set |= C3X_ST_LUF;
    ST = (ST & ~clear) | set;
}

static inline uint32_t nz32(uint32_t v)
{
    return (v == 0 ? C3X_ST_Z : 0) | ((v >> 31) ? C3X_ST_N : 0);
}

static inline uint32_t nzext(ext_t x)
{
    return (x.e == -128 ? C3X_ST_Z : 0) | ((x.m >> 31) ? C3X_ST_N : 0);
}

static int cond_true(c3x_state *cpu, int c)
{
    uint32_t s = ST;
    int C = !!(s & C3X_ST_C), V = !!(s & C3X_ST_V), Z = !!(s & C3X_ST_Z);
    int N = !!(s & C3X_ST_N), UF = !!(s & C3X_ST_UF);
    int LV = !!(s & C3X_ST_LV), LUF = !!(s & C3X_ST_LUF);

    switch (c) {
    case 0x00: return 1;
    case 0x01: return C;
    case 0x02: return C || Z;
    case 0x03: return !C && !Z;
    case 0x04: return !C;
    case 0x05: return Z;
    case 0x06: return !Z;
    case 0x07: return N;
    case 0x08: return N || Z;
    case 0x09: return !N && !Z;
    case 0x0A: return !N;
    case 0x0C: return !V;
    case 0x0D: return V;
    case 0x0E: return !UF;
    case 0x0F: return UF;
    case 0x10: return !LV;
    case 0x11: return LV;
    case 0x12: return !LUF;
    case 0x13: return LUF;
    case 0x14: return Z || UF;
    default:   return 0;
    }
}

/* ------------------------------------------------------------------------ */
/* Direccionamiento                                                          */
/* ------------------------------------------------------------------------ */

static uint32_t circ_add(uint32_t ar, int32_t step, uint32_t bk)
{
    uint32_t mask = 1;
    int32_t idx;

    if (bk == 0)
        return ar + step;
    while (mask <= bk)
        mask <<= 1;
    mask -= 1;
    idx = (int32_t)(ar & mask) + step;
    if (step >= 0) {
        if ((uint32_t)idx >= bk)
            idx -= bk;
    } else if (idx < 0) {
        idx += bk;
    }
    return (ar & ~mask) | ((uint32_t)idx & mask);
}

static uint32_t bitrev_add(uint32_t ar, uint32_t ir)
{
    /* suma con acarreo invertido sobre 24 bits: invertir, sumar, invertir */
    uint32_t ra = 0, ri = 0, r = 0, i;
    for (i = 0; i < 24; i++) {
        ra |= ((ar >> i) & 1u) << (23 - i);
        ri |= ((ir >> i) & 1u) << (23 - i);
    }
    ra = (ra + ri) & 0xFFFFFF;
    for (i = 0; i < 24; i++)
        r |= ((ra >> i) & 1u) << (23 - i);
    return (ar & 0xFF000000u) | r;
}

/* Calcula la direccion efectiva y aplica la modificacion del ARn. */
static uint32_t ea_indirect(c3x_state *cpu, int mod, int arn, int32_t disp)
{
    uint32_t *ar = &cpu->r[C3X_AR0 + arn];
    uint32_t a = *ar;
    uint32_t bk = cpu->r[C3X_BK];

    if (mod >= 0x08 && mod < 0x18)
        disp = (int32_t)cpu->r[mod < 0x10 ? C3X_IR0 : C3X_IR1];

    switch (mod & 0x1F) {
    case 0x00: case 0x08: case 0x10: return a + disp;
    case 0x01: case 0x09: case 0x11: return a - disp;
    case 0x02: case 0x0A: case 0x12: *ar = a + disp; return *ar;
    case 0x03: case 0x0B: case 0x13: *ar = a - disp; return *ar;
    case 0x04: case 0x0C: case 0x14: *ar = a + disp; return a;
    case 0x05: case 0x0D: case 0x15: *ar = a - disp; return a;
    case 0x06: case 0x0E: case 0x16: *ar = circ_add(a, disp, bk); return a;
    case 0x07: case 0x0F: case 0x17: *ar = circ_add(a, -disp, bk); return a;
    case 0x18: return a;
    case 0x19: *ar = bitrev_add(a, cpu->r[C3X_IR0]); return a;
    default:
        fprintf(stderr, "c3x: modo indirecto %02X no valido en %06X\n", mod, (unsigned)cpu->pc);
        return a;
    }
}

static inline uint32_t ea16(c3x_state *cpu, uint32_t f)
{
    return ea_indirect(cpu, (f >> 11) & 0x1F, (f >> 8) & 7, f & 0xFF) & 0xFFFFFF;
}

static inline uint32_t ea8(c3x_state *cpu, uint32_t f)
{
    return ea_indirect(cpu, (f >> 3) & 0x1F, f & 7, 1) & 0xFFFFFF;
}

static inline uint32_t ea_direct(c3x_state *cpu, uint32_t f)
{
    return ((cpu->r[C3X_DP] & 0xFF) << 16) | (f & 0xFFFF);
}

static inline uint32_t rd(uint32_t a)      { return c3x_mem_read(a & 0xFFFFFF); }
static inline void wr(uint32_t a, uint32_t v) { c3x_mem_write(a & 0xFFFFFF, v); }

static inline ext_t reg_ext(c3x_state *cpu, int n)
{
    ext_t x;
    if (ISEXT(n)) {
        x.e = cpu->exp[n];
        x.m = cpu->r[n];
    } else {
        x = ext_from_mem(cpu->r[n]);
    }
    return x;
}

static inline void set_reg_ext(c3x_state *cpu, int n, ext_t x)
{
    if (ISEXT(n)) {
        cpu->exp[n] = x.e;
        cpu->r[n] = x.m;
    } else {
        cpu->r[n] = ext_to_mem(x);
    }
}

/* Operando fuente de formato general (entero). */
static uint32_t src_int(c3x_state *cpu, int g, uint32_t f, int is_unsigned)
{
    switch (g) {
    case 0: return cpu->r[f & 0x1F];
    case 1: return rd(ea_direct(cpu, f));
    case 2: return rd(ea16(cpu, f));
    default: return is_unsigned ? (f & 0xFFFF) : (uint32_t)(int32_t)(int16_t)f;
    }
}

static ext_t src_flt(c3x_state *cpu, int g, uint32_t f)
{
    switch (g) {
    case 0: return reg_ext(cpu, f & 0x1F);
    case 1: return ext_from_mem(rd(ea_direct(cpu, f)));
    case 2: return ext_from_mem(rd(ea16(cpu, f)));
    default: return ext_from_short(f);
    }
}

/* ------------------------------------------------------------------------ */
/* Pila y control                                                            */
/* ------------------------------------------------------------------------ */

static inline void push(c3x_state *cpu, uint32_t v)
{
    cpu->r[C3X_SP]++;
    wr(cpu->r[C3X_SP], v);
}

static inline uint32_t pop(c3x_state *cpu)
{
    uint32_t v = rd(cpu->r[C3X_SP]);
    cpu->r[C3X_SP]--;
    return v;
}

void c3x_reset(c3x_state *cpu)
{
    memset(cpu, 0, sizeof(*cpu));
    {
        int i;
        for (i = 0; i < 8; i++)
            cpu->exp[i] = -128;
    }
    cpu->pc = rd(0) & 0xFFFFFF;
}

void c3x_set_irq(c3x_state *cpu, int bit)
{
    cpu->r[C3X_IF] |= 1u << bit;
}

static void check_interrupts(c3x_state *cpu)
{
    uint32_t pending;
    int bit;

    if (!(ST & C3X_ST_GIE) || cpu->delay_count)
        return;
    pending = cpu->r[C3X_IE] & cpu->r[C3X_IF] & 0x7FF;
    if (!pending)
        return;
    for (bit = 0; !(pending & (1u << bit)); bit++)
        ;
    cpu->r[C3X_IF] &= ~(1u << bit);
    push(cpu, cpu->pc);
    ST &= ~C3X_ST_GIE;
    cpu->pc = rd(1 + bit) & 0xFFFFFF;
    cpu->idle = 0;
}

/* ------------------------------------------------------------------------ */
/* Operaciones enteras                                                       */
/* ------------------------------------------------------------------------ */

static uint32_t op_add(c3x_state *cpu, uint32_t a, uint32_t b, uint32_t carry, int dst)
{
    uint64_t r = (uint64_t)a + b + carry;
    uint32_t res = (uint32_t)r;
    if (ISEXT(dst) || dst < 0) {
        uint32_t f = nz32(res);
        if (r >> 32) f |= C3X_ST_C;
        if (((a ^ res) & (b ^ res)) >> 31) f |= C3X_ST_V;
        set_flags(cpu, FLAG_MASK, f);
    }
    return res;
}

/* a - b - borrow */
static uint32_t op_sub(c3x_state *cpu, uint32_t a, uint32_t b, uint32_t borrow, int dst)
{
    uint32_t res = a - b - borrow;
    if (ISEXT(dst) || dst < 0) {
        uint32_t f = nz32(res);
        if ((uint64_t)a < (uint64_t)b + borrow) f |= C3X_ST_C;
        if (((a ^ b) & (a ^ res)) >> 31) f |= C3X_ST_V;
        set_flags(cpu, FLAG_MASK, f);
    }
    return res;
}

static uint32_t op_mpyi(c3x_state *cpu, uint32_t a, uint32_t b, int dst)
{
    int64_t r = (int64_t)(((int32_t)(a << 8)) >> 8) * (int64_t)(((int32_t)(b << 8)) >> 8);
    uint32_t res = (uint32_t)r;
    if (ISEXT(dst)) {
        uint32_t f = nz32(res);
        if (r != (int64_t)(int32_t)res) f |= C3X_ST_V;
        set_flags(cpu, FLAG_MASK & ~C3X_ST_C, f);
    }
    return res;
}

static uint32_t op_shift(c3x_state *cpu, uint32_t val, uint32_t cnt_src, int arith, int dst)
{
    int32_t cnt = ((int32_t)(cnt_src << 25)) >> 25;
    uint32_t res, c = 0;

    if (cnt > 0) {
        if (cnt >= 32) {
            res = 0;
            c = (cnt == 32) ? (val & 1) : 0;
        } else {
            res = val << cnt;
            c = (val >> (32 - cnt)) & 1;
        }
    } else if (cnt < 0) {
        int n = -cnt;
        if (n >= 32) {
            res = arith ? (uint32_t)((int32_t)val >> 31) : 0;
            c = arith ? (val >> 31) : (n == 32 ? (val >> 31) : 0);
        } else {
            res = arith ? (uint32_t)((int32_t)val >> n) : (val >> n);
            c = (val >> (n - 1)) & 1;
        }
    } else {
        res = val;
    }
    if (ISEXT(dst))
        set_flags(cpu, FLAG_MASK, nz32(res) | (c ? C3X_ST_C : 0));
    return res;
}

static inline void logic_flags(c3x_state *cpu, uint32_t res, int dst)
{
    if (ISEXT(dst) || dst < 0)
        set_flags(cpu, FLAG_MASK & ~C3X_ST_C, nz32(res));
}

/* ------------------------------------------------------------------------ */
/* Operaciones en coma flotante                                              */
/* ------------------------------------------------------------------------ */

static ext_t flt_result(c3x_state *cpu, double d, int setflags)
{
    int ovf, unf;
    ext_t x = ext_from_double(d, &ovf, &unf);
    if (setflags)
        set_flags(cpu, FLAG_MASK & ~C3X_ST_C,
                  nzext(x) | (ovf ? C3X_ST_V : 0) | (unf ? C3X_ST_UF : 0));
    return x;
}

static void flt_load_flags(c3x_state *cpu, ext_t x)
{
    set_flags(cpu, FLAG_MASK & ~C3X_ST_C, nzext(x));
}

/* ------------------------------------------------------------------------ */
/* Formato general de 2 operandos                                            */
/* ------------------------------------------------------------------------ */

static void exec_general(c3x_state *cpu, uint32_t w)
{
    int op = (w >> 23) & 0x3F;
    int g = (w >> 21) & 3;
    int d = (w >> 16) & 0x1F;
    uint32_t f = w & 0xFFFF;
    uint32_t a, res;
    ext_t x, y;

    switch (op) {
    case 0x00: /* ABSF */
        x = src_flt(cpu, g, f);
        set_reg_ext(cpu, d, flt_result(cpu, fabs(ext_to_double(x)), ISEXT(d)));
        break;
    case 0x01: /* ABSI */
        a = src_int(cpu, g, f, 0);
        res = ((int32_t)a < 0) ? (uint32_t)(-(int64_t)(int32_t)a) : a;
        if (ISEXT(d))
            set_flags(cpu, FLAG_MASK & ~C3X_ST_C, nz32(res) | (a == 0x80000000u ? C3X_ST_V : 0));
        REG(d) = res;
        break;
    case 0x02: /* ADDC */
        a = src_int(cpu, g, f, 0);
        REG(d) = op_add(cpu, REG(d), a, (ST & C3X_ST_C) ? 1 : 0, d);
        break;
    case 0x03: /* ADDF */
        x = src_flt(cpu, g, f);
        y = reg_ext(cpu, d);
        set_reg_ext(cpu, d, flt_result(cpu, ext_to_double(y) + ext_to_double(x), ISEXT(d)));
        break;
    case 0x04: /* ADDI */
        a = src_int(cpu, g, f, 0);
        REG(d) = op_add(cpu, REG(d), a, 0, d);
        break;
    case 0x05: /* AND */
        res = REG(d) & src_int(cpu, g, f, 1);
        logic_flags(cpu, res, d);
        REG(d) = res;
        break;
    case 0x06: /* ANDN */
        res = REG(d) & ~src_int(cpu, g, f, 1);
        logic_flags(cpu, res, d);
        REG(d) = res;
        break;
    case 0x07: /* ASH */
        REG(d) = op_shift(cpu, REG(d), src_int(cpu, g, f, 0), 1, d);
        break;
    case 0x08: /* CMPF */
        x = src_flt(cpu, g, f);
        y = reg_ext(cpu, d);
        flt_result(cpu, ext_to_double(y) - ext_to_double(x), 1);
        break;
    case 0x09: /* CMPI */
        a = src_int(cpu, g, f, 0);
        op_sub(cpu, REG(d), a, 0, -1);
        break;
    case 0x0A: { /* FIX */
        double v;
        uint32_t fl;
        x = src_flt(cpu, g, f);
        v = floor(ext_to_double(x));
        fl = 0;
        if (v > 2147483647.0) { res = 0x7FFFFFFFu; fl = C3X_ST_V; }
        else if (v < -2147483648.0) { res = 0x80000000u; fl = C3X_ST_V; }
        else res = (uint32_t)(int32_t)v;
        if (ISEXT(d))
            set_flags(cpu, FLAG_MASK & ~C3X_ST_C, nz32(res) | fl);
        REG(d) = res;
        break;
    }
    case 0x0B: /* FLOAT */
        a = src_int(cpu, g, f, 0);
        set_reg_ext(cpu, d, flt_result(cpu, (double)(int32_t)a, ISEXT(d)));
        break;
    case 0x0C: /* IDLE */
        ST |= C3X_ST_GIE;
        cpu->idle = 1;
        break;
    case 0x0D: /* LDE */
        x = src_flt(cpu, g, f);
        if (ISEXT(d)) {
            cpu->exp[d] = x.e;
            if (x.e == -128)
                cpu->r[d] = 0;
        }
        break;
    case 0x0E: /* LDF */
    case 0x0F: /* LDFI */
        x = src_flt(cpu, g, f);
        if (ISEXT(d))
            flt_load_flags(cpu, x);
        set_reg_ext(cpu, d, x);
        break;
    case 0x10: /* LDI */
    case 0x11: /* LDII */
        res = src_int(cpu, g, f, 0);
        if (ISEXT(d))
            set_flags(cpu, FLAG_MASK & ~C3X_ST_C, nz32(res));
        REG(d) = res;
        break;
    case 0x12: /* LDM */
        x = src_flt(cpu, g, f);
        if (ISEXT(d))
            cpu->r[d] = x.m;
        break;
    case 0x13: /* LSH */
        REG(d) = op_shift(cpu, REG(d), src_int(cpu, g, f, 0), 0, d);
        break;
    case 0x14: /* MPYF */
        x = src_flt(cpu, g, f);
        y = reg_ext(cpu, d);
        set_reg_ext(cpu, d, flt_result(cpu, ext_to_double(y) * ext_to_double(x), ISEXT(d)));
        break;
    case 0x15: /* MPYI */
        REG(d) = op_mpyi(cpu, REG(d), src_int(cpu, g, f, 0), d);
        break;
    case 0x16: /* NEGB */
        a = src_int(cpu, g, f, 0);
        REG(d) = op_sub(cpu, 0, a, (ST & C3X_ST_C) ? 1 : 0, d);
        break;
    case 0x17: /* NEGF */
        x = src_flt(cpu, g, f);
        set_reg_ext(cpu, d, flt_result(cpu, -ext_to_double(x), ISEXT(d)));
        break;
    case 0x18: /* NEGI */
        a = src_int(cpu, g, f, 0);
        REG(d) = op_sub(cpu, 0, a, 0, d);
        break;
    case 0x19: /* NOP (puede modificar un AR) */
        if (g == 2)
            (void)ea16(cpu, f);
        break;
    case 0x1A: /* NORM */
        x = src_flt(cpu, g, f);
        set_reg_ext(cpu, d, flt_result(cpu, ext_to_double(x), ISEXT(d)));
        break;
    case 0x1B: /* NOT */
        res = ~src_int(cpu, g, f, 1);
        logic_flags(cpu, res, d);
        REG(d) = res;
        break;
    case 0x1C: /* POP */
        res = pop(cpu);
        if (ISEXT(d))
            set_flags(cpu, FLAG_MASK & ~C3X_ST_C, nz32(res));
        REG(d) = res;
        break;
    case 0x1D: /* POPF */
        x = ext_from_mem(pop(cpu));
        if (ISEXT(d))
            flt_load_flags(cpu, x);
        set_reg_ext(cpu, d, x);
        break;
    case 0x1E: /* PUSH */
        push(cpu, REG(d));
        break;
    case 0x1F: /* PUSHF */
        push(cpu, ext_to_mem(reg_ext(cpu, d)));
        break;
    case 0x20: /* OR */
        res = REG(d) | src_int(cpu, g, f, 1);
        logic_flags(cpu, res, d);
        REG(d) = res;
        break;
    case 0x22: { /* RND */
        double v;
        x = src_flt(cpu, g, f);
        v = (double)(float)ext_to_double(x);
        set_reg_ext(cpu, d, flt_result(cpu, v, ISEXT(d)));
        break;
    }
    case 0x23: /* ROL */
        a = REG(d);
        res = (a << 1) | (a >> 31);
        if (ISEXT(d))
            set_flags(cpu, FLAG_MASK, nz32(res) | ((a >> 31) ? C3X_ST_C : 0));
        REG(d) = res;
        break;
    case 0x24: /* ROLC */
        a = REG(d);
        res = (a << 1) | ((ST & C3X_ST_C) ? 1 : 0);
        if (ISEXT(d))
            set_flags(cpu, FLAG_MASK, nz32(res) | ((a >> 31) ? C3X_ST_C : 0));
        REG(d) = res;
        break;
    case 0x25: /* ROR */
        a = REG(d);
        res = (a >> 1) | (a << 31);
        if (ISEXT(d))
            set_flags(cpu, FLAG_MASK, nz32(res) | ((a & 1) ? C3X_ST_C : 0));
        REG(d) = res;
        break;
    case 0x26: /* RORC */
        a = REG(d);
        res = (a >> 1) | ((ST & C3X_ST_C) ? 0x80000000u : 0);
        if (ISEXT(d))
            set_flags(cpu, FLAG_MASK, nz32(res) | ((a & 1) ? C3X_ST_C : 0));
        REG(d) = res;
        break;
    case 0x27: /* RPTS */
        cpu->r[C3X_RC] = src_int(cpu, g, f, 1);
        cpu->r[C3X_RS] = cpu->pc;
        cpu->r[C3X_RE] = cpu->pc;
        ST |= C3X_ST_RM;
        break;
    case 0x28: /* STF */
    case 0x29: /* STFI */
        a = (g == 1) ? ea_direct(cpu, f) : ea16(cpu, f);
        wr(a, ext_to_mem(reg_ext(cpu, d)));
        break;
    case 0x2A: /* STI */
    case 0x2B: /* STII */
        a = (g == 1) ? ea_direct(cpu, f) : ea16(cpu, f);
        wr(a, REG(d));
        break;
    case 0x2C: /* SIGI */
        break;
    case 0x2D: /* SUBB */
        a = src_int(cpu, g, f, 0);
        REG(d) = op_sub(cpu, REG(d), a, (ST & C3X_ST_C) ? 1 : 0, d);
        break;
    case 0x2E: { /* SUBC */
        uint32_t s = src_int(cpu, g, f, 0);
        uint32_t v = REG(d);
        if ((int64_t)(int32_t)v - (int64_t)(int32_t)s >= 0)
            REG(d) = ((v - s) << 1) | 1;
        else
            REG(d) = v << 1;
        break;
    }
    case 0x2F: /* SUBF */
        x = src_flt(cpu, g, f);
        y = reg_ext(cpu, d);
        set_reg_ext(cpu, d, flt_result(cpu, ext_to_double(y) - ext_to_double(x), ISEXT(d)));
        break;
    case 0x30: /* SUBI */
        a = src_int(cpu, g, f, 0);
        REG(d) = op_sub(cpu, REG(d), a, 0, d);
        break;
    case 0x31: /* SUBRB */
        a = src_int(cpu, g, f, 0);
        REG(d) = op_sub(cpu, a, REG(d), (ST & C3X_ST_C) ? 1 : 0, d);
        break;
    case 0x32: /* SUBRF */
        x = src_flt(cpu, g, f);
        y = reg_ext(cpu, d);
        set_reg_ext(cpu, d, flt_result(cpu, ext_to_double(x) - ext_to_double(y), ISEXT(d)));
        break;
    case 0x33: /* SUBRI */
        a = src_int(cpu, g, f, 0);
        REG(d) = op_sub(cpu, a, REG(d), 0, d);
        break;
    case 0x34: /* TSTB */
        res = REG(d) & src_int(cpu, g, f, 1);
        logic_flags(cpu, res, -1);
        break;
    case 0x35: /* XOR */
        res = REG(d) ^ src_int(cpu, g, f, 1);
        logic_flags(cpu, res, d);
        REG(d) = res;
        break;
    case 0x36: /* IACK */
        a = (g == 1) ? ea_direct(cpu, f) : ea16(cpu, f);
        (void)rd(a);
        break;
    default:
        fprintf(stderr, "c3x: opcode %02X no implementado en %06X (%08X)\n", op, (unsigned)(cpu->pc - 1), (unsigned)w);
        break;
    }
}

/* ------------------------------------------------------------------------ */
/* Formato de 3 operandos                                                    */
/* ------------------------------------------------------------------------ */

static void exec_three(c3x_state *cpu, uint32_t w)
{
    int op = (w >> 23) & 0x3F;
    int t = (w >> 21) & 3;
    int d = (w >> 16) & 0x1F;
    uint32_t f1 = (w >> 8) & 0xFF, f2 = w & 0xFF;
    uint32_t s1 = 0, s2 = 0, res;
    ext_t x1, x2;
    int isflt = (op == 0x01 || op == 0x06 || op == 0x09 || op == 0x0D);

    /* Fuente 1 (bits 15-8) y fuente 2 (bits 7-0). La sintaxis TI es
     * OP3 src2,src1,dst con dst = src1 OP src2. */
    if (isflt) {
        x1 = (t & 1) ? ext_from_mem(rd(ea8(cpu, f1))) : reg_ext(cpu, f1 & 0x1F);
        x2 = (t & 2) ? ext_from_mem(rd(ea8(cpu, f2))) : reg_ext(cpu, f2 & 0x1F);
    } else {
        s1 = (t & 1) ? rd(ea8(cpu, f1)) : REG(f1 & 0x1F);
        s2 = (t & 2) ? rd(ea8(cpu, f2)) : REG(f2 & 0x1F);
    }

    switch (op) {
    case 0x00: REG(d) = op_add(cpu, s1, s2, (ST & C3X_ST_C) ? 1 : 0, d); break; /* ADDC3 */
    case 0x01: set_reg_ext(cpu, d, flt_result(cpu, ext_to_double(x1) + ext_to_double(x2), ISEXT(d))); break;
    case 0x02: REG(d) = op_add(cpu, s1, s2, 0, d); break;                         /* ADDI3 */
    case 0x03: res = s1 & s2; logic_flags(cpu, res, d); REG(d) = res; break;     /* AND3 */
    case 0x04: res = s1 & ~s2; logic_flags(cpu, res, d); REG(d) = res; break;    /* ANDN3 */
    case 0x05: REG(d) = op_shift(cpu, s1, s2, 1, d); break;                      /* ASH3 */
    case 0x06: flt_result(cpu, ext_to_double(x1) - ext_to_double(x2), 1); break; /* CMPF3 */
    case 0x07: op_sub(cpu, s1, s2, 0, -1); break;                                /* CMPI3 */
    case 0x08: REG(d) = op_shift(cpu, s1, s2, 0, d); break;                      /* LSH3 */
    case 0x09: set_reg_ext(cpu, d, flt_result(cpu, ext_to_double(x1) * ext_to_double(x2), ISEXT(d))); break;
    case 0x0A: REG(d) = op_mpyi(cpu, s1, s2, d); break;                          /* MPYI3 */
    case 0x0B: res = s1 | s2; logic_flags(cpu, res, d); REG(d) = res; break;     /* OR3 */
    case 0x0C: REG(d) = op_sub(cpu, s1, s2, (ST & C3X_ST_C) ? 1 : 0, d); break; /* SUBB3 */
    case 0x0D: set_reg_ext(cpu, d, flt_result(cpu, ext_to_double(x1) - ext_to_double(x2), ISEXT(d))); break;
    case 0x0E: REG(d) = op_sub(cpu, s1, s2, 0, d); break;                        /* SUBI3 */
    case 0x0F: res = s1 & s2; logic_flags(cpu, res, -1); break;                  /* TSTB3 */
    case 0x10: res = s1 ^ s2; logic_flags(cpu, res, d); REG(d) = res; break;     /* XOR3 */
    default:
        fprintf(stderr, "c3x: opcode 3op %02X no implementado en %06X\n", op, (unsigned)(cpu->pc - 1));
        break;
    }
}

/* ------------------------------------------------------------------------ */
/* Instrucciones paralelas                                                   */
/* ------------------------------------------------------------------------ */

static void exec_par_mpy(c3x_state *cpu, uint32_t w)
{
    int op = (w >> 26) & 3;
    int p = (w >> 24) & 3;
    int d1 = (w >> 23) & 1;
    int d2 = 2 + ((w >> 22) & 1);
    int r1 = (w >> 19) & 7, r2 = (w >> 16) & 7;
    uint32_t a3 = ea8(cpu, (w >> 8) & 0xFF);
    uint32_t a4 = ea8(cpu, w & 0xFF);

    if (op < 2) {
        ext_t s1 = reg_ext(cpu, r1), s2 = reg_ext(cpu, r2);
        ext_t s3 = ext_from_mem(rd(a3)), s4 = ext_from_mem(rd(a4));
        ext_t ma, mb, aa, ab, rm, ra;
        switch (p) {
        case 0: ma = s3; mb = s4; aa = s1; ab = s2; break;
        case 1: ma = s3; mb = s1; aa = s4; ab = s2; break;
        case 2: ma = s1; mb = s2; aa = s3; ab = s4; break;
        default: ma = s3; mb = s1; aa = s2; ab = s4; break;
        }
        rm = flt_result(cpu, ext_to_double(ma) * ext_to_double(mb), 0);
        /* sintaxis TI: ADDF3/SUBF3 src2,src1,dst ; aqui aa es src1 */
        if (op == 0)
            ra = flt_result(cpu, ext_to_double(aa) + ext_to_double(ab), 1);
        else
            ra = flt_result(cpu, ext_to_double(aa) - ext_to_double(ab), 1);
        set_reg_ext(cpu, d1, rm);
        set_reg_ext(cpu, d2, ra);
    } else {
        uint32_t s1 = REG(r1), s2 = REG(r2), s3 = rd(a3), s4 = rd(a4);
        uint32_t ma, mb, aa, ab, rm, ra;
        switch (p) {
        case 0: ma = s3; mb = s4; aa = s1; ab = s2; break;
        case 1: ma = s3; mb = s1; aa = s4; ab = s2; break;
        case 2: ma = s1; mb = s2; aa = s3; ab = s4; break;
        default: ma = s3; mb = s1; aa = s2; ab = s4; break;
        }
        rm = op_mpyi(cpu, ma, mb, -2);  /* sin flags: los pone la suma */
        ra = (op == 2) ? op_add(cpu, aa, ab, 0, d2) : op_sub(cpu, aa, ab, 0, d2);
        REG(d1) = rm;
        REG(d2) = ra;
    }
}

static void exec_par_store(c3x_state *cpu, uint32_t w)
{
    int op = (w >> 25) & 0x1F;
    int r1 = (w >> 22) & 7;   /* dst1 / fuente del primer STx */
    int rs1 = (w >> 19) & 7;  /* src1 / dst2 de LD||LD */
    int rs2 = (w >> 16) & 7;  /* registro del segundo STx */
    uint32_t a3 = ea8(cpu, (w >> 8) & 0xFF);
    uint32_t a2 = ea8(cpu, w & 0xFF);
    uint32_t st_int = REG(rs2);
    ext_t st_flt = reg_ext(cpu, rs2);
    uint32_t v, res;
    ext_t x, y;

    switch (op) {
    case 0x00: /* STF || STF */
        x = reg_ext(cpu, r1);
        wr(a2, ext_to_mem(x));
        wr(a3, ext_to_mem(st_flt));
        return;
    case 0x01: /* STI || STI */
        v = REG(r1);
        wr(a2, v);
        wr(a3, st_int);
        return;
    case 0x02: /* LDF || LDF */
        x = ext_from_mem(rd(a2));
        y = ext_from_mem(rd(a3));
        set_reg_ext(cpu, r1, x);
        set_reg_ext(cpu, rs1, y);
        return;
    case 0x03: /* LDI || LDI */
        v = rd(a2);
        res = rd(a3);
        REG(r1) = v;
        REG(rs1) = res;
        return;
    default:
        break;
    }

    /* Operacion || STx : la operacion lee *a2 (y rs1); el store va a *a3. */
    v = rd(a2);
    x = ext_from_mem(v);
    switch (op) {
    case 0x04: set_reg_ext(cpu, r1, flt_result(cpu, fabs(ext_to_double(x)), 1)); break;
    case 0x05:
        res = ((int32_t)v < 0) ? (uint32_t)(-(int64_t)(int32_t)v) : v;
        set_flags(cpu, FLAG_MASK & ~C3X_ST_C, nz32(res) | (v == 0x80000000u ? C3X_ST_V : 0));
        REG(r1) = res;
        break;
    case 0x06: set_reg_ext(cpu, r1, flt_result(cpu, ext_to_double(x) + ext_to_double(reg_ext(cpu, rs1)), 1)); break;
    case 0x07: REG(r1) = op_add(cpu, v, REG(rs1), 0, r1); break;
    case 0x08: res = v & REG(rs1); logic_flags(cpu, res, r1); REG(r1) = res; break;
    case 0x09: REG(r1) = op_shift(cpu, v, REG(rs1), 1, r1); break;        /* ASH3 cnt,src */
    case 0x0A: {
        double dv = floor(ext_to_double(x));
        res = dv > 2147483647.0 ? 0x7FFFFFFFu : dv < -2147483648.0 ? 0x80000000u : (uint32_t)(int32_t)dv;
        set_flags(cpu, FLAG_MASK & ~C3X_ST_C, nz32(res));
        REG(r1) = res;
        break;
    }
    case 0x0B: set_reg_ext(cpu, r1, flt_result(cpu, (double)(int32_t)v, 1)); break;
    case 0x0C: flt_load_flags(cpu, x); set_reg_ext(cpu, r1, x); break;
    case 0x0D: set_flags(cpu, FLAG_MASK & ~C3X_ST_C, nz32(v)); REG(r1) = v; break;
    case 0x0E: REG(r1) = op_shift(cpu, v, REG(rs1), 0, r1); break;        /* LSH3 cnt,src */
    case 0x0F: set_reg_ext(cpu, r1, flt_result(cpu, ext_to_double(x) * ext_to_double(reg_ext(cpu, rs1)), 1)); break;
    case 0x10: REG(r1) = op_mpyi(cpu, v, REG(rs1), r1); break;
    case 0x11: set_reg_ext(cpu, r1, flt_result(cpu, -ext_to_double(x), 1)); break;
    case 0x12: REG(r1) = op_sub(cpu, 0, v, 0, r1); break;
    case 0x13: res = ~v; logic_flags(cpu, res, r1); REG(r1) = res; break;
    case 0x14: res = v | REG(rs1); logic_flags(cpu, res, r1); REG(r1) = res; break;
    case 0x15: /* SUBF3 rs1,*a2,r1 => r1 = *a2 - rs1 */
        set_reg_ext(cpu, r1, flt_result(cpu, ext_to_double(x) - ext_to_double(reg_ext(cpu, rs1)), 1));
        break;
    case 0x16: REG(r1) = op_sub(cpu, v, REG(rs1), 0, r1); break;
    case 0x17: res = v ^ REG(rs1); logic_flags(cpu, res, r1); REG(r1) = res; break;
    default:
        fprintf(stderr, "c3x: paralela %02X no implementada en %06X\n", op, (unsigned)(cpu->pc - 1));
        return;
    }
    /* El store usa el valor del registro anterior a la operacion. */
    if (op == 0x04 || op == 0x06 || op == 0x0B || op == 0x0C || op == 0x0F || op == 0x11 || op == 0x15)
        wr(a3, ext_to_mem(st_flt));
    else
        wr(a3, st_int);
}

/* ------------------------------------------------------------------------ */
/* Saltos y control de flujo                                                 */
/* ------------------------------------------------------------------------ */

static void branch(c3x_state *cpu, uint32_t target, int delayed)
{
    target &= 0xFFFFFF;
    if (delayed) {
        cpu->delay_target = target;
        cpu->delay_count = 3;
    } else {
        cpu->pc = target;
    }
}

static void exec_flow(c3x_state *cpu, uint32_t w, uint32_t addr)
{
    uint32_t hi = w >> 24;
    int cond;

    if (hi == 0x60 || hi == 0x61) {           /* BR / BRD */
        branch(cpu, w, hi == 0x61);
        return;
    }
    if (hi == 0x62) {                         /* CALL */
        push(cpu, cpu->pc);
        cpu->pc = w & 0xFFFFFF;
        return;
    }
    if (hi == 0x64) {                         /* RPTB */
        cpu->r[C3X_RS] = cpu->pc;
        cpu->r[C3X_RE] = w & 0xFFFFFF;
        ST |= C3X_ST_RM;
        return;
    }
    if (hi == 0x66) {                         /* SWI */
        fprintf(stderr, "c3x: SWI en %06X\n", (unsigned)addr);
        return;
    }
    cond = (w >> 16) & 0x1F;
    switch (w >> 26) {
    case 0x1A: { /* Bcond */
        int rel = (w >> 25) & 1, d = (w >> 21) & 1;
        uint32_t tgt = rel ? addr + (d ? 3 : 1) + (int16_t)w : cpu->r[w & 0x1F];
        if (cond_true(cpu, cond))
            branch(cpu, tgt, d);
        return;
    }
    case 0x1B: { /* DBcond */
        int rel = (w >> 25) & 1, d = (w >> 21) & 1;
        int arn = (w >> 22) & 7;
        uint32_t tgt = rel ? addr + (d ? 3 : 1) + (int16_t)w : cpu->r[w & 0x1F];
        uint32_t *ar = &cpu->r[C3X_AR0 + arn];
        int taken = cond_true(cpu, cond);
        *ar = (*ar & 0xFF000000u) | ((*ar - 1) & 0xFFFFFF);
        if (taken && !(*ar & 0x800000))
            branch(cpu, tgt, d);
        return;
    }
    case 0x1C: { /* CALLcond */
        int rel = (w >> 25) & 1;
        uint32_t tgt = rel ? addr + 1 + (int16_t)w : cpu->r[w & 0x1F];
        if (cond_true(cpu, cond)) {
            push(cpu, cpu->pc);
            cpu->pc = tgt & 0xFFFFFF;
        }
        return;
    }
    case 0x1D:
        if ((w >> 23) == 0xE8) { /* TRAPcond */
            if (cond_true(cpu, cond)) {
                push(cpu, cpu->pc);
                ST &= ~C3X_ST_GIE;
                cpu->pc = rd(0x20 + (w & 0x1F)) & 0xFFFFFF;
            }
            return;
        }
        break;
    case 0x1E:
        if ((w >> 23) == 0xF0) { /* RETIcond */
            if (cond_true(cpu, cond)) {
                cpu->pc = pop(cpu) & 0xFFFFFF;
                ST |= C3X_ST_GIE;
            }
            return;
        }
        if ((w >> 23) == 0xF1) { /* RETScond */
            if (cond_true(cpu, cond))
                cpu->pc = pop(cpu) & 0xFFFFFF;
            return;
        }
        break;
    }
    fprintf(stderr, "c3x: instruccion de flujo desconocida %08X en %06X\n", (unsigned)w, (unsigned)addr);
}

/* ------------------------------------------------------------------------ */
/* Bucle principal                                                           */
/* ------------------------------------------------------------------------ */

uint32_t c3x_pc_ring[64];
unsigned c3x_pc_ring_pos;
uint8_t *c3x_cov;          /* cobertura: 1 = ejecutada, 2 = destino de salto */
static uint32_t cov_expected;

void c3x_step(c3x_state *cpu)
{
    uint32_t addr = cpu->pc;
    c3x_pc_ring[c3x_pc_ring_pos++ & 63] = addr;
    if (c3x_cov) {
        c3x_cov[addr] |= (addr != cov_expected) ? 3 : 1;
        cov_expected = addr + 1;
    }
    uint32_t w = rd(addr);
    int pending_delay = cpu->delay_count;

    cpu->pc = (addr + 1) & 0xFFFFFF;
    cpu->cycles++;

    switch (w >> 29) {
    case 0: exec_general(cpu, w); break;
    case 1: exec_three(cpu, w); break;
    case 2: { /* LDFcond / LDIcond */
        int cond = (w >> 23) & 0x1F;
        int g = (w >> 21) & 3, d = (w >> 16) & 0x1F;
        if ((w >> 28) & 1) {
            uint32_t v = src_int(cpu, g, w & 0xFFFF, 0);
            if (cond_true(cpu, cond))
                REG(d) = v;
        } else {
            ext_t x = src_flt(cpu, g, w & 0xFFFF);
            if (cond_true(cpu, cond))
                set_reg_ext(cpu, d, x);
        }
        break;
    }
    case 3: exec_flow(cpu, w, addr); break;
    case 4: case 5: exec_par_mpy(cpu, w); break;
    default: exec_par_store(cpu, w); break;
    }

    /* Bucle de repeticion (RPTB/RPTS) */
    if ((ST & C3X_ST_RM) && addr == cpu->r[C3X_RE]) {
        cpu->r[C3X_RC]--;
        if ((int32_t)cpu->r[C3X_RC] >= 0)
            cpu->pc = cpu->r[C3X_RS];
        else
            ST &= ~C3X_ST_RM;
    }

    /* Salto retardado: se aplica tras las tres instrucciones siguientes. */
    if (pending_delay) {
        if (--cpu->delay_count == 0)
            cpu->pc = cpu->delay_target;
    }
}

uint32_t c3x_break_pc = 0xFFFFFFFFu;
void (*c3x_break_cb)(c3x_state *cpu);

void c3x_run(c3x_state *cpu, uint64_t until_cycle)
{
    while (cpu->cycles < until_cycle) {
        if (cpu->pc == c3x_break_pc && c3x_break_cb)
            c3x_break_cb(cpu);
        if (!(ST & C3X_ST_RM) || cpu->r[C3X_RS] != cpu->r[C3X_RE])
            check_interrupts(cpu);
        if (cpu->idle) {
            cpu->cycles = until_cycle;
            break;
        }
        c3x_step(cpu);
    }
}
