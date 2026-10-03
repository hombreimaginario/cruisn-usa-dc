#!/usr/bin/env python3
"""Recompilador estatico TMS320C31 -> C para Cruis'n USA.

Traduce la imagen de programa (FASTRAM, palabras 0x000000-0x01FFFF) a
funciones C que usan el runtime de src/recomp/rt.h. La semantica de cada
instruccion replica la del interprete de referencia (src/c3x/interp.c), con
el modelo de registros "partido" descrito en rt.h.

Uso:
    c31recomp.py <program.bin> <salida_dir> [--coverage coverage.bin]

La salida contiene codigo derivado de la ROM: va a generated/ y no se versiona.
"""
import argparse
import os
import re
import struct
import sys

# Temporales de flags: hay dos juegos (sufijo 0/1) que se alternan para que una
# instruccion nueva no pise los operandos de las flags pendientes.
TEMP_RE = re.compile(r"\b(fa|fb|fr|fc|fw|xa|xb|xr)\b")
MARK = "\x00"

CODE_END = 0x20000
ROM_BASE, ROM_END = 0xC00000, 0xC80000     # codigo que se ejecuta desde ROM (THECODE)
RANGES = ((0x40, CODE_END), (ROM_BASE, ROM_END))


# Esperas activas conocidas: al llegar a la direccion, si se cumple la
# condicion, el juego solo esta esperando la siguiente interrupcion de video.
IDLE_HINTS = {
    # ZSORTWT (OBJ.ASM): "BR ZSORTWL" tras una pasada sin intercambios; la
    # lista ya esta ordenada y solo se espera a que INT0 borre CLEARRDY.
    0x0071A8: ("SYNC_I(6);", "C.r[6] == 0"),
}


# Rutinas sustituidas por codigo nativo (src/recomp/hle.c): al llegar a la
# direccion se llama a la funcion, que devuelve el siguiente PC.
HLE_HOOKS = {
    0x00A334: "hle_lzw_segment",     # COMP.ASM DECOMPRESS_TOPLP3
}


def in_space(a):
    return 0x40 <= a < CODE_END or ROM_BASE <= a < ROM_END


def range_end(a):
    return CODE_END if a < CODE_END else ROM_END

# Registros
R_DP, R_IR0, R_IR1, R_BK, R_SP, R_ST, R_IE, R_IF, R_IOF, R_RS, R_RE, R_RC = range(16, 28)
SPECIAL_WRITE = (R_ST, R_IE, R_IF)

# Flags
FC, FV, FZ, FN, FUF = 1, 2, 4, 8, 16
FALL = 0x1F
ST_RM = 0x100
ST_GIE = 0x2000

COND_USE = {
    0x00: 0, 0x01: FC, 0x02: FC | FZ, 0x03: FC | FZ, 0x04: FC, 0x05: FZ, 0x06: FZ,
    0x07: FN, 0x08: FN | FZ, 0x09: FN | FZ, 0x0A: FN, 0x0C: FV, 0x0D: FV,
    0x0E: FUF, 0x0F: FUF, 0x10: 0, 0x11: 0, 0x12: 0, 0x13: 0, 0x14: FZ | FUF,
}

COND_ST = {
    0x00: "1",
    0x01: "(ST & 1u)",
    0x02: "(ST & 5u)",
    0x03: "!(ST & 5u)",
    0x04: "!(ST & 1u)",
    0x05: "(ST & 4u)",
    0x06: "!(ST & 4u)",
    0x07: "(ST & 8u)",
    0x08: "(ST & 12u)",
    0x09: "!(ST & 12u)",
    0x0A: "!(ST & 8u)",
    0x0C: "!(ST & 2u)",
    0x0D: "(ST & 2u)",
    0x0E: "!(ST & 16u)",
    0x0F: "(ST & 16u)",
    0x10: "!(ST & 32u)",
    0x11: "(ST & 32u)",
    0x12: "!(ST & 64u)",
    0x13: "(ST & 64u)",
    0x14: "(ST & 20u)",
}


def sext(v, bits):
    m = 1 << (bits - 1)
    return (v ^ m) - m


def short_float(v):
    e = sext(v >> 12, 4)
    if e == -8:
        return 0.0
    frac = v & 0x7FF
    mant = (-2.0 + frac / 2048.0) if (v >> 11) & 1 else (1.0 + frac / 2048.0)
    return mant * (2.0 ** e)


def flit(x):
    if x == 0.0:
        return "0.0f"
    return float(x).hex() + "f"


def reg(n):
    return "C.r[%d]" % n


def ext(n):
    return n < 8


# --------------------------------------------------------------------------
# Decodificacion para analisis de flujo
# --------------------------------------------------------------------------

class Ins:
    __slots__ = ("addr", "w", "kind", "cond", "target", "delayed", "valid", "dbreg")

    def __init__(self, addr, w):
        self.addr, self.w = addr, w
        self.kind = "op"       # op, br, bcond, breg, db, call, callcond, callreg, rets, reti, trap, rptb, rpts, swi, idle
        self.cond = 0
        self.target = None
        self.delayed = False
        self.valid = True
        self.dbreg = None


def decode(addr, w):
    i = Ins(addr, w)
    top = w >> 29
    if top == 0:
        op = (w >> 23) & 0x3F
        if op > 0x36 or op == 0x21:
            i.valid = False
        elif op == 0x27:
            i.kind = "rpts"
        elif op == 0x0C:
            i.kind = "idle"
        return i
    if top == 1:
        if ((w >> 23) & 0x3F) > 0x10:
            i.valid = False
        return i
    if top == 2:
        return i
    if top == 3:
        hi = w >> 24
        if hi in (0x60, 0x61):
            i.kind, i.target, i.delayed = "br", w & 0xFFFFFF, hi == 0x61
            return i
        if hi == 0x62:
            i.kind, i.target = "call", w & 0xFFFFFF
            return i
        if hi == 0x64:
            i.kind, i.target = "rptb", w & 0xFFFFFF
            return i
        if hi == 0x66:
            i.kind = "swi"
            return i
        i.cond = (w >> 16) & 0x1F
        sel = w >> 26
        if sel == 0x1A:
            rel, d = (w >> 25) & 1, (w >> 21) & 1
            i.delayed = bool(d)
            if rel:
                i.kind = "bcond"
                i.target = (addr + (3 if d else 1) + sext(w & 0xFFFF, 16)) & 0xFFFFFF
            else:
                i.kind = "breg"
            return i
        if sel == 0x1B:
            rel, d = (w >> 25) & 1, (w >> 21) & 1
            i.delayed = bool(d)
            i.dbreg = (w >> 22) & 7
            if rel:
                i.kind = "db"
                i.target = (addr + (3 if d else 1) + sext(w & 0xFFFF, 16)) & 0xFFFFFF
            else:
                i.kind = "dbreg"
            return i
        if sel == 0x1C:
            if (w >> 25) & 1:
                i.kind = "callcond"
                i.target = (addr + 1 + sext(w & 0xFFFF, 16)) & 0xFFFFFF
            else:
                i.kind = "callreg"
            return i
        if (w >> 23) == 0xE8:
            i.kind = "trap"
            return i
        if (w >> 23) == 0xF0:
            i.kind = "reti"
            return i
        if (w >> 23) == 0xF1:
            i.kind = "rets"
            return i
        i.valid = False
        return i
    if (w >> 30) == 2:
        return i
    if ((w >> 25) & 0x1F) > 0x17:
        i.valid = False
    return i


def is_terminator(i):
    """La ejecucion no continua en addr+1 (sin contar ranuras de retardo)."""
    if i.kind in ("br", "swi"):
        return True
    if i.kind in ("bcond", "breg") and i.cond == 0:
        return True
    if i.kind in ("rets", "reti") and i.cond == 0:
        return True
    if not i.valid:
        return True
    return False


# --------------------------------------------------------------------------
# Analisis: codigo, lideres y regiones
# --------------------------------------------------------------------------

class Program:
    def __init__(self, words, cov):
        self.words = words
        self.cov = cov
        self.ins = {}
        self.leaders = set()
        self.call_targets = set()
        self.rptb_ends = {}          # RE -> set(RS)
        self.backward_heads = set()

    def get(self, a):
        if a not in self.ins:
            w = self.words[a] if a < CODE_END else (self.words[a - ROM_BASE] if in_space(a) else 0xFFFFFFFF)
            self.ins[a] = decode(a, w)
        return self.ins[a]

    def discover(self, seeds):
        work = list(seeds)
        seen = set()
        while work:
            a = work.pop()
            if a in seen or not in_space(a):
                continue
            seen.add(a)
            i = self.get(a)
            if not i.valid:
                continue
            succ = []
            if i.delayed:
                succ += [a + 1, a + 2, a + 3]
                if i.kind != "br":
                    succ.append(a + 4)
                    self.leaders.add(a + 4)
            elif not is_terminator(i):
                succ.append(a + 1)
            if i.target is not None and i.kind != "rptb":
                succ.append(i.target)
                self.leaders.add(i.target)
                if i.target <= a:
                    self.backward_heads.add(i.target)
            if i.kind in ("call", "callcond"):
                self.call_targets.add(i.target)
            if i.kind in ("call", "callcond", "callreg", "trap", "rets", "reti"):
                self.leaders.add(a + 1)
            if i.kind == "rptb":
                self.leaders.add(a + 1)
                self.leaders.add(i.target + 1)
                self.rptb_ends.setdefault(i.target, set()).add(a + 1)
                succ.append(i.target + 1)
            if is_terminator(i) or i.kind in ("bcond", "db", "dbreg", "breg"):
                self.leaders.add(a + (4 if i.delayed else 1))
            work += succ
        return seen


def build(words, cov):
    prog = Program(words, cov)
    seeds = set()
    vectors = set()
    for v in range(0x40):
        t = words[v] & 0xFFFFFF
        if in_space(t):
            seeds.add(t)
            vectors.add(t)
    executed = set()
    if cov:
        for a in [x for lo, hi in RANGES for x in range(lo, hi)]:
            c = cov[a]
            if c & 1:
                executed.add(a)
                seeds.add(a)
            if c & 2:
                prog.leaders.add(a)
    code = prog.discover(seeds)
    # Direcciones de codigo citadas desde datos (procesos creados con CREATE,
    # tablas de saltos): se anaden como entradas si empiezan tras un fin de
    # bloque.
    extra = set()
    for a in range(0x40, CODE_END):
        if a in code:
            continue
        v = words[a]
        if in_space(v) and v not in code:
            prev = prog.get(v - 1)
            if (v - 1) not in code or is_terminator(prev):
                ok = True
                for k in range(4):
                    if not in_space(v + k) or not prog.get(v + k).valid:
                        ok = False
                        break
                if ok:
                    extra.add(v)
    code |= prog.discover(extra)
    prog.leaders |= extra
    prog.leaders |= vectors
    prog.leaders = {a for a in prog.leaders if a in code}
    prog.code = code
    prog.vectors = vectors
    starts = set((prog.call_targets | vectors) & code)
    for lo, hi in RANGES:
        inr = [a for a in code if lo <= a < hi]
        if inr:
            starts.add(min(inr))
    starts = sorted(starts)
    prog.region_starts = starts
    prog.region_set = set(starts)
    prog.leaders |= set(starts)
    prog.region_set = set(starts)
    return prog


# --------------------------------------------------------------------------
# Generacion de codigo
# --------------------------------------------------------------------------

class Flags:
    """Descripcion de las flags pendientes (aun no escritas en ST)."""

    def __init__(self, kind, defs, mat, conds):
        self.kind = kind
        self.defs = defs
        self.mat = mat          # sentencia C que las escribe en ST
        self.conds = conds      # cond -> expresion C directa
        self.sfx = "0"

    def bind(self, sfx):
        self.sfx = sfx
        self.mat = TEMP_RE.sub(r"\g<1>" + sfx, self.mat)
        self.conds = {k: TEMP_RE.sub(r"\g<1>" + sfx, v) for k, v in self.conds.items()}
        return self


def int_conds(r):
    s = "(int32_t)%s" % r
    return {0x05: "(%s == 0)" % r, 0x06: "(%s != 0)" % r, 0x07: "(%s < 0)" % s,
            0x0A: "(%s >= 0)" % s, 0x09: "(%s > 0)" % s, 0x08: "(%s <= 0)" % s}


def sub_conds(a, b, r, borrow):
    c = int_conds(r)
    if borrow is None:
        c.update({0x01: "(%s < %s)" % (a, b), 0x04: "(%s >= %s)" % (a, b),
                  0x03: "(%s > %s)" % (a, b), 0x02: "(%s <= %s)" % (a, b)})
    return c


def flt_conds(x):
    return {0x05: "(%s == 0.0f)" % x, 0x06: "(%s != 0.0f)" % x, 0x07: "(%s < 0.0f)" % x,
            0x0A: "(%s >= 0.0f)" % x, 0x09: "(%s > 0.0f)" % x, 0x08: "(%s <= 0.0f)" % x,
            0x14: "(%s == 0.0f)" % x}


def cmpf_conds(a, b):
    return {0x05: "(%s == %s)" % (a, b), 0x06: "(%s != %s)" % (a, b), 0x07: "(%s < %s)" % (a, b),
            0x0A: "(%s >= %s)" % (a, b), 0x09: "(%s > %s)" % (a, b), 0x08: "(%s <= %s)" % (a, b),
            0x14: "(%s == %s)" % (a, b)}


def fl_add(cin):
    return Flags("add", FALL, "FL_ADD(fa, fb, %s, fr);" % cin, int_conds("fr"))


def fl_sub(borrow):
    return Flags("sub", FALL, "FL_SUB(fa, fb, %s, fr);" % (borrow or "0"),
                 sub_conds("fa", "fb", "fr", borrow))


def fl_logic():
    return Flags("logic", FV | FZ | FN | FUF, "FL_LOGIC(fr);", int_conds("fr"))


def fl_mpyi():
    return Flags("mpyi", FV | FZ | FN | FUF, "FL_MPYI(fr, fw);", int_conds("fr"))


def fl_shift():
    return Flags("shift", FALL, "FL_SHIFT(fr, fc);", int_conds("fr"))


def fl_flt():
    return Flags("flt", FV | FZ | FN | FUF, "FL_FLT(xr);", flt_conds("xr"))


def fl_cmpf():
    return Flags("cmpf", FV | FZ | FN | FUF, "FL_FLT(xa - xb);", cmpf_conds("xa", "xb"))


class Gen:
    def __init__(self, prog):
        self.p = prog
        self.out = []
        self.region = None
        self.pending = None
        self.dp = None
        self.nea = 0
        self.sfx = "0"
        self.known = {}
        self.rkset = {}

    # ---------------- utilidades ----------------

    def emit(self, s):
        self.out.append("    " + s)

    def in_region(self, a):
        return self.region[0] <= a < self.region[1] and a in self.p.leaders

    def jump(self, target):
        if self.in_region(target):
            return "goto L_%06X;" % target
        return "return 0x%06XU;" % target

    def materialize(self):
        if self.pending is not None:
            self.out.append(MARK + "    " + self.pending.mat)
            self.pending = None

    def cond_expr(self, c, live_after_ignore=None):
        if c == 0:
            return "1"
        need = COND_USE.get(c, FALL)
        p = self.pending
        if p is not None and c in p.conds and (need & ~p.defs) == 0:
            return p.conds[c]
        self.materialize()
        return COND_ST.get(c, "0")

    def set_flags(self, fl, live_after):
        """Registra flags nuevas; materializa las anteriores si hace falta."""
        fl.bind(self.sfx)
        if self.pending is not None and (live_after & self.pending.defs & ~fl.defs):
            self.materialize()
        self.pending = fl
        if not (live_after & fl.defs):
            # nadie las va a leer: se descartan
            self.pending = None

    def drop_flags(self):
        self.pending = None

    # ---------------- operandos ----------------

    def new_ea(self):
        n = "ea%d" % self.nea
        self.nea += 1
        return n

    def ind(self, mod, ar, disp):
        """Calcula la direccion de un modo indirecto; devuelve el nombre del temporal."""
        e = self.new_ea()
        a = reg(8 + ar)
        if 0x08 <= mod < 0x18:
            d = reg(R_IR0 if mod < 0x10 else R_IR1)
        else:
            d = str(disp)
        m = mod & 7 if mod < 0x18 else mod
        if mod >= 0x18:
            if mod == 0x18:
                self.emit("%s = %s;" % (e, a))
            elif mod == 0x19:
                self.emit("%s = %s; %s = rt_bitrev(%s, %s);" % (e, a, a, e, reg(R_IR0)))
            else:
                self.emit("%s = %s; /* modo %02X no valido */" % (e, a, mod))
            return e
        if m == 0:
            self.emit("%s = %s + %s;" % (e, a, d))
        elif m == 1:
            self.emit("%s = %s - %s;" % (e, a, d))
        elif m == 2:
            self.emit("%s += %s; %s = %s;" % (a, d, e, a))
        elif m == 3:
            self.emit("%s -= %s; %s = %s;" % (a, d, e, a))
        elif m == 4:
            self.emit("%s = %s; %s += %s;" % (e, a, a, d))
        elif m == 5:
            self.emit("%s = %s; %s -= %s;" % (e, a, a, d))
        elif m == 6:
            self.emit("%s = %s; %s = rt_circ(%s, (int32_t)%s, %s);" % (e, a, a, e, d, reg(R_BK)))
        else:
            self.emit("%s = %s; %s = rt_circ(%s, -(int32_t)%s, %s);" % (e, a, a, e, d, reg(R_BK)))
        return e

    def ind8(self, f):
        return self.ind((f >> 3) & 0x1F, f & 7, 1)

    def ind16(self, f):
        return self.ind((f >> 11) & 0x1F, (f >> 8) & 7, f & 0xFF)

    def direct(self, f):
        if self.dp is not None:
            return ("const", (self.dp << 16) | (f & 0xFFFF))
        return ("expr", "(((C.r[16] & 0xFFu) << 16) | 0x%04XU)" % (f & 0xFFFF))

    def mem_read(self, addr):
        kind, v = addr
        if kind == "const":
            if v < CODE_END:
                return "vu.fastram[0x%05X]" % v
            return "c3x_mem_read(0x%06XU)" % v
        return "RD(%s)" % v

    def mem_write(self, addr, val):
        kind, v = addr
        if kind == "const":
            if v < CODE_END:
                self.emit("vu.fastram[0x%05X] = %s;" % (v, val))
            else:
                self.emit("c3x_mem_write(0x%06XU, %s);" % (v, val))
        else:
            self.emit("WR(%s, %s);" % (v, val))

    def gaddr(self, g, f):
        if g == 1:
            return self.direct(f)
        return ("expr", self.ind16(f))

    def src_int(self, g, f, unsigned=False):
        if g == 0:
            n = f & 0x1F
            return self.ri(n)
        if g == 3:
            return "0x%XU" % (f & 0xFFFF) if unsigned else "0x%XU" % (sext(f & 0xFFFF, 16) & 0xFFFFFFFF)
        return self.mem_read(self.gaddr(g, f))

    def src_flt(self, g, f):
        if g == 0:
            n = f & 0x1F
            return self.rf(n)
        if g == 3:
            return flit(short_float(f & 0xFFFF))
        return "c3x_to_float(%s)" % self.mem_read(self.gaddr(g, f))

    # Vistas de R0-R7 (ver rt.h): known[n] = bits de vista valida conocidos
    # en compilacion (1 = flotante, 2 = entera+exponente).
    def ri(self, n):
        if not ext(n):
            if n == R_ST:
                self.materialize()
            return reg(n)
        if not (self.known.get(n, 0) & 2):
            self.emit("SYNC_I(%d);" % n)
            self.known[n] = self.known.get(n, 0) | 2
            self.rkset.pop(n, None)
        return "C.r[%d]" % n

    def rf(self, n):
        if not ext(n):
            return "c3x_to_float(%s)" % reg(n)
        if not (self.known.get(n, 0) & 1):
            self.emit("SYNC_F(%d);" % n)
            self.known[n] = self.known.get(n, 0) | 1
            self.rkset.pop(n, None)
        return "C.f[%d]" % n

    def wi(self, n, expr):
        if not ext(n):
            self.set_r(n, expr)
            return
        k = self.known.get(n, 0)
        if not (k & 2):
            self.emit("SYNC_I(%d);" % n)
        if self.rkset.get(n) == 2:
            self.emit("C.r[%d] = %s;" % (n, expr))
        else:
            self.emit("C.r[%d] = %s; C.rk[%d] = 2;" % (n, expr, n))
        self.known[n] = 2
        self.rkset[n] = 2

    def wf(self, n, expr):
        if not ext(n):
            self.emit("%s = float_to_c3x(%s);" % (reg(n), expr))
            self.after_write(n)
            return
        if self.rkset.get(n) == 1:
            self.emit("C.f[%d] = %s;" % (n, expr))
        else:
            self.emit("C.f[%d] = %s; C.rk[%d] = 1;" % (n, expr, n))
        self.known[n] = 1
        self.rkset[n] = 1

    def freg(self, n):
        return self.rf(n)

    def set_f(self, n, expr):
        if ext(n):
            self.wf(n, expr)
        else:
            self.emit("%s = float_to_c3x(%s);" % (reg(n), expr))
            self.after_write(n)

    def set_r(self, n, expr):
        if ext(n):
            self.wi(n, expr)
            return
        if n == R_ST:
            self.drop_flags()
        self.emit("%s = %s;" % (reg(n), expr))
        self.after_write(n)

    def after_write(self, n):
        if n in SPECIAL_WRITE:
            self.emit("C.next_event = 0;")
        if n == R_DP:
            self.dp = None

    # ---------------- instrucciones ----------------

    def gen_general(self, i, live):
        w = i.w
        op = (w >> 23) & 0x3F
        g = (w >> 21) & 3
        d = (w >> 16) & 0x1F
        f = w & 0xFFFF
        fl = ext(d)
        if d == R_ST and op not in (0x10, 0x11, 0x1C, 0x0E, 0x0F):
            self.materialize()      # operacion que lee ST como destino

        def iflags(maker):
            if fl:
                self.set_flags(maker, live)

        if op == 0x00:    # ABSF
            self.emit("xr = fabsf(%s);" % self.src_flt(g, f))
            self.set_f(d, "xr"); iflags(fl_flt())
        elif op == 0x01:  # ABSI
            self.emit("t0 = %s; fr = ((int32_t)t0 < 0) ? (uint32_t)(-(int64_t)(int32_t)t0) : t0;" % self.src_int(g, f))
            self.set_r(d, "fr"); iflags(fl_logic())
        elif op == 0x02:  # ADDC
            s = self.src_int(g, f)
            self.materialize()
            self.emit("fb = %s; fa = %s; fc = ST & 1u; fr = fa + fb + fc;" % (s, self.ri(d)))
            self.set_r(d, "fr"); iflags(fl_add("fc"))
        elif op == 0x03:  # ADDF
            self.emit("xr = %s + %s;" % (self.freg(d), self.src_flt(g, f)))
            self.set_f(d, "xr"); iflags(fl_flt())
        elif op == 0x04:  # ADDI
            self.emit("fb = %s; fa = %s; fr = fa + fb;" % (self.src_int(g, f), self.ri(d)))
            self.set_r(d, "fr"); iflags(fl_add("0"))
        elif op in (0x05, 0x06, 0x20, 0x35, 0x34):  # AND ANDN OR XOR TSTB
            s = self.src_int(g, f, unsigned=True)
            expr = {0x05: "%s & %s", 0x06: "%s & ~%s", 0x20: "%s | %s", 0x35: "%s ^ %s",
                    0x34: "%s & %s"}[op] % (self.ri(d), s)
            self.emit("fr = %s;" % expr)
            if op == 0x34:
                self.set_flags(fl_logic(), live)
            else:
                self.set_r(d, "fr"); iflags(fl_logic())
        elif op in (0x07, 0x13):  # ASH LSH
            self.emit("fr = rt_shift(%s, %s, %d, &fc);" % (self.ri(d), self.src_int(g, f), 1 if op == 0x07 else 0))
            self.set_r(d, "fr"); iflags(fl_shift())
        elif op == 0x08:  # CMPF
            self.emit("xb = %s; xa = %s;" % (self.src_flt(g, f), self.freg(d)))
            self.set_flags(fl_cmpf(), live)
        elif op == 0x09:  # CMPI
            self.emit("fb = %s; fa = %s; fr = fa - fb;" % (self.src_int(g, f), self.ri(d)))
            self.set_flags(fl_sub(None), live)
        elif op == 0x0A:  # FIX
            self.emit("fr = rt_fix(%s, 0);" % self.src_flt(g, f))
            self.set_r(d, "fr"); iflags(fl_logic())
        elif op == 0x0B:  # FLOAT
            self.emit("xr = (float)(int32_t)%s;" % self.src_int(g, f))
            self.set_f(d, "xr"); iflags(fl_flt())
        elif op == 0x0C:  # IDLE
            self.emit("ST |= 0x%XU; C.next_event = 0;" % ST_GIE)
        elif op == 0x0D:  # LDE
            self.emit("xr = %s;" % self.src_flt(g, f))
            self.set_f(d, "rt_lde(%s, xr)" % self.freg(d))
        elif op in (0x0E, 0x0F):  # LDF LDFI
            self.emit("xr = %s;" % self.src_flt(g, f))
            self.set_f(d, "xr"); iflags(fl_flt())
        elif op in (0x10, 0x11):  # LDI LDII
            s = self.src_int(g, f)
            if d == R_DP and g == 3:
                self.dp = f & 0xFF
                self.emit("%s = 0x%XU;" % (reg(d), sext(f, 16) & 0xFFFFFFFF))
                return
            self.emit("fr = %s;" % s)
            self.set_r(d, "fr"); iflags(fl_logic())
        elif op == 0x12:  # LDM
            self.emit("xr = %s;" % self.src_flt(g, f))
            self.set_f(d, "rt_ldm(%s, xr)" % self.freg(d))
        elif op == 0x14:  # MPYF
            self.emit("xr = %s * %s;" % (self.freg(d), self.src_flt(g, f)))
            self.set_f(d, "xr"); iflags(fl_flt())
        elif op == 0x15:  # MPYI
            self.emit("fr = rt_mpyi(%s, %s, &fw);" % (self.ri(d), self.src_int(g, f)))
            self.set_r(d, "fr"); iflags(fl_mpyi())
        elif op == 0x16:  # NEGB
            s = self.src_int(g, f)
            self.materialize()
            self.emit("fa = 0; fb = %s; fc = ST & 1u; fr = fa - fb - fc;" % s)
            self.set_r(d, "fr"); iflags(fl_sub("fc"))
        elif op == 0x17:  # NEGF
            self.emit("xr = -(%s);" % self.src_flt(g, f))
            self.set_f(d, "xr"); iflags(fl_flt())
        elif op == 0x18:  # NEGI
            self.emit("fa = 0; fb = %s; fr = fa - fb;" % self.src_int(g, f))
            self.set_r(d, "fr"); iflags(fl_sub(None))
        elif op == 0x19:  # NOP
            if g == 2:
                self.ind16(f)
        elif op in (0x1A, 0x22):  # NORM RND
            self.emit("xr = %s;" % self.src_flt(g, f))
            self.set_f(d, "xr"); iflags(fl_flt())
        elif op == 0x1B:  # NOT
            self.emit("fr = ~%s;" % self.src_int(g, f, unsigned=True))
            self.set_r(d, "fr"); iflags(fl_logic())
        elif op == 0x1C:  # POP
            self.emit("fr = POP();")
            self.set_r(d, "fr"); iflags(fl_logic())
        elif op == 0x1D:  # POPF
            self.emit("xr = c3x_to_float(POP());")
            self.set_f(d, "xr"); iflags(fl_flt())
        elif op == 0x1E:  # PUSH
            if d == R_ST:
                self.materialize()
            self.emit("PUSH(%s);" % self.ri(d))
        elif op == 0x1F:  # PUSHF
            self.emit("PUSH(float_to_c3x(%s));" % self.freg(d))
        elif op in (0x23, 0x24, 0x25, 0x26):  # ROL ROLC ROR RORC
            if op in (0x24, 0x26):
                self.materialize()
            a = self.ri(d)
            expr = {0x23: "(t0 << 1) | (t0 >> 31)", 0x24: "(t0 << 1) | (ST & 1u)",
                    0x25: "(t0 >> 1) | (t0 << 31)", 0x26: "(t0 >> 1) | ((ST & 1u) << 31)"}[op]
            cbit = "t0 >> 31" if op in (0x23, 0x24) else "t0 & 1u"
            self.emit("t0 = %s; fr = %s; fc = %s;" % (a, expr, cbit))
            self.set_r(d, "fr"); iflags(fl_shift())
        elif op in (0x28, 0x29):  # STF STFI
            self.mem_write(self.gaddr(g, f), "float_to_c3x(%s)" % self.freg(d))
        elif op in (0x2A, 0x2B):  # STI STII
            if d == R_ST:
                self.materialize()
            ad = self.gaddr(g, f)
            self.mem_write(ad, self.ri(d))
        elif op == 0x2C:  # SIGI
            pass
        elif op == 0x2D:  # SUBB
            s = self.src_int(g, f)
            self.materialize()
            self.emit("fb = %s; fa = %s; fc = ST & 1u; fr = fa - fb - fc;" % (s, self.ri(d)))
            self.set_r(d, "fr"); iflags(fl_sub("fc"))
        elif op == 0x2E:  # SUBC
            self.emit("t1 = %s; t0 = %s;" % (self.src_int(g, f), self.ri(d)))
            self.set_r(d, "((int64_t)(int32_t)t0 - (int64_t)(int32_t)t1 >= 0) ? (((t0 - t1) << 1) | 1u) : (t0 << 1)")
        elif op == 0x2F:  # SUBF
            self.emit("xr = %s - %s;" % (self.freg(d), self.src_flt(g, f)))
            self.set_f(d, "xr"); iflags(fl_flt())
        elif op == 0x30:  # SUBI
            if g == 0 and d == R_DP and (f & 0x1F) == R_DP:   # SETDP
                self.emit("%s = 0;" % reg(R_DP))
                self.dp = 0
                return
            self.emit("fb = %s; fa = %s; fr = fa - fb;" % (self.src_int(g, f), self.ri(d)))
            self.set_r(d, "fr"); iflags(fl_sub(None))
        elif op == 0x31:  # SUBRB
            s = self.src_int(g, f)
            self.materialize()
            self.emit("fa = %s; fb = %s; fc = ST & 1u; fr = fa - fb - fc;" % (s, self.ri(d)))
            self.set_r(d, "fr"); iflags(fl_sub("fc"))
        elif op == 0x32:  # SUBRF
            self.emit("xr = %s - %s;" % (self.src_flt(g, f), self.freg(d)))
            self.set_f(d, "xr"); iflags(fl_flt())
        elif op == 0x33:  # SUBRI
            self.emit("fa = %s; fb = %s; fr = fa - fb;" % (self.src_int(g, f), self.ri(d)))
            self.set_r(d, "fr"); iflags(fl_sub(None))
        elif op == 0x36:  # IACK
            ad = self.gaddr(g, f)
            self.emit("(void)%s;" % self.mem_read(ad))
        elif op == 0x27:  # RPTS (se trata en gen_block)
            raise AssertionError("RPTS")
        else:
            self.emit("return rt_unknown(0x%06XU);" % i.addr)

    def gen_three(self, i, live):
        w = i.w
        op = (w >> 23) & 0x3F
        t = (w >> 21) & 3
        d = (w >> 16) & 0x1F
        f1, f2 = (w >> 8) & 0xFF, w & 0xFF
        isflt = op in (0x01, 0x06, 0x09, 0x0D)
        fl = ext(d)

        if isflt:
            s1 = "c3x_to_float(RD(%s))" % self.ind8(f1) if t & 1 else self.freg(f1 & 0x1F)
            s2 = "c3x_to_float(RD(%s))" % self.ind8(f2) if t & 2 else self.freg(f2 & 0x1F)
            self.emit("xa = %s; xb = %s;" % (s1, s2))
            if op == 0x06:
                self.set_flags(fl_cmpf(), live)
                return
            expr = {0x01: "xa + xb", 0x09: "xa * xb", 0x0D: "xa - xb"}[op]
            self.emit("xr = %s;" % expr)
            self.set_f(d, "xr")
            if fl:
                self.set_flags(fl_flt(), live)
            return

        if (not (t & 1)) and (f1 & 0x1F) == R_ST or (not (t & 2)) and (f2 & 0x1F) == R_ST:
            self.materialize()
        s1 = "RD(%s)" % self.ind8(f1) if t & 1 else self.ri(f1 & 0x1F)
        s2 = "RD(%s)" % self.ind8(f2) if t & 2 else self.ri(f2 & 0x1F)
        if op in (0x00, 0x0C):
            self.materialize()
        self.emit("fa = %s; fb = %s;" % (s1, s2))
        if op == 0x00:
            self.emit("fc = ST & 1u; fr = fa + fb + fc;")
            mk = fl_add("fc")
        elif op == 0x02:
            self.emit("fr = fa + fb;")
            mk = fl_add("0")
        elif op == 0x03:
            self.emit("fr = fa & fb;")
            mk = fl_logic()
        elif op == 0x04:
            self.emit("fr = fa & ~fb;")
            mk = fl_logic()
        elif op == 0x05:
            self.emit("fr = rt_shift(fa, fb, 1, &fc);")
            mk = fl_shift()
        elif op == 0x07:
            self.emit("fr = fa - fb;")
            self.set_flags(fl_sub(None), live)
            return
        elif op == 0x08:
            self.emit("fr = rt_shift(fa, fb, 0, &fc);")
            mk = fl_shift()
        elif op == 0x0A:
            self.emit("fr = rt_mpyi(fa, fb, &fw);")
            mk = fl_mpyi()
        elif op == 0x0B:
            self.emit("fr = fa | fb;")
            mk = fl_logic()
        elif op == 0x0C:
            self.emit("fc = ST & 1u; fr = fa - fb - fc;")
            mk = fl_sub("fc")
        elif op == 0x0E:
            self.emit("fr = fa - fb;")
            mk = fl_sub(None)
        elif op == 0x0F:
            self.emit("fr = fa & fb;")
            self.set_flags(fl_logic(), live)
            return
        elif op == 0x10:
            self.emit("fr = fa ^ fb;")
            mk = fl_logic()
        else:
            self.emit("return rt_unknown(0x%06XU);" % i.addr)
            return
        self.set_r(d, "fr")
        if fl:
            self.set_flags(mk, live)

    def gen_condload(self, i, live):
        w = i.w
        cond = (w >> 23) & 0x1F
        g = (w >> 21) & 3
        d = (w >> 16) & 0x1F
        f = w & 0xFFFF
        if (w >> 28) & 1:
            if cond == 0 and g == 3 and d == R_DP:     # LDP
                self.dp = f & 0xFF
                self.emit("%s = 0x%XU;" % (reg(d), sext(f, 16) & 0xFFFFFFFF))
                return
            s = self.src_int(g, f)
            self.emit("t0 = %s;" % s)
            c = self.cond_expr(cond)
            if d == R_DP:
                self.dp = None
            if d == R_ST:
                self.materialize()
            if ext(d):
                self.ri(d)          # exponente al dia antes de escribir la mantisa
                self.emit("if (%s) { C.r[%d] = t0; C.rk[%d] = 2; }" % (c, d, d))
                self.known[d] = 2
                self.rkset.pop(d, None)
            else:
                self.emit("if (%s) { %s = t0;%s }" % (c, reg(d), " C.next_event = 0;" if d in SPECIAL_WRITE else ""))
        else:
            s = self.src_flt(g, f)
            self.emit("x0 = %s;" % s)
            c = self.cond_expr(cond)
            if ext(d):
                self.emit("if (%s) { C.f[%d] = x0; C.rk[%d] = 1; }" % (c, d, d))
                self.known.pop(d, None)
                self.rkset.pop(d, None)
            else:
                self.emit("if (%s) %s = float_to_c3x(x0);" % (c, reg(d)))

    def gen_par_mpy(self, i, live):
        w = i.w
        op = (w >> 26) & 3
        p = (w >> 24) & 3
        d1 = (w >> 23) & 1
        d2 = 2 + ((w >> 22) & 1)
        r1, r2 = (w >> 19) & 7, (w >> 16) & 7
        e3 = self.ind8((w >> 8) & 0xFF)
        e4 = self.ind8(w & 0xFF)
        if op < 2:
            self.emit("x0 = %s; x1 = %s; x2 = c3x_to_float(RD(%s)); x3 = c3x_to_float(RD(%s));"
                      % (self.rf(r1), self.rf(r2), e3, e4))
            names = ("x0", "x1", "x2", "x3")
        else:
            self.emit("t0 = %s; t1 = %s; t2 = RD(%s); t3 = RD(%s);" % (self.ri(r1), self.ri(r2), e3, e4))
            names = ("t0", "t1", "t2", "t3")
        s1, s2, s3, s4 = names
        ma, mb, aa, ab = {0: (s3, s4, s1, s2), 1: (s3, s1, s4, s2),
                          2: (s1, s2, s3, s4), 3: (s3, s1, s2, s4)}[p]
        if op < 2:
            self.emit("xa = %s; xb = %s; xr = %s %s %s;" % (aa, ab, "xa", "+" if op == 0 else "-", "xb"))
            self.wf(d1, "%s * %s" % (ma, mb))
            self.wf(d2, "xr")
            self.set_flags(fl_flt(), live)
        else:
            self.emit("fa = %s; fb = %s; fr = fa %s fb;" % (aa, ab, "+" if op == 2 else "-"))
            self.wi(d1, "rt_mpyi(%s, %s, 0)" % (ma, mb))
            self.wi(d2, "fr")
            self.set_flags(fl_add("0") if op == 2 else fl_sub(None), live)

    def gen_par_store(self, i, live):
        w = i.w
        op = (w >> 25) & 0x1F
        r1, rs1, rs2 = (w >> 22) & 7, (w >> 19) & 7, (w >> 16) & 7
        e3 = self.ind8((w >> 8) & 0xFF)
        e2 = self.ind8(w & 0xFF)
        if op == 0x00:
            self.emit("t0 = float_to_c3x(%s); t1 = float_to_c3x(%s); WR(%s, t0); WR(%s, t1);"
                      % (self.rf(r1), self.rf(rs2), e2, e3))
            return
        if op == 0x01:
            self.emit("t0 = %s; t1 = %s; WR(%s, t0); WR(%s, t1);" % (self.ri(r1), self.ri(rs2), e2, e3))
            return
        if op == 0x02:
            self.emit("x0 = c3x_to_float(RD(%s)); x1 = c3x_to_float(RD(%s));" % (e2, e3))
            self.wf(r1, "x0")
            self.wf(rs1, "x1")
            return
        if op == 0x03:
            self.emit("t0 = RD(%s); t1 = RD(%s);" % (e2, e3))
            self.wi(r1, "t0")
            self.wi(rs1, "t1")
            return
        fstore = op in (0x04, 0x06, 0x0B, 0x0C, 0x0F, 0x11, 0x15)
        if fstore:
            self.emit("t3 = float_to_c3x(%s); t2 = RD(%s);" % (self.rf(rs2), e2))
        else:
            self.emit("t3 = %s; t2 = RD(%s);" % (self.ri(rs2), e2))
        fv = "c3x_to_float(t2)"
        FI = lambda: self.ri(rs1)
        FF = lambda: self.rf(rs1)
        flt_ops = {
            0x04: lambda: "fabsf(%s)" % fv,
            0x06: lambda: "%s + %s" % (fv, FF()),
            0x0B: lambda: "(float)(int32_t)t2",
            0x0C: lambda: fv,
            0x0F: lambda: "%s * %s" % (fv, FF()),
            0x11: lambda: "-%s" % fv,
            0x15: lambda: "%s - %s" % (fv, FF()),
        }
        if op in flt_ops:
            self.emit("xr = %s;" % flt_ops[op]())
            self.wf(r1, "xr")
            mk = fl_flt()
        else:
            if op == 0x05:
                self.emit("fr = ((int32_t)t2 < 0) ? (uint32_t)(-(int64_t)(int32_t)t2) : t2;"); mk = fl_logic()
            elif op == 0x07:
                self.emit("fa = t2; fb = %s; fr = fa + fb;" % FI()); mk = fl_add("0")
            elif op == 0x08:
                self.emit("fr = t2 & %s;" % FI()); mk = fl_logic()
            elif op == 0x09:
                self.emit("fr = rt_shift(t2, %s, 1, &fc);" % FI()); mk = fl_shift()
            elif op == 0x0A:
                self.emit("fr = rt_fix(%s, 0);" % fv); mk = fl_logic()
            elif op == 0x0D:
                self.emit("fr = t2;"); mk = fl_logic()
            elif op == 0x0E:
                self.emit("fr = rt_shift(t2, %s, 0, &fc);" % FI()); mk = fl_shift()
            elif op == 0x10:
                self.emit("fr = rt_mpyi(t2, %s, &fw);" % FI()); mk = fl_mpyi()
            elif op == 0x12:
                self.emit("fa = 0; fb = t2; fr = fa - fb;"); mk = fl_sub(None)
            elif op == 0x13:
                self.emit("fr = ~t2;"); mk = fl_logic()
            elif op == 0x14:
                self.emit("fr = t2 | %s;" % FI()); mk = fl_logic()
            elif op == 0x16:
                self.emit("fa = t2; fb = %s; fr = fa - fb;" % FI()); mk = fl_sub(None)
            elif op == 0x17:
                self.emit("fr = t2 ^ %s;" % FI()); mk = fl_logic()
            else:
                self.emit("return rt_unknown(0x%06XU);" % i.addr)
                return
            self.wi(r1, "fr")
        self.emit("WR(%s, t3);" % e3)
        self.set_flags(mk, live)

    def gen_simple(self, i, live):
        """Instruccion sin control de flujo."""
        self.nea = 0
        self.sfx = "1" if (self.pending is not None and self.pending.sfx == "0") else "0"
        start = len(self.out)
        self._gen_simple(i, live)
        for k in range(start, len(self.out)):
            line = self.out[k]
            if not line.startswith(MARK):
                self.out[k] = MARK + TEMP_RE.sub(r"\g<1>" + self.sfx, line)

    def _gen_simple(self, i, live):
        top = i.w >> 29
        if top == 0:
            self.gen_general(i, live)
        elif top == 1:
            self.gen_three(i, live)
        elif top == 2:
            self.gen_condload(i, live)
        elif (i.w >> 30) == 2:
            self.gen_par_mpy(i, live)
        else:
            self.gen_par_store(i, live)

    # ---------------- flags por instruccion ----------------

    def flag_effect(self, i):
        """(usa, define) de flags de una instruccion simple."""
        NOC = FV | FZ | FN | FUF
        w = i.w
        top = w >> 29
        if i.kind in ("call", "callreg", "trap"):
            return FALL, FALL
        if i.kind in ("bcond", "breg", "db", "dbreg", "callcond", "rets", "reti"):
            return COND_USE.get(i.cond, FALL), 0
        if i.kind in ("br", "rptb", "swi", "idle"):
            return 0, 0
        if top == 0:
            op = (w >> 23) & 0x3F
            d = (w >> 16) & 0x1F
            g = (w >> 21) & 3
            use = 0
            if op in (0x02, 0x16, 0x2D, 0x31, 0x24, 0x26):
                use |= FC
            if (g == 0 and (w & 0x1F) == R_ST) or (op in (0x1E, 0x2A, 0x2B) and d == R_ST):
                use |= FALL
            nodef = (0x0C, 0x0D, 0x12, 0x19, 0x1E, 0x1F, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2E, 0x36)
            if op in nodef:
                return use, 0
            if d == R_ST:
                if op not in (0x10, 0x11, 0x1C, 0x0E, 0x0F):
                    use |= FALL
                return use, FALL
            withc = (0x02, 0x04, 0x07, 0x09, 0x13, 0x16, 0x18, 0x23, 0x24, 0x25, 0x26,
                     0x2D, 0x30, 0x31, 0x33)
            defs = FALL if op in withc else NOC
            if op in (0x08, 0x09, 0x34):
                return use, defs
            return (use, defs) if ext(d) else (use, 0)
        if top == 1:
            op = (w >> 23) & 0x3F
            d = (w >> 16) & 0x1F
            use = FC if op in (0x00, 0x0C) else 0
            defs = FALL if op in (0x00, 0x02, 0x05, 0x07, 0x08, 0x0C, 0x0E) else NOC
            if op in (0x06, 0x07, 0x0F) or ext(d):
                return use, defs
            return use, 0
        if top == 2:
            return COND_USE.get((w >> 23) & 0x1F, FALL), 0
        if (w >> 30) == 2:
            op = (w >> 26) & 3
            return 0, (FALL if op >= 2 else NOC)
        op = (w >> 25) & 0x1F
        if op <= 3:
            return 0, 0
        return 0, (FALL if op in (0x07, 0x09, 0x0E, 0x12, 0x16) else NOC)


# --------------------------------------------------------------------------
# Bloques
# --------------------------------------------------------------------------

def block_of(prog, start):
    """Lista de instrucciones de un bloque (sin ranuras de retardo) y su fin."""
    seq = []
    a = start
    while True:
        i = prog.get(a)
        seq.append(i)
        if not i.valid:
            break
        if i.kind == "rpts":
            seq.append(prog.get(a + 1))
            a += 2
            if a in prog.leaders or a not in prog.code:
                break
            continue
        if i.kind in ("br", "bcond", "breg", "db", "dbreg", "call", "callcond", "callreg",
                      "rets", "reti", "trap", "swi"):
            break
        if a in prog.rptb_ends:
            break
        a += 1
        if a in prog.leaders or a not in prog.code:
            break
    return seq


def successors(prog, seq):
    last = seq[-1]
    a = last.addr
    if not last.valid:
        return None
    if a in prog.rptb_ends and last.kind == "op":
        return sorted(prog.rptb_ends[a]) + [a + 1]
    k = last.kind
    nxt = a + (4 if last.delayed else 1)
    if k == "br":
        return [last.target]
    if k == "bcond":
        return [last.target] if last.cond == 0 else [last.target, nxt]
    if k == "db":
        return [last.target, nxt]
    if k in ("breg", "dbreg", "rets", "reti", "swi", "trap", "callreg"):
        return None
    if k in ("call", "callcond"):
        return [a + 1]
    return [a + 1] if (a + 1) in prog.code else None


def liveness(prog, gen, blocks):
    """Flags vivas a la salida de cada bloque (punto fijo)."""
    info = {}
    for start, seq in blocks.items():
        use = defs = 0
        insts = list(seq)
        last = seq[-1]
        if last.delayed:
            # la condicion del salto se evalua antes de las ranuras de retardo
            insts = seq + [prog.get(last.addr + k) for k in (1, 2, 3)]
        for i in insts:
            u, d = gen.flag_effect(i)
            use |= u & ~defs
            defs |= d
        info[start] = (use, defs, successors(prog, seq))
    live_in = {s: FALL for s in blocks}
    changed = True
    while changed:
        changed = False
        for s, (use, defs, succ) in info.items():
            if succ is None:
                out = FALL
            else:
                out = 0
                for t in succ:
                    out |= live_in.get(t, FALL)
            new = use | (out & ~defs)
            if new != live_in[s]:
                live_in[s] = new
                changed = True
    live_out = {}
    for s, (use, defs, succ) in info.items():
        if succ is None:
            live_out[s] = FALL
        else:
            out = 0
            for t in succ:
                out |= live_in.get(t, FALL)
            live_out[s] = out
    return live_out


def gen_region(prog, gen, rstart, rend, blocks, live_out, out):
    gen.out = out
    gen.region = (rstart, rend)
    leaders = sorted(a for a in blocks if rstart <= a < rend)
    out.append("uint32_t rg_%06X(uint32_t entry)" % rstart)
    out.append("{")
    out.append("    uint32_t fa0 = 0, fb0 = 0, fr0 = 0, fc0 = 0, fa1 = 0, fb1 = 0, fr1 = 0, fc1 = 0;")
    out.append("    int64_t fw0 = 0, fw1 = 0; float xa0 = 0, xb0 = 0, xr0 = 0, xa1 = 0, xb1 = 0, xr1 = 0;")
    out.append("    uint32_t t0 = 0, t1 = 0, t2 = 0, t3 = 0;")
    out.append("    uint32_t ea0 = 0, ea1 = 0, ea2 = 0, n = 0, tj = 0; int bc = 0;")
    out.append("    float x0 = 0, x1 = 0, x2 = 0, x3 = 0;")
    out.append("    (void)fa0; (void)fb0; (void)fr0; (void)fc0; (void)fa1; (void)fb1; (void)fr1; (void)fc1;")
    out.append("    (void)fw0; (void)fw1; (void)xa0; (void)xb0; (void)xr0; (void)xa1; (void)xb1; (void)xr1;")
    out.append("    (void)t0; (void)t1; (void)t2; (void)t3;")
    out.append("    (void)ea0; (void)ea1; (void)ea2; (void)n; (void)tj; (void)bc;")
    out.append("    (void)x0; (void)x1; (void)x2; (void)x3;")
    out.append("    switch (entry) {")
    for a in leaders:
        out.append("    case 0x%06X: goto L_%06X;" % (a, a))
    out.append("    default: return rt_unknown(entry);")
    out.append("    }")
    for a in leaders:
        gen_block(prog, gen, a, blocks[a], live_out[a])
    out.append("}")
    for k in range(len(out)):
        if out[k].startswith(MARK):
            out[k] = out[k][1:]
    out.append("")


def gen_block(prog, gen, start, seq, live_out):
    gen.pending = None
    gen.dp = None
    gen.known = {}; gen.rkset = {}
    gen.out.append("L_%06X:" % start)
    gen.emit("RT_TRACE(0x%06XU);" % start)
    if start in HLE_HOOKS:
        gen.emit("return %s();" % HLE_HOOKS[start])
        return
    if start == gen.region[0] or start in prog.backward_heads or start in prog.vectors:
        gen.emit("RT_CHECK(0x%06XU);" % start)
    last = seq[-1]
    count = len(seq) + (3 if last.delayed else 0)
    gen.emit("C.cycles += %d;" % count)
    if start in IDLE_HINTS:
        pre, cond = IDLE_HINTS[start]
        gen.emit("%s if (%s) rt_idle();" % (pre, cond))

    # Vida de flags tras cada instruccion (hacia atras desde live_out)
    insts = list(seq)
    lives = [0] * len(insts)
    live = live_out
    tail_slots = []
    if last.delayed:
        tail_slots = [prog.get(last.addr + k) for k in (1, 2, 3)]
        for s in reversed(tail_slots):
            u, d = gen.flag_effect(s)
            live = (live & ~d) | u
        # la condicion del salto se evalua antes de las ranuras
    for idx in range(len(insts) - 1, -1, -1):
        lives[idx] = live
        u, d = gen.flag_effect(insts[idx])
        live = (live & ~d) | u

    idx = 0
    while idx < len(insts):
        i = insts[idx]
        if not i.valid:
            gen.materialize()
            gen.emit("return rt_unknown(0x%06XU);" % i.addr)
            return
        if i.kind == "rpts":
            body = insts[idx + 1]
            w = i.w
            g = (w >> 21) & 3
            gen.nea = 0
            cnt = gen.src_int(g, w & 0xFFFF, unsigned=True)
            gen.materialize()
            gen.emit("C.r[%d] = %s; n = C.r[%d]; C.r[%d] = C.r[%d] = 0x%06XU;"
                     % (R_RC, cnt, R_RC, R_RS, R_RE, i.addr + 1))
            gen.emit("C.cycles += n;")
            gen.emit("do {")
            gen.known = {}; gen.rkset = {}
            saved = gen.dp
            gen.gen_simple(body, lives[idx + 1] | FALL)
            gen.materialize()
            gen.emit("} while ((int32_t)--n >= 0);")
            gen.emit("C.r[%d] = 0xFFFFFFFFU;" % R_RC)
            gen.known = {}; gen.rkset = {}
            if gen.dp != saved:
                gen.dp = None
            idx += 2
            continue
        if i.kind == "op" or i.kind == "idle":
            gen.gen_simple(i, lives[idx])
            if i.addr in prog.rptb_ends:
                gen_rptb_end(prog, gen, i.addr, live_out)
            idx += 1
            continue
        gen_flow(prog, gen, i, tail_slots, live_out, lives[idx])
        return
    # cae al siguiente bloque
    if live_out:
        gen.materialize()
    nxt = seq[-1].addr + 1
    if seq[-1].kind == "rpts" or (len(seq) >= 2 and seq[-2].kind == "rpts"):
        nxt = seq[-1].addr + 1
    if nxt in prog.code:
        gen.emit(gen.jump(nxt))
    else:
        gen.emit("return rt_unknown(0x%06XU);" % nxt)


def gen_rptb_end(prog, gen, addr, live_out):
    rss = sorted(prog.rptb_ends[addr])
    if live_out:
        gen.materialize()
    gen.emit("if ((ST & 0x%XU) && C.r[%d] == 0x%06XU) {" % (ST_RM, R_RE, addr))
    gen.emit("    if ((int32_t)--C.r[%d] >= 0) {" % R_RC)
    if len(rss) == 1:
        gen.emit("        if (C.r[%d] == 0x%06XU) %s" % (R_RS, rss[0], gen.jump(rss[0])))
    gen.emit("        return C.r[%d] & 0xFFFFFFU;" % R_RS)
    gen.emit("    }")
    gen.emit("    ST &= ~0x%XU;" % ST_RM)
    gen.emit("}")


def is_pure(ins):
    """Sin escrituras a memoria ni llamadas (solo lee y compara)."""
    if not ins.valid or ins.kind != "op":
        return False
    w = ins.w
    top = w >> 29
    if top == 0:
        op = (w >> 23) & 0x3F
        d = (w >> 16) & 0x1F
        if op in (0x1C, 0x1D, 0x1E, 0x1F, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x0C, 0x36):
            return False
        return d not in (R_SP, R_ST, R_IE, R_IF)
    if top in (1, 2):
        return ((w >> 16) & 0x1F) not in (R_SP, R_ST, R_IE, R_IF)
    return False


def is_idle_loop(prog, br, slots):
    """Bucle de espera: salta a su propio inicio y solo lee."""
    t = br.target
    if t is None or t > br.addr or br.addr - t > 8:
        return False
    if not (t in prog.leaders):
        return False
    for a in range(t, br.addr):
        if not is_pure(prog.get(a)):
            return False
        if a != t and a in prog.leaders:
            return False
    return all(is_pure(s) for s in slots)


def gen_flow(prog, gen, i, slots, live_out, live_here):
    k = i.kind
    a = i.addr
    w = i.w
    nxt = a + (4 if i.delayed else 1)
    gen.nea = 0

    def emit_slots():
        for s in slots:
            if not s.valid:
                gen.emit("return rt_unknown(0x%06XU);" % s.addr)
                return
            gen.gen_simple(s, FALL)

    if k == "br":
        if i.delayed:
            emit_slots()
        gen.materialize() if live_out else None
        gen.emit(gen.jump(i.target))
        return
    if k in ("bcond", "breg"):
        c = gen.cond_expr(i.cond)
        if k == "breg":
            gen.emit("tj = C.r[%d] & 0xFFFFFFU;" % (w & 0x1F))
        gen.emit("bc = %s;" % c)
        if i.delayed:
            emit_slots()
        if live_out:
            gen.materialize()
        if k == "breg":
            gen.emit("if (bc) return tj;")
        elif is_idle_loop(prog, i, slots):
            gen.emit("if (bc) { rt_idle(); %s }" % gen.jump(i.target))
        else:
            gen.emit("if (bc) %s" % gen.jump(i.target))
        if i.cond != 0:
            gen.emit(gen.jump(nxt) if nxt in prog.code else "return rt_unknown(0x%06XU);" % nxt)
        else:
            gen.emit("return rt_unknown(0x%06XU);" % a)
        return
    if k in ("db", "dbreg"):
        c = gen.cond_expr(i.cond)
        ar = reg(8 + i.dbreg)
        if k == "dbreg":
            gen.emit("tj = C.r[%d] & 0xFFFFFFU;" % (w & 0x1F))
        gen.emit("bc = %s;" % c)
        gen.emit("%s = (%s & 0xFF000000U) | ((%s - 1) & 0xFFFFFFU);" % (ar, ar, ar))
        gen.emit("bc = bc && !(%s & 0x800000U);" % ar)
        if i.delayed:
            emit_slots()
        if live_out:
            gen.materialize()
        if k == "dbreg":
            gen.emit("if (bc) return tj;")
        else:
            gen.emit("if (bc) %s" % gen.jump(i.target))
        gen.emit(gen.jump(nxt) if nxt in prog.code else "return rt_unknown(0x%06XU);" % nxt)
        return
    if k in ("call", "callcond", "callreg"):
        ret = a + 1
        if k == "callcond":
            c = gen.cond_expr(i.cond)
            gen.materialize()
            gen.emit("if (%s) {" % c)
        else:
            gen.materialize()
            gen.emit("{")
        if k == "callreg":
            gen.emit("    t3 = C.r[%d] & 0xFFFFFFU;" % (w & 0x1F))
            gen.emit("    PUSH(0x%06XU);" % ret)
            gen.emit("    n = rt_dispatch(t3);")
        else:
            gen.emit("    PUSH(0x%06XU);" % ret)
            if i.target in prog.region_set:
                gen.emit("    n = rg_%06X(0x%06XU);" % (i.target, i.target))
            else:
                gen.emit("    n = rt_dispatch(0x%06XU);" % i.target)
        gen.emit("    if (n != 0x%06XU) return n;" % ret)
        gen.emit("}")
        gen.emit(gen.jump(ret) if ret in prog.code else "return rt_unknown(0x%06XU);" % ret)
        return
    if k in ("rets", "reti"):
        c = gen.cond_expr(i.cond)
        gen.materialize()
        if k == "rets":
            body = "return POP() & 0xFFFFFFU;"
        else:
            body = "{ t3 = POP() & 0xFFFFFFU; ST |= 0x%XU; C.next_event = 0; return t3; }" % ST_GIE
        if i.cond == 0:
            gen.emit(body)
        else:
            gen.emit("if (%s) %s" % (c, body))
            gen.emit(gen.jump(a + 1) if (a + 1) in prog.code else "return rt_unknown(0x%06XU);" % (a + 1))
        return
    if k == "trap":
        c = gen.cond_expr(i.cond)
        gen.materialize()
        gen.emit("if (%s) {" % c)
        gen.emit("    PUSH(0x%06XU); ST &= ~0x%XU;" % (a + 1, ST_GIE))
        gen.emit("    n = rt_call(RD(0x%02X) & 0xFFFFFFU, 0x%06XU);" % (0x20 + (w & 0x1F), a + 1))
        gen.emit("}")
        gen.emit(gen.jump(a + 1) if (a + 1) in prog.code else "return rt_unknown(0x%06XU);" % (a + 1))
        return
    if k == "rptb":
        gen.emit("C.r[%d] = 0x%06XU; C.r[%d] = 0x%06XU; ST |= 0x%XU;" % (R_RS, a + 1, R_RE, i.target, ST_RM))
        if live_out:
            gen.materialize()
        gen.emit(gen.jump(a + 1))
        return
    gen.emit("return rt_unknown(0x%06XU);" % a)


# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("program")
    ap.add_argument("outdir")
    ap.add_argument("--coverage")
    ap.add_argument("--per-file", type=int, default=150)
    a = ap.parse_args()

    data = open(a.program, "rb").read()
    words = list(struct.unpack("<%dI" % (len(data) // 4), data))
    # Mismos parches que vu_skip_memtests() (src/vunit/mem.c)
    for addr, word in ((0x004AFF, 0x620062D5), (0x004B17, 0x62006381)):
        if words[addr] == word:
            words[addr] = 0x0C800000
    cov = None
    if a.coverage and os.path.exists(a.coverage):
        with open(a.coverage, "rb") as f:
            cov = f.read()

    prog = build(words, cov)
    gen = Gen(prog)

    # Bloques: uno por lider
    blocks = {}
    for s in sorted(prog.leaders):
        blocks[s] = block_of(prog, s)
    live_out = liveness(prog, gen, blocks)

    starts = prog.region_starts
    bounds = []
    for n, s in enumerate(starts):
        e = starts[n + 1] if n + 1 < len(starts) else range_end(s)
        e = min(e, range_end(s))
        bounds.append((s, e))

    os.makedirs(a.outdir, exist_ok=True)
    header = ['/* Generado por tools/c31recomp.py a partir de la ROM. No editar ni versionar. */',
              '#include <math.h>', '#include "rt.h"', '']
    decls = []
    for s, e in bounds:
        decls.append("uint32_t rg_%06X(uint32_t entry);" % s)
    with open(os.path.join(a.outdir, "rc_decls.h"), "w") as f:
        f.write("\n".join(header[:1] + ["#include <stdint.h>"] + decls) + "\n")

    files = []
    for chunk in range(0, len(bounds), a.per_file):
        out = header[:] + ['#include "rc_decls.h"', '']
        for s, e in bounds[chunk:chunk + a.per_file]:
            gen_region(prog, gen, s, e, blocks, live_out, out)
        name = "rc_%03d.c" % (chunk // a.per_file)
        with open(os.path.join(a.outdir, name), "w") as f:
            f.write("\n".join(out) + "\n")
        files.append(name)

    table = header[:] + ['#include "rc_decls.h"', '', 'const rt_region rt_regions[] = {']
    for s, e in bounds:
        table.append("    { 0x%06XU, 0x%06XU, rg_%06X }," % (s, e, s))
    table += ['};', 'const unsigned rt_num_regions = %d;' % len(bounds), '']
    with open(os.path.join(a.outdir, "rc_table.c"), "w") as f:
        f.write("\n".join(table))
    files.append("rc_table.c")

    with open(os.path.join(a.outdir, "files.mk"), "w") as f:
        f.write("RECOMP_SRCS = " + " ".join(os.path.join(a.outdir, x) for x in files) + "\n")

    print("codigo: %d instrucciones, %d bloques, %d regiones, %d ficheros"
          % (len(prog.code), len(blocks), len(bounds), len(files)))


if __name__ == "__main__":
    main()
