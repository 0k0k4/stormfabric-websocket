@echo off
rem Builds the websocket mod and its offline self-test. Requires MSVC x64 (cl.exe).
rem SDK headers: include\ (copied from StormFabric), or set SF_SDK to another include folder.
setlocal

if not defined VCVARS (
  if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  ) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  ) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  )
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined VCVARS if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
    if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
  )
)
if not defined VCVARS (
  echo ERROR: vcvars64.bat not found. Set VCVARS.
  exit /b 1
)
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build

if not defined SF_SDK set "SF_SDK=%~dp0include"
set CFLAGS=/nologo /O2 /MT /EHsc /std:c++17 /W4 /WX /DUNICODE /D_UNICODE /I"%SF_SDK%"

for %%d in (obj_dll obj_selftest obj_modtest) do if not exist build\%%d mkdir build\%%d
set SRC=src\ws_client.cpp src\ws_registry.cpp

rem The mod itself. Never name it like a System32 DLL (websocket.dll): Windows would hand
rem it to WinHTTP in place of the real one.
cl %CFLAGS% /LD /Fo:build\obj_dll\ /Fe:build\sf_websocket.dll src\ws_mod.cpp %SRC% ^
   /link /DLL /MANIFEST:NO /DEBUG:NONE winhttp.lib || exit /b 1

rem Offline tests (need tests\echo_server.py running)
cl %CFLAGS% /Fo:build\obj_selftest\ /Fe:build\ws_selftest.exe tests\ws_selftest.cpp src\ws_client.cpp ^
   /link /DEBUG:NONE winhttp.lib || exit /b 1
cl %CFLAGS% /Fo:build\obj_modtest\ /Fe:build\mod_test.exe tests\mod_test.cpp src\ws_mod.cpp %SRC% ^
   /link /DEBUG:NONE winhttp.lib || exit /b 1

echo OK
