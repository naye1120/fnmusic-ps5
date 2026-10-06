#!/usr/bin/env bash
# ps5-homebrew-ui - Rebuild the baked SDF fonts in assets/fonts.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Each line of the table is: source, output name, pixel size, SDF range,
# atlas size, glyph set. The two last columns are optional and default to the
# 1024 `basic` set this repository ships; `2048 european` covers accented
# Latin, Greek and Cyrillic, and `@gb2312-codepoints.txt` (a code point list
# read from third_party/fonts, one hex value per line) covers the 6763 Han
# signs of GB2312 plus kana and punctuation - an app that shows names it did
# not write needs one of those. The whole U+4E00..U+9FFF block (`cjk`) is more
# than an 8192 atlas can hold at a readable size, so it is here for reference.
# To add a typeface, drop its TTF and licence into third_party/fonts, add a
# line here, run `make fonts`, and load the new .huifont where the others are
# loaded (src/main.cpp and host/snapshot_main.cpp).

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cxx=$(command -v "${HOST_CXX:-clang++}")
mkdir -p "$root/build/host" "$root/assets/fonts"
"$cxx" -std=c++20 -O2 -w "$root/tools/font-baker/bake_font.cpp" -o "$root/build/host/bake_font"
while read -r source output size range atlas glyphs; do
    [[ -n $source && $source != \#* ]] || continue
    [[ -n $atlas ]] || atlas=1024
    [[ -n $glyphs ]] || glyphs=basic
    "$root/build/host/bake_font" "$root/third_party/fonts/$source" \
        "$root/assets/fonts/$output.huifont" "$size" "$range" "$atlas" "$glyphs"
done <<'FONTS'
Inter-Regular.ttf inter-regular 56 8
Inter-SemiBold.ttf inter-semibold 56 8
Montserrat-Medium.ttf montserrat-medium 56 8
DejaVuSansMono.ttf dejavu-sans-mono 52 8
PressStart2P-Regular.ttf press-start-2p 32 4
PatrickHand-Regular.ttf patrick-hand 56 8
# The Chinese face is baked smaller than the Latin ones: 44 pixels at range 7
# keeps the 7000-sign set inside one 8192-wide atlas. SDF text scales cleanly,
# so it is still crisp at the sizes this app draws at.
wqy-microhei.ttc wenquanyi-cjk 44 7 8192 @gb2312-codepoints.txt
FONTS
cp "$root"/third_party/fonts/*-LICENSE.txt "$root/assets/fonts/"
