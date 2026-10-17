#!/usr/bin/env python3
"""
Rotulos del port con la tipografia del juego: "PRESS X FOR HIDDEN CARS",
etc., para las pantallas de elegir coche y pista (secretos del arcade).

Como make_loading.py, saca de la WAVERAM (frame 1440, eleccion de carrera)
la tipografia pequena del juego (osg10fnt, TEXTTAB.ASM OGSMFONT_TAB, 12
pixeles; la pagina 0x2FB5 es FONT10_A en TEXT.ASM) con build/cusa_recomp. El
color lo pone este script (blanco con borde oscuro, como los iconos TURN TO
SELECT; la paleta del juego para esta fuente no esta cargada en ese frame) y
escribe
generated/texts.bin (no se versiona: sale de la ROM):
  4 palabras: ancho en pixeles (de 512) de cada rotulo
  textura 512x128 ARGB1555 (texel 0 transparente), un rotulo por fila de 24
Uso: tools/make_texts.py [generated] [build/cusa_recomp]
"""
import os, struct, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# make_loading ejecuta main() al importarse: se cargan solo sus definiciones
_src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_loading.py")).read()
_src = _src.replace("\nmain()\n", "\n")
_ns = {"__name__": "make_loading_defs", "sys": sys}
exec(compile(_src, "make_loading.py", "exec"), _ns)
texdump, argb1555, FONT18, GLYPH_H, INPUT = (_ns[k] for k in ("texdump", "argb1555", "FONT18", "GLYPH_H", "INPUT"))
GEN, BIN = _ns["GEN"], _ns["BIN"]

TEXTS = ["PRESS X FOR HIDDEN CARS", "PRESS X FOR NORMAL CARS",
         "PRESS X FOR HIDDEN TRACKS", "PRESS X FOR NORMAL TRACKS"]
ROW_H = 24

# OGSMFONT_TAB (TEXTTAB.ASM): x inicial, x final, fila; A-T en la fila 0,
# U-Z en la 13; 13 filas por letra
OGSM = {
    'A': (0, 9, 0), 'B': (10, 19, 0), 'C': (20, 28, 0), 'D': (29, 38, 0), 'E': (39, 47, 0),
    'F': (48, 56, 0), 'G': (57, 66, 0), 'H': (67, 76, 0), 'I': (77, 81, 0), 'J': (82, 90, 0),
    'K': (91, 100, 0), 'L': (101, 109, 0), 'M': (110, 124, 0), 'N': (125, 134, 0),
    'O': (135, 144, 0), 'P': (145, 154, 0), 'Q': (155, 164, 0), 'R': (165, 174, 0),
    'S': (175, 184, 0), 'T': (185, 193, 0), 'U': (0, 9, 13), 'V': (10, 19, 13),
    'W': (20, 34, 13), 'X': (35, 44, 13), 'Y': (45, 55, 13), 'Z': (56, 65, 13),
}
OGSM_H = 13


def recolor(p):
    """Blanco, gris o borde oscuro segun el brillo del texel en la paleta de
    volcado (0x59, la azul de la fuente de 18)."""
    if p is None:
        return None
    lum = (p[0] * 3 + p[1] * 6 + p[2]) // 10
    if lum > 190:
        return (255, 255, 255)
    if lum > 90:
        return (176, 176, 176)
    return (32, 32, 40)
TEX_H = 128


def main():
    with tempfile.TemporaryDirectory() as tmp:
        font = texdump(tmp, 1440, 0x2FB5, 0x59, 26, "font.ppm", INPUT)
    img = [[None] * 512 for _ in range(TEX_H)]
    widths = []
    for r, text in enumerate(TEXTS):
        glyphs = [None if ch == ' ' else OGSM[ch] for ch in text]
        width = sum(5 if g is None else g[1] - g[0] + 2 for g in glyphs)
        widths.append(width)
        x0 = 0
        for g in glyphs:
            if g is None:
                x0 += 5
                continue
            gx0, gx1, gy = g
            for y in range(OGSM_H):
                for x in range(gx0, gx1 + 1):
                    p = recolor(font[gy + y][x])
                    if p is not None and r * ROW_H + y < TEX_H:
                        img[r * ROW_H + y][x0 + x - gx0] = p
            x0 += gx1 - gx0 + 2
    out = bytearray(struct.pack("<4I", *widths))
    for y in range(TEX_H):
        for x in range(512):
            out += struct.pack("<H", argb1555(img[y][x]))
    open(os.path.join(GEN, "texts.bin"), "wb").write(out)
    print("texts.bin: %d bytes, anchos %s" % (len(out), widths))


main()
