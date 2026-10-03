#!/usr/bin/env python3
"""Construye el banco de sonido de Dreamcast (/cd/sound.bin) a partir de
grabaciones de la placa DCS hechas con tools/mame/render_dcs.lua.

Uso:
    dcs_bank.py --plan1 > pasada1.txt
        los codigos que usa el juego (tools/dcs_codes.txt), 6 s cada uno
    dcs_bank.py --plan2 --sndtab src_orig pasada1.wav pasada1.log > pasada2.txt
        segunda pasada: los que siguen sonando a los 6 s (musica 400 s,
        bucles 30 s) y el motor a varias revoluciones
    dcs_bank.py -o generated/sound.bin --sndtab src_orig \\
        grabacion1.wav grabacion1.log [grabacion2.wav grabacion2.log ...]

Las grabaciones posteriores sustituyen a las anteriores para el mismo codigo
(por ejemplo, una pasada de 12 s por codigo y otra de 200 s para la musica y
los bucles). Las entradas con nombre "engXX" son el motor a XX revoluciones.

Que se hace con cada codigo:
  - pista 0 del DCS (musica): ADPCM a 22050 Hz para leer del CD en streaming;
    si sigue sonando al final se busca el periodo exacto del bucle;
  - resto (efectos y voces): ADPCM en la RAM del AICA a la mayor frecuencia
    que quepa en el presupuesto; los que no terminan se recortan a un bucle
    corto con fundido cruzado;
  - motor: PCM de 16 bits en bucle de 1 s.

La pista de cada codigo sale de SNDTAB.INC (fuente original). Todo lo que
genera deriva de la ROM: no se versiona.
"""
import argparse
import array
import os
import re
import struct
import subprocess
import sys
import tempfile
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
SILENCE = 120
MUSIC_RATE = 22050
ENGINE_RATE = 22050
RATES = (22050, 19200, 17000, 16000, 15000, 14000, 13000, 12000, 11025, 10000)
SF_LOOP, SF_PCM16, SF_MUSIC, SF_ENGINE = 1, 2, 4, 8
MAX_SFX_SAMPLES = 65534
TMP = tempfile.mkdtemp(prefix="dcsbank")
DSP = os.path.join(TMP, "dcsdsp")


def build_dsp():
    subprocess.check_call(["cc", "-O2", "-o", DSP, os.path.join(HERE, "dcsdsp.c"), "-lm"])


def dsp(*args):
    return subprocess.run([DSP] + [str(a) for a in args], check=True,
                          capture_output=True, text=True).stdout


def write_raw(name, samples):
    p = os.path.join(TMP, name)
    with open(p, "wb") as f:
        f.write(samples.tobytes())
    return p


def read_raw(p, typecode="h"):
    a = array.array(typecode)
    with open(p, "rb") as f:
        a.frombytes(f.read())
    return a


GAIN = 1.0


def amp(samples):
    """Ganancia global: MAME saca el DCS a unos -13 dB de pico; se sube todo
    el banco por igual (mantiene la mezcla) para aprovechar los 16 bits."""
    if GAIN == 1.0:
        return samples
    return array.array("h", (max(-32768, min(32767, int(x * GAIN))) for x in samples))


def resample(samples, src, dst):
    if src == dst:
        return samples
    i = write_raw("rs_in.raw", samples)
    o = os.path.join(TMP, "rs_out.raw")
    dsp("resample", i, src, o, dst)
    return read_raw(o)


def adpcm(samples, loop=None):
    i = write_raw("ad_in.raw", samples)
    o = os.path.join(TMP, "ad_out.adp")
    if loop is None:
        dsp("adpcm", i, o)
    else:
        dsp("adpcm", i, o, loop)
    with open(o, "rb") as f:
        return f.read()


def load_recording(wav, log):
    """Devuelve {codigo: muestras} con la ventana de cada codigo."""
    with open(wav, "rb") as f:
        raw = f.read()
    try:
        w = wave.open(wav)
        rate, ch = w.getframerate(), w.getnchannels()
        data = raw[44:]
    except wave.Error:                   # grabacion sin cerrar: cabecera vacia
        rate, ch, data = 48000, 1, raw[44:]
    data = data[:len(data) // 2 * 2]
    d = array.array("h", data)
    if ch == 2:
        d = d[::2]
    marks = []
    for line in open(log, errors="ignore"):
        p = line.split()
        if len(p) >= 3 and p[0] == "CODE":
            marks.append((p[1], float(p[2])))
        elif len(p) >= 2 and p[0] == "END":
            marks.append((None, float(p[1])))
    out = {}
    for k, (code, t0) in enumerate(marks):
        if code is None:
            continue
        t1 = marks[k + 1][1] - 0.21 if k + 1 < len(marks) else len(d) / rate
        seg = d[int(t0 * rate):int(t1 * rate)]
        if len(seg) < rate // 2:
            continue
        key = int(code) if code.isdigit() else code
        out[key] = (seg, rate)
    return out


def sndtab_tracks(srcdir):
    """codigo DCS -> pista (CHANn) segun SNDTAB.INC"""
    tracks = {}
    if not srcdir:
        return tracks
    words = []
    for line in open(os.path.join(srcdir, "SNDTAB.INC"), errors="ignore"):
        line = line.split(";")[0].strip()
        if line.lower().startswith(".word"):
            words.append(line.split(None, 1)[1].replace(" ", ""))
    for i in range(0, len(words) - 2, 3):
        m = re.search(r"CHAN(\d)", words[i], re.I)
        m2 = re.search(r"\|(\w+)$", words[i + 1])
        if not m or not m2:
            continue
        v = m2.group(1)
        code = int(v[:-1], 16) if v.lower().endswith("h") else int(v)
        tracks.setdefault(code, int(m.group(1)))
    return tracks


def game_codes():
    """Codigos que usa el juego (tools/dcs_codes.txt): los de las tablas de
    sonido de la fuente mas los observados en ejecucion."""
    codes = set()
    for line in open(os.path.join(HERE, "dcs_codes.txt")):
        line = line.split("#")[0].strip()
        if line:
            codes.add(int(line))
    return codes


def last_sound(seg):
    last = 0
    for i in range(len(seg) - 1, -1, -64):
        if abs(seg[i]) > SILENCE:
            last = i
            break
    return last


def still_playing(seg, rate):
    tail = seg[-rate // 4:]
    return any(abs(v) > SILENCE for v in tail)


def find_loop(seg, rate):
    """Periodo del bucle en muestras (o None). Se busca el menor retardo con
    error casi nulo comparando una ventana de 5 s a partir de los 15 s."""
    if len(seg) < rate * 40:
        return None, None
    p = write_raw("fl.raw", seg)
    best = None
    for line in dsp("findloop", p, rate, 15, 5, 8).splitlines():
        lag, err = line.split()
        lag, err = int(lag), float(err)
        if err < 3e-4 and (best is None or lag < best):
            best = lag
    if best is None:
        return None, None
    start = 15 * rate
    if start + best > len(seg):
        return None, None
    return start, best


def crossfade_loop(seg, rate, intro_s, body_s, fade_s):
    """Recorta seg a intro + cuerpo en bucle con fundido cruzado al final.
    Devuelve (muestras, inicio_bucle)."""
    s = int(intro_s * rate)
    e = s + int(body_s * rate)
    f = int(fade_s * rate)
    if e > len(seg) or s < f:
        return seg, None
    out = array.array("h", seg[:e])
    for i in range(f):
        a = (i + 1) / (f + 1)
        x = out[e - f + i] * (1 - a) + seg[s - f + i] * a
        out[e - f + i] = int(max(-32768, min(32767, x)))
    return out, s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--output")
    ap.add_argument("--plan1", action="store_true")
    ap.add_argument("--plan2", action="store_true")
    ap.add_argument("--sndtab", help="directorio con SNDTAB.INC (fuente original)")
    ap.add_argument("--budget", type=int, default=1500 * 1024,
                    help="bytes de RAM del AICA para efectos")
    ap.add_argument("recordings", nargs="*")
    a = ap.parse_args() if "--plan1" not in sys.argv else None
    if a is None:
        for c in sorted(game_codes()):
            print(c, 6)
        return
    if len(a.recordings) % 2:
        sys.exit("hacen falta pares wav log")
    tracks = sndtab_tracks(a.sndtab)

    sounds = {}
    for i in range(0, len(a.recordings), 2):
        sounds.update(load_recording(a.recordings[i], a.recordings[i + 1]))

    if a.plan2:
        for code, (seg, rate) in sorted(sounds.items(), key=lambda x: str(x[0])):
            if isinstance(code, int) and last_sound(seg) and still_playing(seg, rate):
                print(code, 400 if tracks.get(code, 1) == 0 else 30)
        for rpm in (0x60, 0x90, 0xC0, 0xE0):
            print("eng%02X 4 55CC %02XFF" % (rpm, rpm))
        return
    if not a.output:
        sys.exit("falta -o")
    build_dsp()

    allowed = game_codes()
    music, sfx, engine = [], [], []
    for code, (seg, rate) in sorted(sounds.items(), key=lambda x: str(x[0])):
        if isinstance(code, str):
            m = re.match(r"eng([0-9A-Fa-f]+)$", code)
            if m:
                engine.append((int(m.group(1), 16), seg, rate))
            continue
        if code >= 0x400 or (allowed is not None and code not in allowed):
            continue
        if last_sound(seg) == 0:
            continue
        track = tracks.get(code, 1)
        (music if track == 0 else sfx).append((code, track, seg, rate))

    global GAIN
    peak = 1
    for _, _, seg, _ in music + sfx:
        peak = max(peak, max(abs(x) for x in seg[::3]))
    for _, seg, _ in engine:
        peak = max(peak, max(abs(x) for x in seg[::3]))
    GAIN = min(4.0, 29000.0 / peak)
    print("pico %d, ganancia %.2f" % (peak, GAIN))

    entries, blobs = [], []

    # ---- musica ----
    for code, track, seg, rate in music:
        flags = SF_MUSIC
        loop = None
        if still_playing(seg, rate):
            start, lag = find_loop(seg, rate)
            if lag:
                seg = seg[:start + lag]
                loop = start
                flags |= SF_LOOP
            else:
                print("musica %04X: no se encontro el bucle, se repite entera" % code)
                flags |= SF_LOOP
                loop = 0
        else:
            seg = seg[:last_sound(seg) + rate // 50]
        r = resample(amp(seg), rate, MUSIC_RATE)
        lp = None if loop is None else (int(loop * MUSIC_RATE / rate) & ~1)
        data = adpcm(r, lp)
        entries.append(dict(code=code, track=0, flags=flags, rate=MUSIC_RATE,
                            loopstart=lp or 0, loopend=0, param=0))
        blobs.append(data)
        print("musica %04X: %.1f s%s" % (code, len(r) / MUSIC_RATE,
              "" if lp is None else ", bucle desde %.1f s" % (lp / MUSIC_RATE)))

    # ---- motor ----
    eng_bytes = 0
    for speed, seg, rate in engine:
        body, s = crossfade_loop(seg[rate:], rate, 0.3, 1.0, 0.2)
        body = resample(amp(body), rate, ENGINE_RATE)
        s = int(s * ENGINE_RATE / rate) if s else 0
        body = body[s:]                  # solo el bucle; el fundido ya enlaza
        data = body.tobytes()
        data += b"\0" * (-len(data) % 32)
        entries.append(dict(code=0, track=0, flags=SF_ENGINE | SF_LOOP | SF_PCM16,
                            rate=ENGINE_RATE, loopstart=0, loopend=len(body), param=speed))
        blobs.append(data)
        eng_bytes += len(data)

    # ---- efectos: preparar recortes a la frecuencia original ----
    prepared = []
    for code, track, seg, rate in sfx:
        loop = None
        if still_playing(seg, rate):
            seg, loop = crossfade_loop(seg, rate, 0.5, 2.5, 0.4)
        else:
            seg = seg[:last_sound(seg) + rate // 50]
        prepared.append((code, track, seg, rate, loop))

    def size_at(r):
        total = 0
        for code, track, seg, rate, loop in prepared:
            n = min(int(len(seg) * r / rate), MAX_SFX_SAMPLES)
            total += (n // 2 + 31) & ~31
        return total

    budget = a.budget - eng_bytes
    sfx_rate = RATES[-1]
    for r in RATES:
        if size_at(r) <= budget:
            sfx_rate = r
            break
    print("efectos: %d a %d Hz, %d bytes (motor %d bytes)" %
          (len(prepared), sfx_rate, size_at(sfx_rate), eng_bytes))

    for code, track, seg, rate, loop in prepared:
        r = sfx_rate
        if len(seg) * r / rate > MAX_SFX_SAMPLES:
            r = int(MAX_SFX_SAMPLES * rate / len(seg)) - 1
        x = resample(amp(seg), rate, r)[:MAX_SFX_SAMPLES - 64]
        lp = None if loop is None else (int(loop * r / rate) & ~3)
        data = adpcm(x, lp)
        data += b"\0" * (-len(data) % 32)
        flags = SF_LOOP if lp is not None else 0
        entries.append(dict(code=code, track=track, flags=flags, rate=r,
                            loopstart=lp or 0, loopend=len(x) & ~3 if lp is not None else 0,
                            param=0))
        blobs.append(data)

    # ---- fichero ----
    hdr_size = 16 + 32 * len(entries)
    off = (hdr_size + 2047) & ~2047
    body = b""
    for e, blob in zip(entries, blobs):
        e["offset"] = off + len(body)
        e["bytes"] = len(blob)
        body += blob
        body += b"\0" * (-len(body) % 32)
    with open(a.output, "wb") as f:
        f.write(b"CUSASND1" + struct.pack("<II", len(entries), 0))
        for e in entries:
            f.write(struct.pack("<HBBIIIIIHHI", e["code"], e["track"], e["flags"], e["rate"],
                                e["offset"], e["bytes"], e["loopstart"], e["loopend"],
                                e["param"], 0, 0))
        f.write(b"\0" * (off - hdr_size))
        f.write(body)
    print("%s: %d entradas, %.1f MB" % (a.output, len(entries), os.path.getsize(a.output) / 1e6))


if __name__ == "__main__":
    main()
