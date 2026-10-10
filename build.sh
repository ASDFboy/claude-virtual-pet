#!/bin/sh
# Cross-compile plugin/bin/ClaudePet.exe from Linux/macOS with Zig:  pip install ziglang
# On Windows with MinGW-w64 (gcc):  see README.md ("Building").
set -e
cd "$(dirname "$0")"
mkdir -p plugin/bin
python3 -m ziglang cc -target x86_64-windows-gnu -O2 -s -Wall -Wno-unused-function \
  -Wl,--subsystem,windows -o plugin/bin/ClaudePet.exe src/claudepet.c \
  -luser32 -lgdi32 -lshell32 -ladvapi32 -ldwmapi
echo "built plugin/bin/ClaudePet.exe"
