#!/bin/sh
# Génère docs/manuel/n2kdran-manuel.pdf à partir de manuel.html.
# Usage : sh build.sh   (depuis docs/manuel)
set -e
cd "$(dirname "$0")"
sh annoter.sh
chromium --headless --disable-gpu --no-pdf-header-footer \
    --print-to-pdf="$PWD/n2kdran-manuel.pdf" "file://$PWD/manuel.html" 2>/dev/null
echo "PDF : $PWD/n2kdran-manuel.pdf"
