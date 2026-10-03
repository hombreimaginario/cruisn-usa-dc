# Cruis'n USA DC

Port en desarrollo del arcade **Cruis'n USA** (Midway, 1994) a **Sega Dreamcast** usando KallistiOS.

El juego original corre en un TMS320C31 sobre la placa Midway V-Unit. Este port traduce el código de la ROM a C (recompilación estática) y sustituye el hardware de vídeo y sonido por implementaciones nativas para el PowerVR y el AICA. Detalles en [docs/ANALISIS.md](docs/ANALISIS.md) y [docs/PLAN.md](docs/PLAN.md).

**Estado:** el código del arcade, recompilado a C, arranca en Dreamcast (probado en Flycast) y dibuja con el PowerVR. En el ordenador es jugable (intérprete y versión recompilada). En consola aún va 1-3 veces más lento que el original según la escena. El sonido se pre-renderiza con MAME a partir de tu ROM (ver [docs/SONIDO.md](docs/SONIDO.md)). Ver [docs/PLAN.md](docs/PLAN.md).

## Lo que necesitas

- Tu propia copia de la ROM del arcade (`crusnusa`, revisión L4.1). Este repositorio **no** incluye ROM, gráficos, sonido ni la fuente original.
- Python 3.
- Para compilar para Dreamcast: KallistiOS con su toolchain (`sh-elf-gcc`).

## Uso

```sh
# 1. Fuente original (solo como referencia, no se versiona)
tools/fetch_source.sh

# 2. Extraer la imagen de programa y los datos de tu ROM
python3 tools/romtool.py ruta/a/crusnusa.zip generated/

# 3. Desensamblar una zona (p. ej. el arranque)
python3 tools/c31dis.py generated/program.bin 4AD5 40

# 4. Pruebas y emulador de referencia en el ordenador (sin Dreamcast)
make -f Makefile.host test
mkdir -p generated/frames
build/cusa_host generated 1500 100 generated/frames   # vuelca un PPM cada 100 frames

# Calibracion inicial de controles (solo la primera vez; guarda generated/cmos.bin)
CUSA_INPUT="3100:10:10,3300:10:10,3500:10:10,3700:10:10,3900:10:10" \
CUSA_ANALOG="3200:10:0:0,3400:f0:0:0,3600:80:f0:0,3800:80:0:f0,4000:80:0:0" \
build/cusa_host generated 4200 0

# 5. Recompilar el codigo del juego a C (usa la cobertura del paso 4)
CUSA_SKIPTESTS=1 CUSA_COVERAGE=generated/coverage.bin build/cusa_host generated 3000 0
python3 tools/c31recomp.py generated/program.bin generated/recomp --coverage generated/coverage.bin
make -f Makefile.host recomp difftest      # cusa_recomp y la prueba diferencial
# Cobertura del resto del modo demo y de una carrera (rapido, con el recompilado);
# lo que falte se interpreta, asi que despues se recompila otra vez
CUSA_COVERAGE=generated/coverage.bin build/cusa_recomp generated 20000 0
python3 tools/c31recomp.py generated/program.bin generated/recomp --coverage generated/coverage.bin

# 6. Sonido (opcional, necesita MAME): graba la placa DCS y crea generated/sound.bin
tools/make_sound.sh ~/ruta/a/roms/de/mame

# 7. Compilar para Dreamcast e imagen de CD
source $KOS_BASE/environ.sh
make && make cdi
```

`make cdi` mete en el CD `program.bin`, `gfx.bin`, `cmos.bin` y, si existe, `sound.bin` (el banco de sonido, ver [docs/SONIDO.md](docs/SONIDO.md)).

## Controles (mando de Dreamcast)

| Mando | Arcade |
|---|---|
| Stick analógico | volante |
| Gatillo R / L | acelerador / freno |
| A / B | subir / bajar marcha (4 marchas) |
| Cruceta izquierda, arriba, derecha | vistas 1, 2, 3 |
| X | cambiar emisora de radio |
| Y | moneda |
| START | empezar |
| START + Y | botón de servicio (menú de ajustes del arcade) |

Los ajustes, récords y contadores del arcade (su CMOS) se guardan solos en la primera VMU (fichero `CRUISNUS`, unos 66 bloques) cuando cambian; si no hay VMU se usa la CMOS calibrada del CD.

## Estructura

```
tools/romtool.py      extracción y entrelazado de ROM
tools/c31dis.py       desensamblador TMS320C3x
tools/fetch_source.sh descarga la fuente original en src_orig/
tools/mame/           comparacion con MAME (referencia) en puntos de ruptura
src/host/             emulador de referencia para el ordenador
src/c3x/              estado de CPU y coma flotante del C31
src/vunit/            mapa de memoria y hardware del V-Unit (HLE)
src/platform/         código específico de Dreamcast
tests/                pruebas de host
docs/                 análisis y plan
```

## Aviso legal

Cruis'n USA es propiedad de sus titulares. Este proyecto no distribuye ningún material con copyright; necesitas tu propia copia del juego.
