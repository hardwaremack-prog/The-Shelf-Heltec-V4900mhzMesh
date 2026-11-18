#!/bin/bash
# Renders the screen pages to docs/screens/*.png (needs g++, python3 with Pillow, and the U8g2
# library: the Arduino copy in ~/Arduino/libraries/U8g2_Arduino works).
set -e
cd "$(dirname "$0")"
U8=${U8G2:-$HOME/Arduino/libraries/U8g2_Arduino/src/clib}
W=$(mktemp -d)
# main.cpp's status page and its helpers, from fmtShort() up to the screen_pages.h include
sed -n '/^static String fmtShort/,/^#include "screen_pages.h"/p' ../../ShelfV4/main.cpp | grep -v '^#include "screen_pages.h"' > "$W/status_page.inc"
gcc -O1 -c -I"$U8" "$U8"/*.c 2>/dev/null; mv ./*.o "$W/"
g++ -std=c++17 -O1 -I"$U8" -I"$W" -I. sim.cpp "$W"/*.o -o "$W/sim" -lm
(cd "$W" && ./sim)
python3 png.py "$W" ../../docs/screens
rm -rf "$W"
