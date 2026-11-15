@echo off
rem Installs the websocket mod into <game>\mods\websocket and enables it.
rem Usage: install.bat <game_folder>   (the game must be closed)
setlocal
set "GAME=%~1"
if "%GAME%"=="" (
  echo Usage: install.bat ^<game_folder^>   e.g. install.bat "C:\Games\Stormworks"
  exit /b 1
)
if not exist "%GAME%\stormworks64.exe" (
  echo stormworks64.exe not found in "%GAME%"
  exit /b 1
)
tasklist /FI "IMAGENAME eq stormworks64.exe" | find /I "stormworks64.exe" >nul && (
  echo Stormworks is running: close it first.
  exit /b 1
)
if not exist "%~dp0build\sf_websocket.dll" (
  echo build\sf_websocket.dll missing: run build.bat first
  exit /b 1
)
set "DEST=%GAME%\mods\websocket"
if not exist "%DEST%" mkdir "%DEST%"
copy /Y "%~dp0mod.json" "%DEST%\mod.json" >nul || exit /b 1
copy /Y "%~dp0build\sf_websocket.dll" "%DEST%\sf_websocket.dll" >nul || exit /b 1
rem Older builds were named websocket.dll, which shadows the Windows DLL of that name.
if exist "%DEST%\websocket.dll" del "%DEST%\websocket.dll"

set "INI=%GAME%\mods\_modloader.ini"
rem "(echo ...)>>" keeps cmd from reading "=1>>" as a redirection of handle 1.
findstr /B /C:"websocket=" "%INI%" >nul 2>&1 || ((echo websocket=1)>>"%INI%")
findstr /B /C:"websocket=1" "%INI%" >nul 2>&1 || echo NOTE: websocket is disabled in "%INI%" - enable it in the MODS menu.

echo Installed: "%DEST%"
echo Log: "%GAME%\StormworksModLoader.log"
echo Config (created on first launch): "%DEST%\config\websocket.cfg.bin"
