#!/bin/sh
# Builds Cafe Godot's patched wuhbtool: devkitPro's wuhbtool (wut-tools, GPL-2.0)
# with a --boot-sound option (meta/bootSound.btsnd, played by the Wii U Menu
# through Aroma's Homebrew on Menu plugin) and stb_image (public domain) in
# place of FreeImage, which most distributions no longer package.
# Needs git, g++, zlib. Installs to $CAFE_GODOT_HOME/bin/wuhbtool (default
# ~/.local/share/cafe-godot), keeping an existing one as wuhbtool.devkitpro.
set -e
WUT_TOOLS_COMMIT=b22de1a3c0c8950c4ad4b4df170c89c05d3196aa
STB_COMMIT=f58f558c120e9b32c217290b80bad1a0729fbb2c
HERE="$(cd "$(dirname "$0")" && pwd)"
CAFE_HOME="${CAFE_GODOT_HOME:-$HOME/.local/share/cafe-godot}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

git clone -q https://github.com/devkitPro/wut-tools "$WORK/wut-tools"
cd "$WORK/wut-tools"
git checkout -q "$WUT_TOOLS_COMMIT"
git submodule update --init --depth 1 libraries/excmd
git apply "$HERE/wuhbtool-cafe.patch"
curl -sSfL -o src/wuhbtool/services/stb_image.h \
	"https://raw.githubusercontent.com/nothings/stb/$STB_COMMIT/stb_image.h"
g++ -std=c++17 -O2 -Ilibraries/excmd/src \
	src/wuhbtool/main.cpp src/wuhbtool/entities/*.cpp \
	src/wuhbtool/services/RomFSService.cpp src/wuhbtool/services/TgaGzService.cpp \
	src/wuhbtool/utils/filepath.cpp -lz -o wuhbtool

mkdir -p "$CAFE_HOME/bin"
if [ -e "$CAFE_HOME/bin/wuhbtool" ] && [ ! -e "$CAFE_HOME/bin/wuhbtool.devkitpro" ]; then
	cp -p "$CAFE_HOME/bin/wuhbtool" "$CAFE_HOME/bin/wuhbtool.devkitpro"
fi
cp wuhbtool "$CAFE_HOME/bin/wuhbtool"
echo "installed $CAFE_HOME/bin/wuhbtool"
