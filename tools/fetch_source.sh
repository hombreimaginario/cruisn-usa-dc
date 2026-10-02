#!/bin/sh
# Descarga la fuente original del arcade en src_orig/ (no se versiona aqui).
set -e
cd "$(dirname "$0")/.."
if [ -d src_orig/.git ]; then
    git -C src_orig pull --ff-only
else
    git clone --depth 1 https://github.com/commercial-game-sources/cruisn_usa src_orig
fi
