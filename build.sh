#!/bin/sh
# Builds PdkTool.exe (32-bit Windows, static) with MinGW-w64 on Linux/macOS or MSYS2.
# 32-bit on purpose: the Tactrix op20pt32.dll J2534 driver is a 32-bit DLL.
set -e
cd "$(dirname "$0")/src"
i686-w64-mingw32-g++ -O2 -static -std=c++17 -mwindows -Wall -Wno-unused-function \
  -Wno-misleading-indentation -Wno-comment pdktool.cpp -o ../PdkTool.exe \
  -lcomdlg32 -lgdi32 -luser32 -lkernel32 -ladvapi32
echo "built PdkTool.exe"
