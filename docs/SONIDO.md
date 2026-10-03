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
| `55AB/55AC/55AD vv ~vv` + código | volumen de la pista (B, C, D) para el código que sigue |
| `55CC xx yy` | parámetro continuo del motor (xx sube y baja con las revoluciones) |
| `0000` | parar todo |
| `00C0`-`00CB` | sonidos del motor/neumáticos, se reenvían cada 2 frames |
| `0085` | moneda |
| `01F4`/`0211`/`0210`/... | música y voces del modo demo |

`SNDTAB.INC` de la fuente lista 179 códigos con su prioridad, canal lógico (CHAN0-3) y tiempo de bloqueo del canal.

## Plan para Dreamcast

Emular el ADSP-2105 en el SH-4 a la vez que el juego no es viable, así que el sonido se **pre-renderiza** con MAME a partir de la ROM del usuario:

1. `tools/mame/render_dcs.lua`: arranca el juego en MAME, desactiva `SENDSND` y envía cada código al DCS por `0x9A0000`, grabando la salida con `-wavwrite`.
2. Un script en Python trocea el WAV por código, recorta silencios, detecta bucles (música, motor) y convierte a ADPCM de Yamaha (efectos, en la RAM del AICA) o PCM para streaming desde el CD (música).
3. En Dreamcast, un decodificador del protocolo anterior traduce los comandos del juego a reproducción con `snd_sfx_play_ex` (volumen, tono, bucle) y streaming para la música.

Nada de este audio generado se sube al repositorio.
