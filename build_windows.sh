#!/bin/sh
# Cross-builds the Windows runner with mingw-w64 (posix thread variant).
set -e
cd "$(dirname "$0")"
SDL=${SDL:-$(ls -d /tmp/sdlwin/SDL2-*/x86_64-w64-mingw32)}
DYN=${DYN:-/tmp/dynarmic}; DB=${DB:-/tmp/dyn-win}
CC=x86_64-w64-mingw32-gcc-posix; CXX=x86_64-w64-mingw32-g++-posix
O=build/windows/obj; mkdir -p $O
CFLAGS="-O2 -g -Wall -Wno-unused-function -Wno-misleading-indentation -I$SDL/include/SDL2 -I$SDL/include -DSDL_MAIN_HANDLED"
for f in main platform host_gl host_audio host_http host_aes host_web ui; do $CC $CFLAGS -c runner/$f.c -o $O/$f.o; done
$CXX -std=c++20 -O2 -g -I"$DYN/src" -c runner/cpu_dyn.cpp -o $O/cpu_dyn.o
# The programs' file icon: this program's own crystal, drawn at build time (tools/make_icon.py).
python3 tools/make_icon.py $O/app.ico
x86_64-w64-mingw32-windres -I $O runner/runner.rc -O coff -o $O/res.o
x86_64-w64-mingw32-windres -I $O stub/stub.rc -O coff -o $O/stub_res.o
RES=$O/res.o
$CXX -static -static-libgcc -static-libstdc++ -o build/windows/runner.exe $O/main.o $O/platform.o $O/host_gl.o $O/host_audio.o $O/host_http.o $O/host_aes.o $O/host_web.o $O/ui.o $O/cpu_dyn.o $RES \
  $(find "$DB" -name 'libdynarmic.a') $(find "$DB" -name '*.a' ! -name 'libdynarmic.a' | sort) \
  -L$SDL/lib -Wl,-Bdynamic -lSDL2 -Wl,-Bstatic -lopengl32 -lws2_32 -lwinmm -ladvapi32 -lshell32 -luser32 -lgdi32 -lm -lpthread
cp $SDL/bin/SDL2.dll build/windows/
x86_64-w64-mingw32-strip -o build/windows/runner-stripped.exe build/windows/runner.exe
$CC -O2 -municode -mwindows -o build/windows/stub.exe stub/stub.c $O/stub_res.o
ls -la build/windows
