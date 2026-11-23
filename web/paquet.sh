#!/bin/sh
# Paquet autonome de la version web, à transmettre : web/dist/n2kdran-web.zip
# (page, cœur WebAssembly, relais, ressources CM50, service systemd, manuel PDF).
set -e
cd "$(dirname "$0")"
R=..
OUT=dist/n2kdran-web
rm -rf dist
mkdir -p "$OUT"
cp index.html n2kdran.js n2k.js n2k.wasm manifest.json relay.py n2kdran-web.service LISEZMOI.txt HALPI-CLAUDE.md "$OUT/"
cp $R/app/src/main/res/drawable/cm50_outline.png "$OUT/"
cp $R/docs/icone/n2kdran-rose-1080.png "$OUT/icon.png"
cp $R/app/src/main/assets/polars/CM50.pol "$OUT/"
cp $R/app/src/main/assets/sailchart/CM50.saildef $R/app/src/main/assets/sailchart/CM50.sailselect "$OUT/"
cp $R/docs/manuel/n2kdran-manuel.pdf "$OUT/"
(cd dist && zip -qr9 n2kdran-web.zip n2kdran-web)
ls -l dist/n2kdran-web.zip
