#!/bin/sh
# bake_font.sh - rebuilds ../pimgui_font13.h with the host g++ (run from any directory: sh tools/bake_font.sh)
set -e
D=$(cd "$(dirname "$0")" && pwd)
L=$D/..
T=${TMPDIR:-/tmp}/pimgui_bake_$$
g++ -O1 -DPIMGUI_HOST '-DIMGUI_USER_CONFIG="pimgui_imconfig.h"' -I"$L" -I"$L/third_party/imgui" "$D/bake_font.cpp" \
    "$L/third_party/imgui/imgui.cpp" "$L/third_party/imgui/imgui_draw.cpp" "$L/third_party/imgui/imgui_widgets.cpp" "$L/third_party/imgui/imgui_tables.cpp" -o "$T"
"$T" > "$L/pimgui_font13.h"
rm -f "$T"
