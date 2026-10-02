# Análisis técnico: Cruis'n USA (arcade, Midway V-Unit)

## Hardware original

| Componente | Detalle | Fuente |
|---|---|---|
| CPU principal | TI TMS320C31 (DSP de 32 bits, coma flotante propia, ~50 MHz) | `C30.EQU`, `MAKEFILE` (`asm30`, `C_OPTIONS`) |
| Vídeo | Motor de polígonos por DMA alimentado por una FIFO en `0x600000`; registros en `0x980000` | `VUNIT.EQU` |
| Framebuffer | 512x1024 palabras, dos páginas de 512x512 (`SCREEN0`/`SCREEN1`), color indexado: paleta en bits 15-8 y color en 7-0 | `VUNIT.EQU` |
| Paleta | `COLORAM` en `0x9E0000`, 32K entradas RGB | `VUNIT.EQU` |
| Texturas | `WAVERAM` en `0xA00000`, cargada desde las ROM gráficas (`DOWNLOADING TEXTURES` al arrancar) | `VUNIT.EQU`, `CUSA.ASM` |
| Sonido | Placa DCS (ADSP-2105) controlada por el puerto `SOUND` (`0x995000`) | `SND.ASM`, `SNDTAB.EQU` |
| Persistencia | CMOS en `0x9C0000` (8 bits útiles por palabra) | `CMOS.EQU` |

Todas las direcciones son de palabra de 32 bits, como las ve el C31.

## ROM

El zip contiene `crusnusa.zip` con tres revisiones del programa (`l21`, `l4`, `l41`) y las ROM comunes:

- **Programa**: `cusa-<ver>.u10..u13`, 4 x 512 KB. Se entrelazan por bytes en palabras little-endian de 32 bits (u10 = bits 7-0 ... u13 = bits 31-24). Resultado: 2 MB = 0x80000 palabras.
- **Gráficos**: `cusa.u14..u29`, 16 x 512 KB = 8 MB.
- **Sonido DCS**: `cusa.u2..u9`, 8 x 512 KB = 4 MB.

Comprobado con `tools/romtool.py` sobre la revisión L4.1:

- La palabra 0 es el vector de reset (`0x004AD5`) y 0x01-0x3F los vectores de interrupción/trap, igual que la sección `VECTOR` de `CUSA.CMD`.
- A partir de 0x40 aparecen exactamente las constantes del principio de `DIRQ.ASM` (`OACTIVEI`, `IDLE_LISTI`...), primer módulo enlazado.
- Las rutinas desensambladas (`DIRQ`, `NEXTOBJ`, `UNIV_ROT`...) coinciden instrucción a instrucción con la fuente, incluidas las instrucciones paralelas `MPYF3 || ADDF3`.

Conclusión: **la fuente corresponde a la revisión L4.x** (la versión "Head 2 Head" enlazable) y la imagen de la ROM es la salida del enlazador colocada a partir de la dirección 0 (FASTRAM). Al arrancar el hardware copia las primeras 0x20000 palabras de ROM a FASTRAM.

## Fuente original

- 76.415 líneas de ensamblador TMS320C3x en 70 módulos, más tablas generadas por herramientas propias (`PCOMP`, ficheros `.GEO/.PTG/.TGA/.POL` de modelos y texturas).
- Prácticamente nada está en C (`HPMATH.C`, `LINE.C`, `T.C` son pruebas o versiones de referencia).
- Los módulos más grandes: `DIAG` (tests de operador), `COLLA` (colisiones), `INTRO`, `PLYR` (física del coche del jugador), `HSTDP` (tabla de récords), `DIRQ` (transformación 3D y envío de polígonos), `BACKGRND`, `MOTION`.
- No se conserva el ensamblador/enlazador de TI ni las herramientas de datos, así que **no se puede reensamblar la fuente tal cual**; la fuente sirve como mapa de símbolos y documentación.

### Sistema de procesos (`MPROC.ASM`)

El juego usa multitarea cooperativa: cada proceso es una estructura con sus registros guardados (`PR4`, `PAR4`...). `SLEEP` saca la dirección de retorno de la pila, la guarda en `PWAKE` y salta al despachador, que reanuda el siguiente proceso saltando a su `PWAKE`. Es decir, hay saltos indirectos a direcciones arbitrarias de código, lo que condiciona el diseño del recompilador (ver `PLAN.md`).

### Render (`DIRQ.ASM`)

La CPU hace toda la geometría: transforma objetos con la matriz de cámara, recorta, proyecta y escribe cada polígono (4 vértices X/Y/Z + coordenadas de textura + palabra de control `TM`/`CC`/`DITHER`...) en la FIFO. El hardware solo rasteriza. Esto encaja bien con el PowerVR de Dreamcast: basta con interceptar las escrituras a la FIFO y convertirlas en polígonos PVR.

## Retos para Dreamcast

1. **CPU**: el C31 tiene un formato de coma flotante propio, registros de 40 bits, saltos retardados (3 instrucciones), bucles hardware (`RPTB`/`RPTS`) e instrucciones paralelas. Hay que traducirlo a C para el SH-4 (200 MHz). La conversión de flotantes ya está implementada y probada en `src/c3x/c3x.h`.
2. **Texturas**: 8 MB de ROM gráfica con paletas de 32K colores frente a 8 MB de VRAM en Dreamcast. Habrá que convertir y cachear páginas de textura bajo demanda (texturas de 8 bits con bancos de paleta del PVR, o conversión a 16 bits).
3. **Sonido**: la placa DCS ejecuta su propio DSP (ADSP-2105). Emularlo en el SH-4 a la vez que el juego es inviable; la vía realista es pre-renderizar cada comando de sonido/música con una herramienta en el PC y reproducirlo con el AICA (ver `PLAN.md`).
4. **Controles**: volante, pedales y cambio de 4 marchas → stick analógico, gatillos y botones del mando (o volante Race Controller).
5. **CMOS** → guardado en VMU.

Nota: en la carpeta local también hay una ROM de la versión de Nintendo 64. Es un juego distinto (reprogramado para N64) y no se usa en este port; está excluida del repositorio por `.gitignore`.
