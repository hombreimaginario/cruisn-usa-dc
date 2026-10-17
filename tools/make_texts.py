"""
Rotulos del port con la tipografia pequena del juego: "PRESS X FOR HIDDEN
CARS", etc., para las pantallas de elegir coche y pista (secretos del arcade).

La fuente es la de 7x7 de FONT.ASM (_font1, la de las fichas tecnicas de los
coches), que esta en la ROM de programa (generated/program.bin, palabra
0x122A8): un glifo por caracter ASCII desde '(' (0x28), 7 palabras de 7 bits
(bit 6 = pixel izquierdo). Escribe generated/texts.bin (no se versiona):
  4 palabras: ancho en pixeles (de 512) de cada rotulo
  textura 512x128 ARGB1555 (texel 0 transparente), un rotulo por fila de 24
Uso: tools/make_texts.py [generated]
"""
import os, struct, sys, tempfile

GEN = sys.argv[1] if len(sys.argv) > 1 else "generated"

TEXTS = ["PRESS X FOR HIDDEN CARS!", "PRESS X FOR DEFAULT CARS!",
         "PRESS X FOR HIDDEN TRACKS!", "PRESS X FOR DEFAULT TRACKS!"]
ROW_H = 24
TEX_H = 128

FONT1 = 0x122A8        # _font1 en program.bin (palabras)
GLYPH_W, GLYPH_H, ADV = 7, 7, 8
# la fuente empieza en '(': el signo de exclamacion va dibujado aqui, 3 de ancho
EXTRA = {'!': (0b0110000, 0b0110000, 0b0110000, 0b0110000, 0b0000000, 0b0110000, 0b0110000)}
EXTRA_ADV = {'!': 4}


def argb1555(p):
    if p is None:
        return 0
    r, g, b = p
    return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)


def main():
    prog = open(os.path.join(GEN, "program.bin"), "rb").read()
    words = struct.unpack_from("<%dI" % (len(prog) // 4), prog)
    img = [[None] * 512 for _ in range(TEX_H)]
    widths = []
    for r, text in enumerate(TEXTS):
        widths.append(sum(EXTRA_ADV.get(ch, ADV) for ch in text) - 1)
        x0 = 0
        for ch in text:
            if ch != ' ':
                g = EXTRA[ch] if ch in EXTRA else words[FONT1 + (ord(ch) - 0x28) * GLYPH_H:FONT1 + (ord(ch) - 0x28 + 1) * GLYPH_H]
                for y, row in enumerate(g):
                    for x in range(GLYPH_W):
                        if row >> (6 - x) & 1:
                            img[r * ROW_H + y][x0 + x] = (255, 255, 255)
            x0 += EXTRA_ADV.get(ch, ADV)
    out = bytearray(struct.pack("<4I", *widths))
    for y in range(TEX_H):
        for x in range(512):
            out += struct.pack("<H", argb1555(img[y][x]))
    open(os.path.join(GEN, "texts.bin"), "wb").write(out)
    print("texts.bin: %d bytes, anchos %s" % (len(out), widths))


main()
