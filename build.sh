#!/bin/sh
# Cross-compile SWOTL Enhanced for Windows (x86-64) with clang + mingw-w64 + SDL3.
set -e
CC=${CC:-/opt/mingw/bin/x86_64-w64-mingw32-clang}
RC=${RC:-/opt/mingw/bin/x86_64-w64-mingw32-windres}
SDL=${SDL:-/opt/sdl3-win}
mkdir -p build/win
$RC -I src src/swotl.rc -O coff -o build/win/swotl_res.o 2>/dev/null
$CC -O2 -std=c11 -Wall -Wno-unused-function -Wno-misleading-indentation \
  -I$SDL/include -o build/win/SWOTL-Enhanced.exe \
  src/main_sdl.c src/crt_gl.c src/recorder.c src/machine.c src/hires.c src/hle.c src/cpu.c third_party/opl3.c build/win/swotl_res.o \
  $SDL/lib/libSDL3.a -lm -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 \
  -lversion -luuid -ladvapi32 -lsetupapi -lshell32 -ldinput8 -mwindows -static
echo "built build/win/SWOTL-Enhanced.exe"
