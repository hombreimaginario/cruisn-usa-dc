/*
 * c3x.h - Estado de CPU y utilidades del TMS320C31 para el codigo recompilado.
 *
 * El recompilador estatico (tools/c31recomp.py, en desarrollo) traduce la
 * imagen de programa de la ROM a C que opera sobre esta estructura. Todo lo
 * que dependa del hardware Midway V-Unit pasa por vunit/mem.h.
 */
#ifndef C3X_H
#define C3X_H

#include <stdint.h>

/* Indices de registro tal y como los codifica el C3x (campo de 5 bits). */
enum {
    C3X_R0, C3X_R1, C3X_R2, C3X_R3, C3X_R4, C3X_R5, C3X_R6, C3X_R7,
    C3X_AR0, C3X_AR1, C3X_AR2, C3X_AR3, C3X_AR4, C3X_AR5, C3X_AR6, C3X_AR7,
    C3X_DP, C3X_IR0, C3X_IR1, C3X_BK, C3X_SP, C3X_ST, C3X_IE, C3X_IF,
    C3X_IOF, C3X_RS, C3X_RE, C3X_RC,
    C3X_NUM_REGS
};

/* Bits del registro de estado ST. */
#define C3X_ST_C    0x0001u
#define C3X_ST_V    0x0002u
#define C3X_ST_Z    0x0004u
#define C3X_ST_N    0x0008u
#define C3X_ST_UF   0x0010u
#define C3X_ST_LV   0x0020u
#define C3X_ST_LUF  0x0040u
#define C3X_ST_OVM  0x0080u
#define C3X_ST_RM   0x0100u
#define C3X_ST_CF   0x0400u
#define C3X_ST_CE   0x0800u
#define C3X_ST_CC   0x1000u
#define C3X_ST_GIE  0x2000u

/*
 * Los registros R0-R7 son de precision extendida (40 bits): exponente de 8
 * bits con signo + mantisa de 32 bits (signo en el bit 31). Las operaciones
 * enteras solo tocan los 32 bits bajos (r[n]); las de coma flotante escriben
 * tambien exp[n]. El resto de registros son de 32 bits.
 */
typedef struct {
    uint32_t r[C3X_NUM_REGS];   /* R0-R7: mantisa/entero; resto: valor */
    int32_t  exp[8];            /* exponente de R0-R7 (-128 = cero) */
    uint32_t pc;
    uint32_t delay_target;      /* salto retardado pendiente */
    int      delay_count;
    int      idle;              /* ejecutando IDLE: espera interrupcion */
    uint64_t cycles;            /* instrucciones ejecutadas */
} c3x_state;

/* Bus de memoria: lo implementa el hardware (vunit/mem.c). */
uint32_t c3x_mem_read(uint32_t addr);
void     c3x_mem_write(uint32_t addr, uint32_t value);

/* Interprete de referencia (c3x/interp.c). */
void c3x_reset(c3x_state *cpu);
void c3x_run(c3x_state *cpu, uint64_t until_cycle);
void c3x_step(c3x_state *cpu);
double c3x_reg_double(const c3x_state *cpu, int n);   /* valor flotante de R0-R7 */

/* Depuracion: se llama a c3x_break_cb cada vez que el PC llega a c3x_break_pc. */
extern uint32_t c3x_break_pc;
extern void (*c3x_break_cb)(c3x_state *cpu);
extern uint32_t c3x_pc_ring[64];        /* ultimos PC ejecutados */
extern unsigned c3x_pc_ring_pos;
/* Cobertura (array de 16M entradas): bit 0 = ejecutada, bit 1 = se llego
 * a ella por un salto (entrada de bloque). La usa tools/c31recomp.py. */
extern uint8_t *c3x_cov;
void c3x_set_irq(c3x_state *cpu, int bit);   /* activa un bit de IF */

/* ---- Conversion entre el flotante de 32 bits del C3x e IEEE-754 ---- */

/*
 * Formato C3x: exponente con signo en bits 31-24, signo en bit 23 y
 * mantisa en complemento a dos en bits 22-0. Valor:
 *   positivo: (1 + f) * 2^e      negativo: (-2 + f) * 2^e
 * con f = frac / 2^23. El exponente -128 representa el cero.
 */
static inline float c3x_to_float(uint32_t v)
{
    union { uint32_t u; float f; } out;
    int32_t e = (int8_t)(v >> 24);
    uint32_t frac = v & 0x7FFFFFu;

    if (e == -128)
        return 0.0f;
    if (!(v & 0x800000u)) {
        if (e + 127 <= 0)
            return 0.0f;                         /* desnormal: a cero */
        out.u = ((uint32_t)(e + 127) << 23) | frac;
        return out.f;
    }
    if (frac == 0) {
        /* -2 * 2^e = -1 * 2^(e+1) */
        if (e + 128 >= 255)
            out.u = 0xFF7FFFFFu;                 /* satura al minimo finito */
        else if (e + 128 <= 0)
            return 0.0f;
        else
            out.u = 0x80000000u | ((uint32_t)(e + 128) << 23);
        return out.f;
    }
    if (e + 127 <= 0)
        return 0.0f;
    out.u = 0x80000000u | ((uint32_t)(e + 127) << 23) | (0x800000u - frac);
    return out.f;
}

static inline uint32_t float_to_c3x(float x)
{
    union { float f; uint32_t u; } in;
    uint32_t ie, mant;
    int32_t e;

    in.f = x;
    ie = (in.u >> 23) & 0xFF;
    mant = in.u & 0x7FFFFFu;

    if (ie == 0)
        return 0x80000000u;                      /* cero y desnormales */
    if (ie == 0xFF)                              /* inf/nan: satura */
        return (in.u & 0x80000000u) ? 0x7F800000u : 0x7F7FFFFFu;

    e = (int32_t)ie - 127;
    if (!(in.u & 0x80000000u))
        return ((uint32_t)(e & 0xFF) << 24) | mant;
    if (mant == 0) {
        /* -1 * 2^E = -2 * 2^(E-1) */
        e -= 1;
        if (e < -127)
            return 0x80000000u;
        return ((uint32_t)(e & 0xFF) << 24) | 0x800000u;
    }
    return ((uint32_t)(e & 0xFF) << 24) | 0x800000u | (0x800000u - mant);
}

/* Flotante corto de 16 bits usado en los inmediatos (LDF 1.5,R0, etc.). */
static inline float c3x_short_float(uint16_t v)
{
    int32_t e = ((int32_t)(v << 16)) >> 28;
    uint32_t frac = v & 0x7FF;
    float mant;

    if (e == -8)
        return 0.0f;
    mant = (v & 0x800) ? (-2.0f + frac / 2048.0f) : (1.0f + frac / 2048.0f);
    if (e >= 0)
        return mant * (float)(1u << e);
    return mant / (float)(1u << -e);
}

#endif /* C3X_H */
