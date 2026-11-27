#!/usr/bin/env bash
# Vitreous (C / Win32 + OpenGL 3.3) — MSYS / Git Bash build
set -e
CC=${CC:-gcc}
OUT=${OUT:-vitreous.exe}
"$CC" -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
    -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
    vitreous.c -o "$OUT" \
    -lopengl32 -lgdi32 -luser32 -lole32 -luuid -lshell32 -lcomdlg32 \
    -mwindows -lm
echo "Built $OUT"
