@echo off
setlocal EnableExtensions
set "KFB24_LNK=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\kfb24 Bridge.lnk"

if exist "%KFB24_LNK%" del /q "%KFB24_LNK%"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'wscript.exe' -and $_.CommandLine -match 'start_bridge_hidden\.vbs' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"
timeout /t 1 /nobreak >nul
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'kfb24_bridge\.py' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"
echo kfb24 autostart removed, its supervisor stopped, and its bridge process stopped.
pause
