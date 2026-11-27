@echo off
rem ---- Vitreous (C / Win32 + OpenGL 3.3) ----------------------------------
rem Requires MinGW-w64 gcc on PATH (or edit CC below).
setlocal
set CC=gcc
set OUT=vitreous.exe

%CC% -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter ^
  -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 ^
  vitreous.c -o %OUT% ^
  -lopengl32 -lgdi32 -luser32 -lole32 -luuid -lshell32 -lcomdlg32 ^
  -mwindows -lm

if errorlevel 1 (
  echo Build FAILED
  exit /b 1
)
echo Built %OUT%
endlocal
