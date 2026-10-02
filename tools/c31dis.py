#!/usr/bin/env python3
"""Desensamblador TMS320C3x (C30/C31) para la imagen de programa de Cruis'n USA.

Es la base del recompilador estatico: decode() devuelve una estructura con
mnemonico, operandos y flujo de control, y format() la convierte en texto con
la sintaxis de asm30 de TI para poder compararla con la fuente original.

Uso:
    c31dis.py program.bin <inicio_hex> <num_palabras>
"""
import struct
import sys

REGS = ["R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
        "AR0", "AR1", "AR2", "AR3", "AR4", "AR5", "AR6", "AR7",
        "DP", "IR0", "IR1", "BK", "SP", "ST", "IE", "IF",
        "IOF", "RS", "RE", "RC", "R28?", "R29?", "R30?", "R31?"]

CONDS = ["U", "LO", "LS", "HI", "HS", "EQ", "NE", "LT",
         "LE", "GT", "GE", "C0B?", "NV", "V", "NUF", "UF",
         "NLV", "LV", "NLUF", "LUF", "ZUF", "C15?", "C16?", "C17?",
         "C18?", "C19?", "C1A?", "C1B?", "C1C?", "C1D?", "C1E?", "C1F?"]

# Formato general de 2 operandos (bits 31-29 = 000, opcode en 28-23).
# kind: 'i' entero con signo, 'u' entero sin signo (logicos), 'f' flotante,
#       's' store (dst es memoria), 'n' sin operandos, '1' un solo operando
OPS2 = {
    0x00: ("ABSF", "f"), 0x01: ("ABSI", "i"), 0x02: ("ADDC", "i"), 0x03: ("ADDF", "f"),
    0x04: ("ADDI", "i"), 0x05: ("AND", "u"), 0x06: ("ANDN", "u"), 0x07: ("ASH", "i"),
    0x08: ("CMPF", "f"), 0x09: ("CMPI", "i"), 0x0A: ("FIX", "f"), 0x0B: ("FLOAT", "i"),
    0x0C: ("IDLE", "n"), 0x0D: ("LDE", "f"), 0x0E: ("LDF", "f"), 0x0F: ("LDFI", "f"),
    0x10: ("LDI", "i"), 0x11: ("LDII", "i"), 0x12: ("LDM", "f"), 0x13: ("LSH", "i"),
    0x14: ("MPYF", "f"), 0x15: ("MPYI", "i"), 0x16: ("NEGB", "i"), 0x17: ("NEGF", "f"),
    0x18: ("NEGI", "i"), 0x19: ("NOP", "nop"), 0x1A: ("NORM", "f"), 0x1B: ("NOT", "u"),
    0x1C: ("POP", "1"), 0x1D: ("POPF", "1"), 0x1E: ("PUSH", "1"), 0x1F: ("PUSHF", "1"),
    0x20: ("OR", "u"), 0x21: ("OP21?", "i"), 0x22: ("RND", "f"), 0x23: ("ROL", "1"),
    0x24: ("ROLC", "1"), 0x25: ("ROR", "1"), 0x26: ("RORC", "1"), 0x27: ("RPTS", "rpts"),
    0x28: ("STF", "s"), 0x29: ("STFI", "s"), 0x2A: ("STI", "s"), 0x2B: ("STII", "s"),
    0x2C: ("SIGI", "n"), 0x2D: ("SUBB", "i"), 0x2E: ("SUBC", "u"), 0x2F: ("SUBF", "f"),
    0x30: ("SUBI", "i"), 0x31: ("SUBRB", "i"), 0x32: ("SUBRF", "f"), 0x33: ("SUBRI", "i"),
    0x34: ("TSTB", "u"), 0x35: ("XOR", "u"), 0x36: ("IACK", "iack"),
}

OPS3 = ["ADDC3", "ADDF3", "ADDI3", "AND3", "ANDN3", "ASH3", "CMPF3", "CMPI3",
        "LSH3", "MPYF3", "MPYI3", "OR3", "SUBB3", "SUBF3", "SUBI3", "TSTB3", "XOR3"]

# Instrucciones paralelas (bits 31-30 = 11), opcode en 29-25.
# formato: (op1, op2, tipo) ; tipo indica como se reparten los campos.
PAR_ST = {
    0x00: ("STF", "STF", "stst"), 0x01: ("STI", "STI", "stst"),
    0x02: ("LDF", "LDF", "ldld"), 0x03: ("LDI", "LDI", "ldld"),
    0x04: ("ABSF", "STF", "un"), 0x05: ("ABSI", "STI", "un"),
    0x06: ("ADDF3", "STF", "bin"), 0x07: ("ADDI3", "STI", "bin"),
    0x08: ("AND3", "STI", "bin"), 0x09: ("ASH3", "STI", "bin_r"),
    0x0A: ("FIX", "STI", "un"), 0x0B: ("FLOAT", "STF", "un"),
    0x0C: ("LDF", "STF", "un"), 0x0D: ("LDI", "STI", "un"),
    0x0E: ("LSH3", "STI", "bin_r"), 0x0F: ("MPYF3", "STF", "bin"),
    0x10: ("MPYI3", "STI", "bin"), 0x11: ("NEGF", "STF", "un"),
    0x12: ("NEGI", "STI", "un"), 0x13: ("NOT", "STI", "un"),
    0x14: ("OR3", "STI", "bin"), 0x15: ("SUBF3", "STF", "bin_r"),
    0x16: ("SUBI3", "STI", "bin_r"), 0x17: ("XOR3", "STI", "bin"),
}
PAR_MPY = ["ADDF3", "SUBF3", "ADDI3", "SUBI3"]


def sext(v, bits):
    m = 1 << (bits - 1)
    return (v ^ m) - m


def short_float(v):
    """Flotante corto de 16 bits del C3x: exp(4, con signo) | s | frac(11)."""
    e = sext(v >> 12, 4)
    if e == -8:
        return 0.0
    s = (v >> 11) & 1
    frac = v & 0x7FF
    if s:
        mant = -2.0 + frac / 2048.0
    else:
        mant = 1.0 + frac / 2048.0
    return mant * (2.0 ** e)


def indirect(mod, ar, disp, implicit=False):
    """Modo de direccionamiento indirecto. implicit=True: desplazamiento implicito 1."""
    a = "AR%d" % ar
    if mod < 0x18:
        if mod < 8:
            d = "(%d)" % disp if (not implicit or mod < 2) else ""
        else:
            d = "(IR0)" if mod < 0x10 else "(IR1)"
        tmpl = ["*+%s%s", "*-%s%s", "*++%s%s", "*--%s%s",
                "*%s++%s", "*%s--%s", "*%s++%s%%", "*%s--%s%%"][mod & 7]
        return tmpl % (a, d)
    if mod == 0x18:
        return "*%s" % a
    if mod == 0x19:
        return "*%s++(IR0)B" % a
    return "*?MOD%02X?%s" % (mod, a)


def ind8(field):
    return indirect((field >> 3) & 0x1F, field & 7, 1, implicit=True)


def ind16(field):
    return indirect((field >> 11) & 0x1F, (field >> 8) & 7, field & 0xFF)


class Insn:
    __slots__ = ("addr", "word", "mnem", "ops", "flow", "target", "delayed", "cond")

    def __init__(self, addr, word, mnem, ops=(), flow=None, target=None, delayed=False, cond=None):
        self.addr, self.word, self.mnem, self.ops = addr, word, mnem, list(ops)
        self.flow, self.target, self.delayed, self.cond = flow, target, delayed, cond

    def text(self):
        if "||" in self.mnem:
            m1, m2 = self.mnem.split("||")
            return "%-8s%s\n%18s||  %-8s%s" % (m1, self.ops[0], "", m2, self.ops[1])
        if self.ops:
            return "%-8s%s" % (self.mnem, ",".join(self.ops))
        return self.mnem


def src_general(g, src, kind):
    if g == 0:
        return REGS[src & 0x1F]
    if g == 1:
        return "@%04Xh" % src
    if g == 2:
        return ind16(src)
    if kind == "f":
        return repr(short_float(src))
    if kind == "u":
        return "%Xh" % src
    return str(sext(src, 16))


def decode(addr, w):
    top = w >> 29
    if top == 0:
        op = (w >> 23) & 0x3F
        g = (w >> 21) & 3
        dst = (w >> 16) & 0x1F
        src = w & 0xFFFF
        if op not in OPS2:
            return Insn(addr, w, ".word", ["%08Xh" % w])
        m, kind = OPS2[op]
        if kind == "n":
            return Insn(addr, w, m)
        if kind == "nop":
            return Insn(addr, w, m, [src_general(g, src, "i")] if g == 2 else [])
        if kind == "1":
            return Insn(addr, w, m, [REGS[dst]])
        if kind == "s":
            # store: el campo dst es el registro fuente, src es la direccion
            return Insn(addr, w, m, [REGS[dst], src_general(g, src, "i")])
        if kind == "rpts":
            return Insn(addr, w, m, [src_general(g, src, "u")], flow="rpts")
        if kind == "iack":
            return Insn(addr, w, m, [src_general(g, src, "i")])
        if m == "LDI" and g == 3 and dst == 16:
            return Insn(addr, w, "LDP", ["%02Xh" % (src & 0xFF)])
        return Insn(addr, w, m, [src_general(g, src, kind), REGS[dst]])

    if top == 1:
        op = (w >> 23) & 0x3F
        t = (w >> 21) & 3
        dst = REGS[(w >> 16) & 0x1F]
        s1f = (w >> 8) & 0xFF
        s2f = w & 0xFF
        if op >= len(OPS3):
            return Insn(addr, w, ".word", ["%08Xh" % w])
        src1 = ind8(s1f) if t & 1 else REGS[s1f & 0x1F]
        src2 = ind8(s2f) if t & 2 else REGS[s2f & 0x1F]
        m = OPS3[op]
        if m in ("CMPF3", "CMPI3", "TSTB3"):
            return Insn(addr, w, m, [src2, src1])
        return Insn(addr, w, m, [src2, src1, dst])

    if top == 2:
        m = "LDF" if (w >> 28) & 1 == 0 else "LDI"
        cond = (w >> 23) & 0x1F
        g = (w >> 21) & 3
        dst = (w >> 16) & 0x1F
        return Insn(addr, w, m + CONDS[cond], [src_general(g, w & 0xFFFF, "f" if m == "LDF" else "i"), REGS[dst]])

    if top == 3:
        hi = w >> 24
        if hi in (0x60, 0x61):
            tgt = w & 0xFFFFFF
            return Insn(addr, w, "BR" + ("D" if hi == 0x61 else ""), ["%06Xh" % tgt],
                        flow="jump", target=tgt, delayed=hi == 0x61)
        if hi == 0x62:
            tgt = w & 0xFFFFFF
            return Insn(addr, w, "CALL", ["%06Xh" % tgt], flow="call", target=tgt)
        if hi == 0x64:
            tgt = w & 0xFFFFFF
            return Insn(addr, w, "RPTB", ["%06Xh" % tgt], flow="rptb", target=tgt)
        if hi == 0x66:
            return Insn(addr, w, "SWI")
        if (w >> 26) == 0x1A:  # Bcond: 011010 B 00 D cond src
            rel = (w >> 25) & 1
            d = (w >> 21) & 1
            cond = (w >> 16) & 0x1F
            m = "B" + CONDS[cond] + ("D" if d else "")
            if rel:
                tgt = (addr + (3 if d else 1) + sext(w & 0xFFFF, 16)) & 0xFFFFFF
                return Insn(addr, w, m, ["%06Xh" % tgt], flow="branch", target=tgt, delayed=bool(d), cond=cond)
            return Insn(addr, w, m, [REGS[w & 0x1F]], flow="branch_reg", delayed=bool(d), cond=cond)
        if (w >> 26) == 0x1B:  # DBcond: 011011 B ARn(3) D cond src
            rel = (w >> 25) & 1
            ar = (w >> 22) & 7
            d = (w >> 21) & 1
            cond = (w >> 16) & 0x1F
            m = "DB" + CONDS[cond] + ("D" if d else "")
            if rel:
                tgt = (addr + (3 if d else 1) + sext(w & 0xFFFF, 16)) & 0xFFFFFF
                return Insn(addr, w, m, ["AR%d" % ar, "%06Xh" % tgt], flow="dbranch", target=tgt, delayed=bool(d), cond=cond)
            return Insn(addr, w, m, ["AR%d" % ar, REGS[w & 0x1F]], flow="dbranch_reg", delayed=bool(d), cond=cond)
        if (w >> 26) == 0x1C:  # CALLcond: 011100 B 0000 cond src
            rel = (w >> 25) & 1
            cond = (w >> 16) & 0x1F
            if rel:
                tgt = (addr + 1 + sext(w & 0xFFFF, 16)) & 0xFFFFFF
                return Insn(addr, w, "CALL" + CONDS[cond], ["%06Xh" % tgt], flow="ccall", target=tgt, cond=cond)
            return Insn(addr, w, "CALL" + CONDS[cond], [REGS[w & 0x1F]], flow="ccall_reg", cond=cond)
        if (w >> 23) == 0xE8:  # TRAPcond
            cond = (w >> 16) & 0x1F
            return Insn(addr, w, "TRAP" + CONDS[cond], [str(w & 0x1F)], flow="trap", cond=cond)
        if (w >> 23) == 0xF0:
            cond = (w >> 16) & 0x1F
            return Insn(addr, w, "RETI" + CONDS[cond], flow="ret", cond=cond)
        if (w >> 23) == 0xF1:
            cond = (w >> 16) & 0x1F
            return Insn(addr, w, "RETS" + CONDS[cond], flow="ret", cond=cond)
        return Insn(addr, w, ".word", ["%08Xh" % w])

    # Paralelas
    if (w >> 30) == 2:
        op = (w >> 26) & 3
        p = (w >> 24) & 3
        d1 = "R%d" % ((w >> 23) & 1)
        d2 = "R%d" % (2 + ((w >> 22) & 1))
        s1 = "R%d" % ((w >> 19) & 7)
        s2 = "R%d" % ((w >> 16) & 7)
        s3 = ind8((w >> 8) & 0xFF)
        s4 = ind8(w & 0xFF)
        mp = "MPYF3" if op < 2 else "MPYI3"
        ad = PAR_MPY[op]
        # P selecciona que operandos van al multiplicador y cuales al sumador
        (ma, mb), (aa, ab) = {0: ((s3, s4), (s1, s2)), 1: ((s3, s1), (s4, s2)),
                              2: ((s1, s2), (s3, s4)), 3: ((s3, s1), (s2, s4))}[p]
        return Insn(addr, w, mp + "||" + ad, ["%s,%s,%s" % (mb, ma, d1), "%s,%s,%s" % (ab, aa, d2)])

    op = (w >> 25) & 0x1F
    if op not in PAR_ST:
        return Insn(addr, w, ".word", ["%08Xh" % w])
    m1, m2, kind = PAR_ST[op]
    r1 = "R%d" % ((w >> 22) & 7)
    rs1 = "R%d" % ((w >> 19) & 7)
    rs2 = "R%d" % ((w >> 16) & 7)
    i3 = ind8((w >> 8) & 0xFF)
    i2 = ind8(w & 0xFF)
    # Campos: r1 = bits 24-22, rs1 = 21-19, rs2 = 18-16,
    #         i3 = indirecto bits 15-8, i2 = indirecto bits 7-0.
    if kind == "stst":
        return Insn(addr, w, m1 + "||" + m2, ["%s,%s" % (r1, i2), "%s,%s" % (rs2, i3)])
    if kind == "ldld":
        return Insn(addr, w, m1 + "||" + m2, ["%s,%s" % (i2, r1), "%s,%s" % (i3, rs1)])
    if kind == "un":
        return Insn(addr, w, m1 + "||" + m2, ["%s,%s" % (i2, r1), "%s,%s" % (rs2, i3)])
    # binarias: op1 src2,src1,dst1 || ST src3,dst2
    # ASH3/LSH3/SUBF3/SUBI3 no son conmutativas: el registro va primero
    # (verificado con LSH R6,*AR4,R0 de DIRQ.ASM).
    if kind == "bin_r":
        return Insn(addr, w, m1 + "||" + m2, ["%s,%s,%s" % (rs1, i2, r1), "%s,%s" % (rs2, i3)])
    return Insn(addr, w, m1 + "||" + m2, ["%s,%s,%s" % (i2, rs1, r1), "%s,%s" % (rs2, i3)])


def load_words(path):
    with open(path, "rb") as f:
        data = f.read()
    return struct.unpack("<%dI" % (len(data) // 4), data)


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)
    words = load_words(sys.argv[1])
    start = int(sys.argv[2], 16)
    n = int(sys.argv[3], 0)
    for a in range(start, min(start + n, len(words))):
        ins = decode(a, words[a])
        print("%06X  %08X  %s" % (a, words[a], ins.text()))


if __name__ == "__main__":
    main()
