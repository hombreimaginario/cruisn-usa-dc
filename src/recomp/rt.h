/*
 * rt.h - Runtime del codigo recompilado (tools/c31recomp.py).
 *
 * Registros R0-R7 de 40 bits con dos vistas sincronizadas de forma perezosa:
 *   - vista entera: exponente e[n] + mantisa r[n] (formato real del C3x),
 *   - vista flotante: f[n] en IEEE de 32 bits, rapida en la FPU del SH-4.
 * rk[n] indica que vistas son validas (bit 0 = flotante, bit 1 = entera).
 * Las operaciones enteras escriben la mantisa y conservan el exponente, como
 * el hardware (el juego guarda flotantes con PUSH/POP enteros y cuenta con
 * ello). El recompilador sabe en la mayoria de los casos que vista es valida
 * y solo emite SYNC_I/SYNC_F cuando no puede saberlo.
 *
 * Flujo de control: cada region (una por destino de CALL) es una funcion C
 * con un switch de entrada y gotos internos. Devuelve el siguiente PC; CALL
 * llama a la region destino de forma nativa y compara el PC devuelto con la
 * direccion de retorno. Si no coincide (cambio de proceso en MPROC, reset...)
 * propaga el PC hacia arriba hasta el despachador. La pila del C31 vive en la
 * memoria emulada, igual que en el original.
 */
#ifndef RECOMP_RT_H
#define RECOMP_RT_H

#include <stdint.h>

#include "../c3x/c3x.h"
#include "../vunit/mem.h"

typedef struct {
    uint32_t r[32];             /* 28-31: codigos de registro no validos */
    float    f[8];
    int32_t  e[8];              /* exponente de R0-R7 (-128 = cero) */
    uint8_t  rk[8];             /* vistas validas: 1 flotante, 2 entera */
    uint32_t cyc;               /* contador de ciclos (32 bits, rapido en SH-4) */
    uint32_t next_ev;           /* proximo evento: cuando cyc lo alcanza */
    uint32_t cyc_hi, cyc_last;  /* extension a 64 bits */
} rt_state;

extern rt_state C;
extern int rt_irq_disabled;
extern uint64_t rt_cycle_limit;
extern void (*rt_limit_cb)(void);

typedef uint32_t (*rt_region_fn)(uint32_t entry);

typedef struct {
    uint32_t start, end;        /* [start, end) */
    rt_region_fn fn;
} rt_region;

/* Generado */
extern const rt_region rt_regions[];
extern const unsigned rt_num_regions;

/* Runtime */
uint32_t rt_dispatch(uint32_t pc);            /* ejecuta una region desde pc */
uint32_t rt_call(uint32_t target, uint32_t ret);
uint32_t rt_service(uint32_t resume_pc);      /* eventos e interrupciones */
void     rt_reset(void);
void     rt_run(void);                        /* no vuelve */
uint32_t rt_unknown(uint32_t pc);

/* Lo implementa la plataforma: se llama cada RT_EVENT_PERIOD ciclos. */
void rt_platform_event(void);
/* Espera activa detectada: la plataforma adelanta el reloj al siguiente
 * evento que pueda despertar al juego (normalmente el fin de frame). */
void rt_platform_idle(void);
void rt_idle(void);
#define RT_EVENT_PERIOD 2000u

#ifndef LIKELY
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

/* ---- reloj ---- */

static inline uint64_t rt_cycles(void)
{
    return ((uint64_t)(C.cyc_hi + (C.cyc < C.cyc_last ? 1u : 0u)) << 32) | C.cyc;
}

static inline void rt_set_cycles(uint64_t v)
{
    C.cyc = (uint32_t)v;
    C.cyc_hi = (uint32_t)(v >> 32);
    C.cyc_last = C.cyc;
}

#define RT_FORCE_CHECK() (C.next_ev = C.cyc)

/* ---- memoria ---- */

#define RT_INLINE static inline __attribute__((always_inline))
#define RT_TAIL   __attribute__((musttail))

/* Accesos rapidos en linea solo para la FASTRAM; el resto pasa por una
 * funcion pequena que atiende primero la RAM interna del C31 (pila y
 * variables "oncram") y despues el bus completo. */
uint32_t rt_rd_slow(uint32_t a);
void     rt_wr_slow(uint32_t a, uint32_t v);

RT_INLINE uint32_t RD(uint32_t a)
{
    a &= 0xFFFFFFu;
    if (LIKELY(a < VU_FASTRAM_WORDS))
        return vu.fastram[a];
    if (LIKELY(a - VU_C31_RAM_BASE < VU_C31_RAM_WORDS))
        return vu.c31ram[a - VU_C31_RAM_BASE];
    return rt_rd_slow(a);
}

RT_INLINE void WR(uint32_t a, uint32_t v)
{
    a &= 0xFFFFFFu;
    if (LIKELY(a < VU_FASTRAM_WORDS))
        vu.fastram[a] = v;
    else if (LIKELY(a - VU_C31_RAM_BASE < VU_C31_RAM_WORDS))
        vu.c31ram[a - VU_C31_RAM_BASE] = v;
    else
        rt_wr_slow(a, v);
}

/* La pila vive en la RAM interna del C31. */
RT_INLINE void PUSH(uint32_t v)
{
    uint32_t sp = ++C.r[C3X_SP] & 0xFFFFFFu;
    if (LIKELY(sp - VU_C31_RAM_BASE < VU_C31_RAM_WORDS))
        vu.c31ram[sp - VU_C31_RAM_BASE] = v;
    else
        WR(sp, v);
}

RT_INLINE uint32_t POP(void)
{
    uint32_t sp = C.r[C3X_SP]-- & 0xFFFFFFu;
    if (LIKELY(sp - VU_C31_RAM_BASE < VU_C31_RAM_WORDS))
        return vu.c31ram[sp - VU_C31_RAM_BASE];
    return RD(sp);
}

/* ---- vistas de R0-R7 ---- */

static inline void rt_sync_i(int n)
{
    union { float f; uint32_t u; } v;
    uint32_t E, F;
    v.f = C.f[n];
    E = (v.u >> 23) & 0xFF;
    F = v.u & 0x7FFFFF;
    if (E == 0) {
        C.e[n] = -128; C.r[n] = 0;
    } else if (!(v.u >> 31)) {
        C.e[n] = (int32_t)E - 127; C.r[n] = F << 8;
    } else if (F == 0) {
        C.e[n] = (int32_t)E - 128; C.r[n] = 0x80000000u;
    } else {
        C.e[n] = (int32_t)E - 127; C.r[n] = 0x80000000u | ((0x800000u - F) << 8);
    }
    C.rk[n] |= 2;
}

static inline void rt_sync_f(int n)
{
    C.f[n] = C.e[n] == -128 ? 0.0f
           : c3x_to_float(((uint32_t)(C.e[n] & 0xFF) << 24) | (C.r[n] >> 8));
    C.rk[n] |= 1;
}

#define SYNC_I(n) do { if (!(C.rk[n] & 2)) rt_sync_i(n); } while (0)
#define SYNC_F(n) do { if (!(C.rk[n] & 1)) rt_sync_f(n); } while (0)

/* ---- flags ---- */

#define ST C.r[C3X_ST]
#define F_C  1u
#define F_V  2u
#define F_Z  4u
#define F_N  8u
#define F_UF 16u

RT_INLINE uint32_t rt_nz(uint32_t r)
{
    return (r == 0 ? F_Z : 0) | ((r >> 31) ? F_N : 0);
}

static inline void FL_ADD(uint32_t a, uint32_t b, uint32_t cin, uint32_t r)
{
    uint64_t wide = (uint64_t)a + b + cin;
    ST = (ST & ~0x1Fu) | rt_nz(r) | ((wide >> 32) ? F_C : 0) |
         ((((a ^ r) & (b ^ r)) >> 31) ? F_V : 0);
}

static inline void FL_SUB(uint32_t a, uint32_t b, uint32_t bin, uint32_t r)
{
    ST = (ST & ~0x1Fu) | rt_nz(r) | (((uint64_t)a < (uint64_t)b + bin) ? F_C : 0) |
         ((((a ^ b) & (a ^ r)) >> 31) ? F_V : 0);
}

static inline void FL_LOGIC(uint32_t r)
{
    ST = (ST & ~(F_V | F_Z | F_N | F_UF)) | rt_nz(r);
}

static inline void FL_MPYI(uint32_t r, int64_t wide)
{
    ST = (ST & ~(F_V | F_Z | F_N | F_UF)) | rt_nz(r) | (wide != (int64_t)(int32_t)r ? F_V : 0);
}

static inline void FL_SHIFT(uint32_t r, uint32_t c)
{
    ST = (ST & ~0x1Fu) | rt_nz(r) | (c ? F_C : 0);
}

static inline void FL_FLT(float x)
{
    ST = (ST & ~(F_V | F_Z | F_N | F_UF)) | (x == 0.0f ? F_Z : 0) | (x < 0.0f ? F_N : 0);
}

/* ---- operaciones ---- */

static inline uint32_t rt_mpyi(uint32_t a, uint32_t b, int64_t *wide)
{
    int64_t w = (int64_t)(((int32_t)(a << 8)) >> 8) * (int64_t)(((int32_t)(b << 8)) >> 8);
    if (wide)
        *wide = w;
    return (uint32_t)w;
}

/* Desplazamiento con contador de 7 bits con signo; *c = ultimo bit saliente. */
static inline uint32_t rt_shift(uint32_t val, uint32_t cnt_src, int arith, uint32_t *c)
{
    int32_t cnt = ((int32_t)(cnt_src << 25)) >> 25;
    uint32_t res, cc = 0;
    if (cnt > 0) {
        if (cnt >= 32) { res = 0; cc = cnt == 32 ? (val & 1) : 0; }
        else { res = val << cnt; cc = (val >> (32 - cnt)) & 1; }
    } else if (cnt < 0) {
        int n = -cnt;
        if (n >= 32) {
            res = arith ? (uint32_t)((int32_t)val >> 31) : 0;
            cc = arith ? (val >> 31) : (n == 32 ? (val >> 31) : 0);
        } else {
            res = arith ? (uint32_t)((int32_t)val >> n) : (val >> n);
            cc = (val >> (n - 1)) & 1;
        }
    } else {
        res = val;
    }
    if (c)
        *c = cc;
    return res;
}

static inline uint32_t rt_fix(float x, int *ovf)
{
    int32_t i;
    if (x >= 2147483648.0f) { if (ovf) *ovf = 1; return 0x7FFFFFFFu; }
    if (x < -2147483648.0f) { if (ovf) *ovf = 1; return 0x80000000u; }
    i = (int32_t)x;
    if ((float)i > x)
        i--;
    if (ovf) *ovf = 0;
    return (uint32_t)i;
}

static inline uint32_t rt_circ(uint32_t ar, int32_t step, uint32_t bk)
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
        if ((uint32_t)idx >= bk) idx -= bk;
    } else if (idx < 0) {
        idx += bk;
    }
    return (ar & ~mask) | ((uint32_t)idx & mask);
}

uint32_t rt_bitrev(uint32_t ar, uint32_t ir);
uint32_t hle_lzw_segment(void);
void     hle_vtx_dirq(void);
void     hle_vtx_model(void);
void     hle_vtx_world(void);
uint32_t hle_poly_emit(void);
uint32_t hle_model_visible(void);
uint32_t hle_zsort(void);
float    rt_lde(float dst, float src);
float    rt_ldm(float dst, float src);

#ifdef RT_TRACE_ON
extern void (*rt_trace_hook)(uint32_t pc);
#define RT_TRACE(pc) do { if (rt_trace_hook) rt_trace_hook(pc); } while (0)
#else
#define RT_TRACE(pc) do { } while (0)
#endif

/* Punto de servicio: eventos de la plataforma e interrupciones. */
#define RT_CHECK(pc) do { \
        if (UNLIKELY((int32_t)(C.cyc - C.next_ev) >= 0)) { \
            uint32_t _n = rt_service(pc); \
            if (_n != (pc)) return _n; \
        } \
    } while (0)

#endif /* RECOMP_RT_H */
