#!/bin/sh
# build_host.sh <repo root> <out binary> - the host harness of the in-game menu (tests/psp_imgui/host_menu.cpp)
set -e
R=$1; O=$2
P=$(ls -d "$R"/games/*/src | while read d; do grep -l '"map_l2"' "$d/hle_settings.c" >/dev/null 2>&1 && echo "$d"; done | head -1)
T=$(mktemp -d)
CF="-O1 -g -Wall -Wno-unused-function -I$R/hle/core -I$R/hle/psp"
gcc $CF -DHLE_CFG_STANDALONE -c "$R/hle/core/hle_cfg.c" -o "$T/cfg.o"
gcc $CF -Werror -c "$R/hle/core/hle_cfg_save.c" -o "$T/save.o"
gcc $CF -c "$P/hle_settings.c" -o "$T/set.o"
gcc $CF -c "$P/hle_settings_menu.c" -o "$T/reg.o"
CX="-O1 -g -DPIMGUI_HOST -DIMGUI_USER_CONFIG=\"pimgui_imconfig.h\" -fno-exceptions -fno-rtti -I$R/lib/psp_imgui -I$R/lib/psp_imgui/third_party/imgui -I$R/hle/core -I$R/hle/psp"
for f in imgui imgui_draw imgui_widgets imgui_tables; do g++ $CX -w -c "$R/lib/psp_imgui/third_party/imgui/$f.cpp" -o "$T/$f.o" & done
g++ $CX -Wall -Werror -c "$R/lib/psp_imgui/psp_imgui.cpp" -o "$T/pimgui.o"
g++ $CX -Wall -Werror -Wno-format-truncation -c "$R/hle/psp/hle_menu.cpp" -o "$T/menu.o"
g++ $CX -Wall -c "$R/tests/psp_imgui/host_menu.cpp" -o "$T/host.o"
wait
g++ "$T"/*.o -o "$O"
rm -rf "$T"
