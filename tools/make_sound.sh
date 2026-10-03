#!/bin/sh
# Genera generated/sound.bin grabando la placa de sonido DCS con MAME.
#
# Uso: tools/make_sound.sh <directorio de ROMs de MAME>
#   El directorio debe contener el romset crusnusa41 (o crusnusa) que MAME
#   reconozca. Hace falta la fuente original en src_orig/ (tools/fetch_source.sh)
#   para saber que codigos son musica.
#
# Son dos pasadas de MAME sin limite de velocidad (unos 2 y 10 minutos en un
# ordenador actual). Los WAV intermedios quedan en generated/sound_work/.
set -e

ROMPATH=${1:?uso: $0 <directorio de ROMs de MAME>}
GAME=${GAME:-crusnusa41}
TOP=$(cd "$(dirname "$0")/.." && pwd)
WORK=$TOP/generated/sound_work
LUA=$TOP/tools/mame/render_dcs.lua
mkdir -p "$WORK"

render() {
    # $1 lista de codigos, $2 nombre de salida
    DCS_CODES="$1" mame "$GAME" -rompath "$ROMPATH" -cfg_directory "$WORK/cfg" \
        -nvram_directory "$WORK/nvram" -video none -nothrottle \
        -wavwrite "$WORK/$2.wav" -autoboot_script "$LUA" > "$WORK/$2.log" 2>&1
}

python3 "$TOP/tools/dcs_bank.py" --plan1 > "$WORK/pasada1.txt"
echo "pasada 1: todos los codigos..."
render "$WORK/pasada1.txt" pasada1

python3 "$TOP/tools/dcs_bank.py" --plan2 --sndtab "$TOP/src_orig" \
    "$WORK/pasada1.wav" "$WORK/pasada1.log" > "$WORK/pasada2.txt"
echo "pasada 2: musica, bucles y motor ($(wc -l < "$WORK/pasada2.txt") entradas)..."
render "$WORK/pasada2.txt" pasada2

python3 "$TOP/tools/dcs_bank.py" -o "$TOP/generated/sound.bin" --sndtab "$TOP/src_orig" \
    --budget 1792000 "$WORK/pasada1.wav" "$WORK/pasada1.log" "$WORK/pasada2.wav" "$WORK/pasada2.log"
