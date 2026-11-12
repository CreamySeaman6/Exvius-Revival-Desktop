#!/bin/sh
set -e
cd "$(dirname "$0")"
BACKEND=${BACKEND:-dyn}
mkdir -p build/linux/obj
CFLAGS="-O2 -g -Wall -Wno-unused-function -Wno-misleading-indentation $(sdl2-config --cflags)"
for f in main platform host_gl host_audio host_http host_aes host_web ui; do gcc $CFLAGS -c runner/$f.c -o build/linux/obj/$f.o; done
OBJS="build/linux/obj/main.o build/linux/obj/platform.o build/linux/obj/host_gl.o build/linux/obj/host_audio.o build/linux/obj/host_http.o build/linux/obj/host_aes.o build/linux/obj/host_web.o build/linux/obj/ui.o"
if [ "$BACKEND" = uc ]; then
  UC=$(python3 -c "import unicorn,os;print(os.path.dirname(unicorn.__file__))")
  gcc $CFLAGS -I"$UC/include" -c runner/cpu_uc.c -o build/linux/obj/cpu_uc.o
  gcc -o build/linux/runner-uc $OBJS build/linux/obj/cpu_uc.o "$UC/lib/libunicorn.a" $(sdl2-config --libs) -lm -lpthread
else
  DYN=${DYN:-/tmp/dynarmic}; DB=${DB:-/tmp/dyn-linux}
  g++ -std=c++20 -O2 -g -I"$DYN/src" -c runner/cpu_dyn.cpp -o build/linux/obj/cpu_dyn.o
  g++ -o build/linux/runner $OBJS build/linux/obj/cpu_dyn.o $(find "$DB" -name 'libdynarmic.a') $(find "$DB" -name '*.a' ! -name 'libdynarmic.a' | sort) $(sdl2-config --libs) -lm -lpthread
fi
