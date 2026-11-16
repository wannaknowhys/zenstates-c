@echo off
REM build.bat -- build zenstates-c.exe, zenstates-c-gui.exe and zenstates-vf.exe on Windows.
REM Needs either MinGW-w64 gcc in PATH, or MSVC cl (Developer Command Prompt).
REM Run from this folder. Output: zenstates-c.exe, zenstates-c-gui.exe, zenstates-vf.exe
setlocal
where gcc >nul 2>nul
if %ERRORLEVEL%==0 goto :mingw
where cl >nul 2>nul
if %ERRORLEVEL%==0 goto :msvc
echo Neither gcc (MinGW-w64) nor cl (MSVC) was found in PATH.
echo Install MinGW-w64 (https://www.mingw-w64.org/) and retry.
exit /b 1

:mingw
echo Building with MinGW-w64 gcc...
windres zenstates-c.rc -O coff -o zenstates-c-res.o
if %ERRORLEVEL% neq 0 echo WARNING: windres failed for zenstates-c.rc; building without manifest.
windres zenstates-c-gui.rc -O coff -o zenstates-c-gui-res.o
if %ERRORLEVEL% neq 0 echo WARNING: windres failed for zenstates-c-gui.rc; building without manifest.
windres zenstates-vf.rc -O coff -o zenstates-vf-res.o
if %ERRORLEVEL% neq 0 echo WARNING: windres failed for zenstates-vf.rc; building without manifest.
if exist zenstates-c-res.o (
  gcc -O2 -Wall -Wextra -o zenstates-c.exe zenstates-c.c zenstates-c-res.o
) else (
  gcc -O2 -Wall -Wextra -o zenstates-c.exe zenstates-c.c
)
if exist zenstates-c-gui-res.o (
  gcc -O2 -Wall -Wextra -mwindows -o zenstates-c-gui.exe zenstates-c-gui.c zenstates-c-gui-res.o -lcomctl32
) else (
  gcc -O2 -Wall -Wextra -mwindows -o zenstates-c-gui.exe zenstates-c-gui.c -lcomctl32
)
if exist zenstates-vf-res.o (
  gcc -O2 -Wall -Wextra -o zenstates-vf.exe zenstates-vf.c zenstates-vf-res.o
) else (
  gcc -O2 -Wall -Wextra -o zenstates-vf.exe zenstates-vf.c
)
goto :done

:msvc
echo Building with MSVC cl...
rc zenstates-c.rc
rc zenstates-c-gui.rc
rc zenstates-vf.rc
cl /O2 /W3 /Fe:zenstates-c.exe zenstates-c.c zenstates-c.res
cl /O2 /W3 /Fe:zenstates-c-gui.exe zenstates-c-gui.c zenstates-c-gui.res comctl32.lib /link /SUBSYSTEM:WINDOWS
cl /O2 /W3 /Fe:zenstates-vf.exe zenstates-vf.c zenstates-vf.res
goto :done

:done
dir zenstates-c.exe zenstates-c-gui.exe zenstates-vf.exe
echo Done. Copy the whole folder (exe + ini + modules\) anywhere you like -- keep all files together.
echo and run install-autostart.bat as Administrator.
