#!/usr/bin/env bash
# MSXFileForge build for Linux / macOS (CMake + system SDL2).
#
# Prerequisites:
#   Debian/Ubuntu : sudo apt install -y build-essential cmake libsdl2-dev
#                   (runtime file dialogs use zenity or kdialog)
#   Fedora        : sudo dnf install -y gcc-c++ cmake SDL2-devel
#   macOS (brew)  : brew install cmake sdl2
set -e
cd "$(dirname "$0")"
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
echo
echo "Built:"
ls -1 build/msxforge build/msxforge-gui build/dsk2dmk 2>/dev/null || true
