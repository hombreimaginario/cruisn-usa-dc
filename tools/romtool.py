#!/usr/bin/env python3
"""Extrae y entrelaza las ROM de programa de Cruis'n USA (Midway V-Unit).

Las ROM de programa son 4 chips de 8 bits (u10..u13) que forman palabras de
32 bits little-endian del TMS320C31: u10 = bits 7-0, u11 = 15-8, u12 = 23-16,
u13 = 31-24. Al arrancar, el hardware copia las primeras 0x20000 palabras a la
FASTRAM (direccion 0), de modo que la palabra 0 es el vector de reset.

Uso:
    romtool.py <crusnusa.zip | zip que lo contiene | directorio> <salida_dir> [--version l41]

Genera:
    program.bin   imagen de programa entrelazada (palabras LE de 32 bits)
    gfx.bin       ROM de graficos u14..u29: 4 bancos de 4 chips entrelazados en
                  palabras LE de 32 bits; el C31 la ve en 0xD00000
    sound.bin     ROM de sonido DCS u2..u9 concatenadas (raw)

Ningun fichero generado debe subirse al repositorio (ver .gitignore).
"""
import argparse
import io
import os
import struct
import sys
import zipfile

ROM_SIZE = 0x80000
VERSIONS = ("l41", "l4", "l21")


class RomSet:
    def __init__(self, files):
        self.files = files  # nombre base -> bytes

    @classmethod
    def load(cls, path):
        files = {}
        if os.path.isdir(path):
            for root, _, names in os.walk(path):
                for n in names:
                    with open(os.path.join(root, n), "rb") as f:
                        files[n.lower()] = f.read()
        else:
            cls._load_zip(zipfile.ZipFile(path), files)
        return cls(files)

    @classmethod
    def _load_zip(cls, z, files):
        for info in z.infolist():
            if info.is_dir():
                continue
            data = z.read(info)
            name = os.path.basename(info.filename).lower()
            if name.endswith(".zip"):
                cls._load_zip(zipfile.ZipFile(io.BytesIO(data)), files)
            else:
                files[name] = data

    def get(self, name):
        if name not in self.files:
            sys.exit("falta la ROM %s" % name)
        data = self.files[name]
        if len(data) != ROM_SIZE:
            sys.exit("%s mide %d bytes, se esperaban %d" % (name, len(data), ROM_SIZE))
        return data


def interleave32(chips):
    out = bytearray(4 * len(chips[0]))
    for i, c in enumerate(chips):
        out[i::4] = c
    return bytes(out)


def build_program(roms, version):
    chips = [roms.get("cusa-%s.u%d" % (version, n)) for n in (10, 11, 12, 13)]
    return interleave32(chips)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("romset")
    ap.add_argument("outdir")
    ap.add_argument("--version", default="l41", choices=VERSIONS)
    a = ap.parse_args()

    roms = RomSet.load(a.romset)
    os.makedirs(a.outdir, exist_ok=True)

    prog = build_program(roms, a.version)
    reset = struct.unpack_from("<I", prog, 0)[0]
    if b"CRUISN USA" not in prog:
        sys.exit("la imagen de programa no contiene la cadena esperada; ROM incorrecta?")
    with open(os.path.join(a.outdir, "program.bin"), "wb") as f:
        f.write(prog)

    gfx = b"".join(interleave32([roms.get("cusa.u%d" % n) for n in range(b, b + 4)])
                   for b in (14, 18, 22, 26))
    with open(os.path.join(a.outdir, "gfx.bin"), "wb") as f:
        f.write(gfx)

    snd = b"".join(roms.get("cusa.u%d" % n) for n in range(2, 10))
    with open(os.path.join(a.outdir, "sound.bin"), "wb") as f:
        f.write(snd)

    print("version %s: programa %d palabras, vector de reset 0x%06X" % (a.version, len(prog) // 4, reset))
    print("graficos %d bytes, sonido %d bytes -> %s" % (len(gfx), len(snd), a.outdir))


if __name__ == "__main__":
    main()
