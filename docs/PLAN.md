# Plan del port

## Enfoque

**Recompilación estática + emulación de alto nivel (HLE) del hardware.**

- El código del juego se traduce de la imagen de programa de la ROM a C (no a mano), así la lógica, física y IA son idénticas al arcade. La fuente original se usa para nombrar funciones y entender el código.
- El hardware del V-Unit no se emula a nivel de ciclo: las escrituras a la FIFO de polígonos se convierten directamente en polígonos del PowerVR, la paleta y las texturas se convierten a formatos nativos y el sonido se sustituye por audio pre-renderizado.
- Ningún dato con copyright va al repositorio: el usuario genera los ficheros a partir de su ROM con las herramientas de `tools/` y los mete en el CD.

Alternativas descartadas:
- *Reescribir a mano 76k líneas de ensamblador en C*: años de trabajo y riesgo alto de diferencias de comportamiento.
- *Intérprete del C31 en el SH-4*: más sencillo, pero un intérprete cuesta 20-50 ciclos por instrucción y el C31 hace toda la geometría 3D; no llegaría a 57 fps.

## Fases

### Fase 0: Base (hecho)
- [x] `tools/romtool.py`: entrelazado de las ROM de programa y extracción de gráficos y sonido.
- [x] `tools/c31dis.py`: desensamblador TMS320C3x validado contra `DIRQ.ASM` (incluidas instrucciones paralelas).
- [x] `src/c3x/c3x.h`: conversión de flotantes C3x <-> IEEE con pruebas en host.
- [x] `src/vunit/mem.*`: mapa de memoria del V-Unit.
- [x] Esqueleto KallistiOS y CI (pruebas de host + compilación con el SDK de Dreamcast).

### Fase 1: Recompilador (`tools/c31recomp.py`)
- Descubrimiento de código a partir de los vectores, `CALL`, saltos y tablas de punteros; símbolos tomados de la fuente (etiquetas de `DIRQ.ASM`, `MPROC.ASM`...).
- Emisión de C por bloque básico sobre `c3x_state`, con:
  - saltos retardados (las 3 instrucciones siguientes se ejecutan antes del salto),
  - `RPTB`/`RPTS` como bucles de C,
  - flags de `ST` calculados solo cuando una instrucción posterior los lee,
  - `DP` y direccionamiento indirecto con módulo y bit-reverso.
- Saltos indirectos (`B Rn`, `RETS`, despachador de procesos de `MPROC`) mediante una tabla dirección -> bloque. La pila del C31 se mantiene en la RAM emulada para que `SLEEP`/despertar funcionen sin cambios.
- Prueba de referencia: ejecutar el mismo código recompilado en el host (macOS/Linux) y compararlo con un intérprete sencillo del C31 instrucción a instrucción.

### Fase 2: Arranque en host
- Ejecutar el juego recompilado en el host con un volcado de la FIFO de polígonos a imagen (SDL u otro) para depurar sin hardware Dreamcast.
- Implementar temporizadores, interrupciones (`DIRQ` por vblank), entradas, CMOS y paleta.

### Fase 3: Vídeo en Dreamcast
- Traducción de la FIFO de polígonos a listas del PVR (opacos y transparentes), con Z.
- Conversión de texturas de `WAVERAM` (8 bits + paleta) a texturas PVR con caché por página y paleta.
- Resolución 640x480 escalando desde 512x400.

### Fase 4: Sonido
- Herramienta en PC que emule la placa DCS (ADSP-2105 + ROM `u2..u9`) y grabe cada comando de efecto/música a ADPCM.
- Reproductor en el AICA que reciba los mismos comandos que el juego escribe en `SOUND`.

### Fase 5: Controles, guardado y empaquetado
- Mando estándar y Race Controller; cambio de marchas en botones.
- CMOS en la VMU.
- Script para generar la imagen de CD (`.cdi`) a partir de la ROM del usuario.

## Riesgos conocidos

- Precisión: los registros del C31 son de 40 bits y el SH-4 trabaja con `float` de 32 bits; puede haber pequeñas diferencias en física. Se medirá con la comparación contra el intérprete.
- Memoria: 16 MB de RAM principal. La imagen de programa ocupa 2 MB, la ROM gráfica 8 MB; habrá que cargar texturas desde el CD por circuito en vez de tenerlas todas en RAM.
- Rendimiento: el C31 a 50 MHz con instrucciones paralelas frente al SH-4 a 200 MHz. Si la transformación de `DIRQ` no llega, se sustituirá esa rutina concreta por una versión nativa que use las instrucciones vectoriales del SH-4 (`FTRV`).
