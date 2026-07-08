#!/usr/bin/env bash
# MSXFileForge build script (MinGW-w64). Adjust MINGW if g++ is elsewhere.
set -e
MINGW="${MINGW:-/d/AgentWorkspace/sdk/mingw64/bin}"
export PATH="$MINGW:$PATH"
cd "$(dirname "$0")"
mkdir -p build

CORE="core/disk_image.cpp core/fat12.cpp core/msxfile.cpp core/rom.cpp core/cas.cpp"

echo "[1/3] CLI"
g++ -std=c++17 -O2 -o build/msxforge.exe cli/main.cpp $CORE

echo "[2/3] dsk2dmk tool"
g++ -std=c++17 -O2 -o build/dsk2dmk.exe tools/dsk2dmk.cpp

echo "[3/3] GUI"
g++ -std=c++17 -O2 -DSDL_MAIN_HANDLED \
  -I libs/imgui -I libs/SDL2/include \
  gui/main.cpp $CORE \
  libs/imgui/imgui.cpp libs/imgui/imgui_draw.cpp libs/imgui/imgui_tables.cpp libs/imgui/imgui_widgets.cpp \
  libs/imgui/backends/imgui_impl_sdl2.cpp libs/imgui/backends/imgui_impl_opengl3.cpp \
  libs/SDL2/lib/SDL2.dll -lopengl32 -lcomdlg32 \
  -o build/msxforge-gui.exe
cp -f libs/SDL2/lib/SDL2.dll build/

echo "done -> build/"
