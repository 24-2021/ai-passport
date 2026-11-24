@echo off
setlocal EnableExtensions
set "KFB24_VBS=%~dp0start_bridge_hidden.vbs"
set "KFB24_LNK=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\kfb24 Bridge.lnk"

if not exist "%KFB24_VBS%" (
    echo Missing %KFB24_VBS%
    pause
    exit /b 2
)

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$vbs=$env:KFB24_VBS; $lnk=$env:KFB24_LNK;" ^
  "$s=(New-Object -ComObject WScript.Shell).CreateShortcut($lnk);" ^
  "$s.TargetPath=Join-Path $env:SystemRoot 'System32\wscript.exe';" ^
  "$s.Arguments=[char]34 + $vbs + [char]34;" ^
  "$s.WorkingDirectory=Split-Path $vbs;" ^
  "$s.Description='kfb24 PC bridge';" ^
  "$s.Save()"
if errorlevel 1 (
    echo Could not create the startup shortcut.
    pause
    exit /b 2
)

start "" wscript.exe "%KFB24_VBS%"
echo kfb24 bridge will start at every login.
echo It is starting now in the background.
echo Log: %LOCALAPPDATA%\kfb24\bridge.log
pause
