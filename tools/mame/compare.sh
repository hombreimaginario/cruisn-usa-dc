#!/bin/sh
# Compara registros en un punto de ruptura entre MAME (referencia) y cusa_host.
# Uso: tools/mame/compare.sh <dir_mame> <dir_generated> <pc_hex> [n_hits]
#   dir_mame: contiene roms/crusnusa41/ y nvram/; MAME debe estar instalado.
set -e
MAMEDIR=$1; GEN=$2; BP=$3; N=${4:-20}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${TMPDIR:-/tmp}/cusa_compare
mkdir -p "$OUT"
[ -f "$GEN/cmos.bin" ] && mkdir -p "$MAMEDIR/nvram/crusnusa41" && cp "$GEN/cmos.bin" "$MAMEDIR/nvram/crusnusa41/nvram"
(cd "$MAMEDIR" && BP=$BP BPN=$((N + 1)) mame crusnusa41 -rompath roms -cfg_directory cfg \
    -nvram_directory nvram -video none -sound none -nothrottle -str 120 \
    -debug -debugger none -autoboot_script "$HERE/bp.lua" 2>/dev/null) \
    | grep '^pc=' | sed 1d | head -n "$N" > "$OUT/mame.txt"
CUSA_BREAK=$BP CUSA_BREAKN=$N "$ROOT/build/cusa_host" "$GEN" 100000 0 "$OUT" | grep '^pc=' > "$OUT/host.txt"
if diff "$OUT/mame.txt" "$OUT/host.txt" > "$OUT/diff.txt"; then
    echo "iguales en $N paradas"
else
    echo "diferencias (ver $OUT/diff.txt):"
    head -6 "$OUT/diff.txt" | cut -c1-400
fi
