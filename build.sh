#!/bin/sh
# Builds tsfp-syslink.exe with clang + lld (Linux, WSL or Windows with LLVM installed).
set -e
cd "$(dirname "$0")"
mkdir -p build dist
CFLAGS="--target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin -nostdlib -fno-stack-protector -fno-jump-tables -Wall"

# 1. bridge payload: one position-independent .text blob, no relocations allowed
clang $CFLAGS -mno-stack-arg-probe -fno-unwind-tables -fno-asynchronous-unwind-tables -c src/bridge.c -o build/bridge.obj
python3 tools/embed.py build/bridge.obj build/payload.h   # extracts .text, rejects relocations/data

# 2. injector
llvm-dlltool -m i386:x86-64 -d tools/kernel32.def -l build/kernel32.lib
llvm-dlltool -m i386:x86-64 -d tools/user32.def -l build/user32.lib
clang $CFLAGS -mstack-probe-size=1000000 -Ibuild -c src/injector.c -o build/injector.obj
lld-link /nologo /subsystem:console /entry:mainCRTStartup /nodefaultlib /opt:ref \
  build/injector.obj build/kernel32.lib build/user32.lib /out:dist/tsfp-syslink.exe
echo "built dist/tsfp-syslink.exe"
