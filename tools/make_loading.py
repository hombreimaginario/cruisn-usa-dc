#!/usr/bin/env python3
"""
Pantalla de carga del port: el logo de Cruis'n USA y "NOW LOADING" con la
tipografia de 18 pixeles del juego, sacados de la propia ROM.

Ejecuta el codigo recompilado en el ordenador (build/cusa_recomp) hasta
frames en los que el juego tiene en la WAVERAM el logo (demo, frame 1000) y
la tipografia (eleccion de carrera, frame 1440) y los vuelca con
CUSA_TEXDUMP. Escribe generated/loading.bin (no se versiona: sale de la ROM):
  logo  256x256 ARGB1555 (texel 0 transparente)
  texto 512x64  ARGB1555
Uso: tools/make_loading.py [generated] [build/cusa_recomp]
"""
import os, subprocess, struct, sys, tempfile

GEN = sys.argv[1] if len(sys.argv) > 1 else "generated"
BIN = sys.argv[2] if len(sys.argv) > 2 else "build/cusa_recomp"
INPUT = "1200:1:5,1220:1:5,1240:1:5,1300:4:5"

# TEXTTAB.ASM FONT18_TAB: (x inicial, x final) en la fila 0; W..Z en la fila 18
FONT18 = {
    'A': (0, 11, 0), 'B': (12, 23, 0), 'C': (24, 33, 0), 'D': (34, 45, 0), 'E': (46, 55, 0),
    'F': (56, 64, 0), 'G': (65, 76, 0), 'H': (77, 88, 0), 'I': (89, 93, 0), 'J': (94, 102, 0),
    'K': (103, 114, 0), 'L': (115, 123, 0), 'M': (124, 142, 0), 'N': (143, 154, 0),
    'O': (155, 166, 0), 'P': (167, 178, 0), 'Q': (179, 190, 0), 'R': (191, 202, 0),
    'S': (203, 213, 0), 'T': (214, 224, 0), 'U': (225, 236, 0), 'V': (237, 248, 0),
    'W': (0, 18, 18), 'X': (19, 30, 18), 'Y': (31, 42, 18), 'Z': (43, 53, 18),
}
GLYPH_H = 18


def texdump(tmp, frame, base, pal, rows, name, inp=None):
    path = os.path.join(tmp, name)
    env = dict(os.environ, CUSA_TEXDUMP="%d,%X,%X,%d,%s" % (frame, base, pal, rows, path))
    if inp:
        env["CUSA_INPUT"] = inp
    subprocess.run([BIN, GEN, str(frame + 1), "0"], env=env, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    d = open(path, "rb").read().split(b"\n", 3)
    w, h = map(int, d[1].split())
    px = d[3]
    img = []
    for y in range(h):
        row = []
        for x in range(w):
            r, g, b = px[(y * w + x) * 3:(y * w + x) * 3 + 3]
            row.append(None if (r, g, b) == (255, 0, 255) else (r, g, b))
        img.append(row)
    return img


def argb1555(p):
    if p is None:
        return 0
    r, g, b = p
    return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        logo = texdump(tmp, 1000, 0x22FC, 0x3D, 256, "logo.ppm")
        font = texdump(tmp, 1440, 0x305B, 0x59, 40, "font.ppm", INPUT)
    # logo: columnas 1..254, filas 0..191 (el resto de la pagina es otra cosa)
    out = bytearray()
    for y in range(256):
        for x in range(256):
            p = logo[y][x] if y < 192 and 1 <= x <= 254 else None
            out += struct.pack("<H", argb1555(p))
    # texto: "NOW LOADING" al doble de tamano, centrado en 512x64
    text = "NOW LOADING"
    glyphs = []
    for ch in text:
        if ch == ' ':
            glyphs.append(None)
        else:
            glyphs.append(FONT18[ch])
    width = sum(8 if g is None else g[1] - g[0] + 2 for g in glyphs) * 2
    img = [[None] * 512 for _ in range(64)]
    x0 = (512 - width) // 2
    for g in glyphs:
        if g is None:
            x0 += 16
            continue
        gx0, gx1, gy = g
        for y in range(GLYPH_H):
            for x in range(gx0, gx1 + 1):
                p = font[gy + y][x]
                if p is None:
                    continue
                for dy in range(2):
                    for dx in range(2):
                        img[14 + y * 2 + dy][x0 + (x - gx0) * 2 + dx] = p
        x0 += (gx1 - gx0 + 2) * 2
    for y in range(64):
        for x in range(512):
            out += struct.pack("<H", argb1555(img[y][x]))
    open(os.path.join(GEN, "loading.bin"), "wb").write(out)
    print("loading.bin: %d bytes" % len(out))


main()
