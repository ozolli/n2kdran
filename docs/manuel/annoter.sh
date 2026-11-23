#!/bin/sh
# Ajoute les repères numérotés sur la capture de l'écran principal.
# Usage : sh annoter.sh   (depuis docs/manuel)
set -e
in=img/principal-sans-sail.png
out=img/principal-annote.png
draw=""
# n x y
for p in "1 705 22" "2 583 118" "3 705 322" "4 280 470" "5 880 122" "6 722 150" \
         "7 442 222" "8 690 235" "9 900 298" "10 708 400" "11 515 580" "12 365 762" "13 846 776" \
         "14 338 42" "15 905 28"; do
    set -- $p
    draw="$draw circle $2,$3 $(($2 + 19)),$3"
done
convert "$in" -fill '#FFD400' -stroke black -strokewidth 3 -draw "$draw" /tmp/n2kdran-annote.png
args=""
for p in "1 705 22" "2 583 118" "3 705 322" "4 280 470" "5 880 122" "6 722 150" \
         "7 442 222" "8 690 235" "9 900 298" "10 708 400" "11 515 580" "12 365 762" "13 846 776" \
         "14 338 42" "15 905 28"; do
    set -- $p
    args="$args -annotate +$(($2 - (${#1} * 7)))+$(($3 + 8)) $1"
done
convert /tmp/n2kdran-annote.png -font DejaVu-Sans-Bold -pointsize 24 -fill black -stroke none $args "$out"
rm -f /tmp/n2kdran-annote.png
