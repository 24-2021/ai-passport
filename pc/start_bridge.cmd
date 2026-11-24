@echo off
setlocal EnableExtensions
cd /d "%~dp0.."

set "AUTOSTART=0"
set "EXTRA="
if /I "%~1"=="--autostart" (
    set "AUTOSTART=1"
) else (
    set "EXTRA=%*"
)

set "PY="
where py >nul 2>nul
if not errorlevel 1 set "PY=py -3"
if not defined PY (
    where python >nul 2>nul
    if not errorlevel 1 set "PY=python"
)
if not defined PY (
    echo Python 3.9 or newer was not found.
    echo Install Python from https://www.python.org/downloads/windows/
    if "%AUTOSTART%"=="0" pause
    exit /b 2
)

%PY% -c "import serial, bleak, sounddevice" >nul 2>nul
if errorlevel 1 (
    echo Installing kfb24 bridge dependencies...
    %PY% -m pip install -r "pc\requirements.txt"
    if errorlevel 1 (
        echo Dependency installation failed. Check the network and retry.
        if "%AUTOSTART%"=="0" pause
        exit /b 2
    )
)

if "%AUTOSTART%"=="0" echo Starting kfb24 bridge. Press Ctrl+C to stop.
%PY% "pc\kfb24_bridge.py" --ble %EXTRA%
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
    echo.
    echo kfb24 bridge exited with code %RC%.
    if "%AUTOSTART%"=="0" pause
)
exit /b %RC%
