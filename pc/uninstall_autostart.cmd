@echo off
setlocal EnableExtensions
set "KFB24_LNK=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\kfb24 Bridge.lnk"

if exist "%KFB24_LNK%" del /q "%KFB24_LNK%"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match 'kfb24_bridge\.py' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"
echo kfb24 autostart removed and its bridge process stopped.
pause
