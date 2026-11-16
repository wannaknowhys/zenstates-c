#!/bin/sh
# build.sh -- cross-compile the Windows binaries on Linux (verification build).
# Needs: x86_64-w64-mingw32-gcc and windres (package mingw-w64 on Debian/Ubuntu).
#   ./build.sh
# Output: zenstates-c.exe, zenstates-c-gui.exe, zenstates-c-gui-xl.exe
# (the 2x-layout GUI, built from the same source with -DGUI_SCALE=2),
# zenstates-vf.exe, zenstates-hsmp.exe in this folder.
set -e
cd "$(dirname "$0")"
CC=${CC:-x86_64-w64-mingw32-gcc}
# WINDRES: Debian cross packages name it x86_64-w64-mingw32-windres, but
# MSYS2's mingw-w64 binutils ships only plain `windres` (no triplet
# prefix; gcc is the exception and provides both names). Prefer the
# triplet name, fall back to plain windres so the same script works in
# CI (MSYS2 MINGW64) and in a Linux cross-compile. Fail with a clear
# message instead of set -e's bare exit 127 if neither exists.
if [ -z "$WINDRES" ]; then
  if command -v x86_64-w64-mingw32-windres >/dev/null 2>&1; then
    WINDRES=x86_64-w64-mingw32-windres
  elif command -v windres >/dev/null 2>&1; then
    WINDRES=windres
  else
    echo "ERROR: windres not found (looked for x86_64-w64-mingw32-windres and windres)" >&2
    exit 1
  fi
fi
"$WINDRES" zenstates-c.rc -O coff -o zenstates-c-res.o
"$WINDRES" zenstates-c-gui.rc -O coff -o zenstates-c-gui-res.o
"$WINDRES" zenstates-c-gui-xl.rc -O coff -o zenstates-c-gui-xl-res.o
"$WINDRES" zenstates-vf.rc -O coff -o zenstates-vf-res.o
"$WINDRES" zenstates-hsmp.rc -O coff -o zenstates-hsmp-res.o
"$CC" -O2 -Wall -Wextra -o zenstates-c.exe zenstates-c.c zenstates-c-res.o
"$CC" -O2 -Wall -Wextra -mwindows -o zenstates-c-gui.exe zenstates-c-gui.c zenstates-c-gui-res.o -lcomctl32
"$CC" -O2 -Wall -Wextra -mwindows -DGUI_SCALE=2 -o zenstates-c-gui-xl.exe zenstates-c-gui.c zenstates-c-gui-xl-res.o -lcomctl32
"$CC" -O2 -Wall -Wextra -o zenstates-vf.exe zenstates-vf.c zenstates-vf-res.o
"$CC" -O2 -Wall -Wextra -o zenstates-hsmp.exe zenstates-hsmp.c zenstates-hsmp-res.o
ls -l zenstates-c.exe zenstates-c-gui.exe zenstates-c-gui-xl.exe zenstates-vf.exe zenstates-hsmp.exe
file zenstates-c.exe zenstates-c-gui.exe zenstates-c-gui-xl.exe zenstates-vf.exe zenstates-hsmp.exe
