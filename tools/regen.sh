#!/bin/sh
# Régénère l'icône (design actuel de la rose, docs/icone/icone.py), recompile l'app
# et l'installe sur l'appareil adb connecté (ou celui de $ANDROID_SERIAL).
# Usage, depuis la racine du dépôt :  sh tools/regen.sh
set -e
cd "$(dirname "$0")/.."
R=app/src/main/res
I=docs/icone
python3 "$I/icone.py"
inkscape "$I/icone.svg" --export-type=png --export-filename="$I/n2kdran-rose-1080.png" -w 1080 -h 1080 >/dev/null 2>&1
for d in "mdpi 108" "hdpi 162" "xhdpi 216" "xxhdpi 324" "xxxhdpi 432"; do
    set -- $d
    s=$(( $2 * 67 / 108 ))                       # rose dans la zone sûre (66 dp) du cadre adaptatif
    convert "$I/n2kdran-rose-1080.png" -resize ${s}x${s} -background none -gravity center \
            -extent ${2}x${2} "$R/mipmap-$1/ic_launcher_fg.png"
done
./gradlew assembleDebug --console=plain -q
ADB="$HOME/Android/Sdk/platform-tools/adb"
"$ADB" install -r app/build/outputs/apk/debug/app-debug.apk
"$ADB" shell am start -S -n oz.n2kdran/.MainActivity >/dev/null
echo "n2kdran régénérée et relancée."
