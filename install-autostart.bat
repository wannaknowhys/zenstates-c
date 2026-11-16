@echo off
REM install-autostart.bat -- run AS ADMINISTRATOR from the install folder.
REM Creates scheduled task "ZenStates-C": runs zenstates-c.exe at boot as
REM SYSTEM with highest privileges, 30 s delay. No UAC prompt at boot.
REM (A manifest requireAdministrator exe in a Run key would prompt UAC at
REM every logon with nobody to click it -- that is why this uses a task.)
setlocal
net session >nul 2>&1
if %ERRORLEVEL% neq 0 (
  echo ERROR: please right-click this file and choose "Run as administrator".
  pause
  exit /b 1
)
set "EXE=%~dp0zenstates-c.exe"
if not exist "%EXE%" (
  echo ERROR: zenstates-c.exe not found next to this script: %EXE%
  pause
  exit /b 1
)
schtasks /create /TN "ZenStates-C" /TR "\"%EXE%\"" /SC ONSTART /RU SYSTEM /RL HIGHEST /DELAY 0000:30 /F
if %ERRORLEVEL% neq 0 (
  echo ERROR: schtasks failed with code %ERRORLEVEL%.
  pause
  exit /b 1
)
echo OK: task "ZenStates-C" installed. It will run at next boot.
echo Config is read from zenstates-c.ini next to the exe.
echo To test now (elevated): right-click zenstates-c.exe ^> Run as administrator.
pause
