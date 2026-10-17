#!/usr/bin/env python3
"""
Rotulos del port con la tipografia del juego: "PRESS X FOR HIDDEN CARS",
etc., para las pantallas de elegir coche y pista (secretos del arcade).

Como make_loading.py, saca la tipografia de 18 pixeles de la WAVERAM (frame
1440, eleccion de carrera) con build/cusa_recomp y escribe
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
TEX_H = 128


def main():
    with tempfile.TemporaryDirectory() as tmp:
        font = texdump(tmp, 1440, 0x305B, 0x59, 40, "font.ppm", INPUT)
    img = [[None] * 512 for _ in range(TEX_H)]
    widths = []
    for r, text in enumerate(TEXTS):
        glyphs = [None if ch == ' ' else FONT18[ch] for ch in text]
        width = sum(6 if g is None else g[1] - g[0] + 2 for g in glyphs)
        widths.append(width)
        x0 = 0
        for g in glyphs:
            if g is None:
                x0 += 6
                continue
            gx0, gx1, gy = g
            for y in range(GLYPH_H):
                for x in range(gx0, gx1 + 1):
                    p = font[gy + y][x]
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
