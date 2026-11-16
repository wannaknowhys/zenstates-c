@echo off
REM uninstall-autostart.bat -- run AS ADMINISTRATOR. Removes the task.
REM Does not delete any files; the cap itself is not persistent, so after
REM a reboot the CPU is back to stock behaviour.
setlocal
net session >nul 2>&1
if %ERRORLEVEL% neq 0 (
  echo ERROR: please right-click this file and choose "Run as administrator".
  pause
  exit /b 1
)
schtasks /delete /TN "ZenStates-C" /F
if %ERRORLEVEL% neq 0 (
  echo schtasks reported code %ERRORLEVEL% (the task may not have existed).
  pause
  exit /b 1
)
echo OK: task "ZenStates-C" removed.
pause
