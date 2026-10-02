# Cruis'n USA DC

Port en desarrollo del arcade **Cruis'n USA** (Midway, 1994) a **Sega Dreamcast** usando KallistiOS.

El juego original corre en un TMS320C31 sobre la placa Midway V-Unit. Este port traduce el código de la ROM a C (recompilación estática) y sustituye el hardware de vídeo y sonido por implementaciones nativas para el PowerVR y el AICA. Detalles en [docs/ANALISIS.md](docs/ANALISIS.md) y [docs/PLAN.md](docs/PLAN.md).

**Estado:** fase 0 (herramientas y esqueleto). Todavía no es jugable.

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

# 4. Pruebas en el ordenador (sin Dreamcast)
make -f Makefile.host test

# 5. Compilar para Dreamcast
source $KOS_BASE/environ.sh
make
```

Para probar en consola o emulador, pon `program.bin` en la raíz del CD (`/cd/program.bin`) o sírvelo con dcload (`/pc/program.bin`).

## Estructura

```
tools/romtool.py      extracción y entrelazado de ROM
tools/c31dis.py       desensamblador TMS320C3x
tools/fetch_source.sh descarga la fuente original en src_orig/
src/c3x/              estado de CPU y coma flotante del C31
src/vunit/            mapa de memoria y hardware del V-Unit (HLE)
src/platform/         código específico de Dreamcast
tests/                pruebas de host
docs/                 análisis y plan
```

## Aviso legal

Cruis'n USA es propiedad de sus titulares. Este proyecto no distribuye ningún material con copyright; necesitas tu propia copia del juego.
