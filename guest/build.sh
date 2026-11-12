#!/bin/sh
set -e
cd "$(dirname "$0")"
CC=aarch64-linux-gnu-gcc
mkdir -p ../build
$CC -O1 -g -w -fno-builtin -c libc_tab.c -o ../build/libc_tab.o
$CC -O2 -g -fno-omit-frame-pointer -static -Wall -Wno-unused-function -Wno-unused-result -Wno-format-truncation -Wno-unused-but-set-variable -Wno-unused-variable -Wno-misleading-indentation \
  -o ../build/game.elf loader.c shims.c jni.c gltrace.c http.c aes.c sl.c text.c ../build/libc_tab.o -lm -lpthread
