# Sonido: placa DCS

## Lo que hace el original

La placa de sonido es una Midway DCS (ADSP-2105 + ROM `u2..u9`). El C31 le habla por:

- `0x9A0000` (SND2): cada escritura envía **un byte** (`0xFF00 | byte`) al DCS. `SENDSND` (`0x9211`) manda los códigos de 16 bits como dos bytes, primero el alto.
- `0x994000` bit 1: línea de reset del DCS (0 = reset). Tras el reset el juego envía un byte suelto que hay que descartar.
- `0x995000` no es sonido: es la placa del volante con motor (fuerza), se ignora.

Comandos observados (registro con `CUSA_SOUNDLOG=1 build/cusa_recomp ...`):

| Secuencia | Significado (deducido) |
|---|---|
| `55AA vv ~vv` | volumen general |
| `55AB+n vv ~vv` | volumen de la pista n (0-3) del DCS; `SNDFX` lo manda justo antes del código |
| `55CC rr vv` | motor del jugador (`PLYR_ENGINE`): rr = revoluciones, vv = volumen. Lo sintetiza el DCS; el tono sube casi en línea recta (f0 ≈ 0.14·rr + 4.4 Hz, medido en MAME) y el volumen es lineal. `0000` no lo para; se apaga con volumen 0 |
| `0000` | parar todo (menos el motor) |
| `03E3`-`03E6` | parar la pista 0-3 (`KILLCHAN0-3`) |
| `00C0`-`00CB` | sonidos del motor/neumáticos, se reenvían cada 2 frames |
| `0085` | moneda |
| `01F4`/`0211`/`0210`/... | música y voces del modo demo |

`SNDTAB.INC` de la fuente lista 179 códigos con su prioridad, canal lógico (CHAN0-3) y tiempo de bloqueo del canal.

El DCS tiene 4 pistas (las `CHAN0-3` de `SNDTAB.INC`): la 0 es la música (emisoras de radio y temas), la 1 y la 2 efectos, la 3 voces. Un código nuevo en una pista corta el anterior, y reenviar la misma música la **reinicia** (comprobado en MAME comparando con una grabación continua).

## Implementación en Dreamcast

Emular el ADSP-2105 en el SH-4 a la vez que el juego no es viable, así que el sonido se **pre-renderiza** con MAME a partir de la ROM del usuario:

1. `tools/mame/render_dcs.lua`: arranca el juego en MAME, desactiva `SENDSND` y envía cada código (o secuencias de palabras, p. ej. `eng90 4 55CC 90FF` para el motor) al DCS por `0x9A0000`, grabando la salida con `-wavwrite`.
2. `tools/dcs_bank.py` (+ `tools/dcsdsp.c` para remuestrear, codificar ADPCM y buscar bucles) genera `generated/sound.bin`:
   - pista 0 → música en ADPCM a 22050 Hz para streaming desde el CD. Si sigue sonando al final de la grabación se busca el periodo exacto del bucle (la salida del DCS es digital y se repite casi muestra a muestra);
   - resto → ADPCM en la RAM del AICA, a la mayor frecuencia que quepa (14 kHz con el presupuesto actual). Los bucles (derrapes, sirenas, multitudes...) se recortan a 2,5 s con fundido cruzado;
   - motor → 4 muestras PCM de 1 s en bucle (60, 90, C0 y E0 revoluciones); en consola se elige la más cercana y se cambia la frecuencia.
   - Los ADPCM en bucle se codifican dos veces el tramo del bucle para que el estado del decodificador al final coincida con el del inicio (sin chasquidos).
3. `src/platform/sound_dc.c` decodifica el protocolo que escribe el juego (gancho `vu_sound_hook`) y reproduce: un canal del AICA por pista, uno para el motor y un stream para la música, que lee del CD un hilo aparte en bloques de 64 KB.

Todo el proceso lo hace `tools/make_sound.sh <dir de ROMs de MAME>`:

1. primera pasada: los 209 códigos que usa el juego (`tools/dcs_codes.txt`: los de las tablas de la fuente más variantes vistas en ejecución) durante 6 s cada uno. Se probaron los 1004 códigos posibles y el DCS tiene más sonidos, pero el juego no los pide;
2. `dcs_bank.py --plan2` elige los que siguen sonando a los 6 s (música 400 s, bucles 30 s) y añade el motor a 60, 90, C0 y E0 revoluciones;
3. segunda pasada con esa lista y `dcs_bank.py -o generated/sound.bin` con las dos grabaciones.

Hace falta MAME (probado con 0.288) con el romset `crusnusa41` y la fuente original en `src_orig/` (para saber qué códigos son música).

Nada de este audio generado se sube al repositorio.

Pendiente: comprobar de oído en consola real (en Flycast se ve que decodifica y que el stream se alimenta), y la música `0002` (Munster surf) y `0171` no muestran un bucle exacto en 400 s: se repiten enteras.
