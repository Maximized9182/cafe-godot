#!/bin/sh
# Builds cafeglsl.rpl from the CafeGLSL Wii U static library
# (https://github.com/Exzap/CafeGLSL/releases, MIT).
# Usage: build.sh /path/to/cafeglsl   (the folder with lib/libcafeglsl.a)
# Copy the result to sd:/wiiu/libs/cafeglsl.rpl.
set -e
CAFEGLSL=${1:?usage: build.sh /path/to/cafeglsl}
: "${DEVKITPRO:=/opt/devkitpro}"
export DEVKITPRO
W=$DEVKITPRO/wut
PATH=$DEVKITPRO/tools/bin:$DEVKITPRO/devkitPPC/bin:$PATH
cd "$(dirname "$0")"
mkdir -p build
rplexportgen exports.def build/exports.s
powerpc-eabi-gcc -mcpu=750 -meabi -mhard-float -O2 -I$W/include -c rpl_main.c -o build/rpl_main.o
powerpc-eabi-gcc -c build/exports.s -o build/exports.o
powerpc-eabi-g++ -mcpu=750 -meabi -mhard-float -specs=$W/share/wut.specs -specs=$W/share/rpl.specs \
	-Wl,--entry=cafeglsl_start build/rpl_main.o build/exports.o \
	-L"$CAFEGLSL/lib" -L$W/lib -lcafeglsl -lstdc++ -lm -lwut -o build/cafeglsl.elf
elf2rpl --rpl build/cafeglsl.elf build/cafeglsl.rpl
echo "built $(pwd)/build/cafeglsl.rpl"
