@echo off
rem Configures and builds the mod (32-bit, MSVC x86, CMake + Ninja) into out\.
rem   build.cmd       RelWithDebInfo (the release build: out\XINPUT1_3.dll and its .pdb)
rem   build.cmd test  the same, then the unit tests (ctest --test-dir out) in the same environment
rem   BUILD_TYPE=...  another configuration (only used when out\ is configured the first time)
rem Visual Studio: an x86 developer shell is used as it is (vcvars32, CI's msvc-dev-cmd); otherwise
rem vswhere finds the newest install with the C++ x86/x64 tools (fallback: VS 18 Build Tools).
rem Ninja: C:\ninja.exe if it exists, else ninja on PATH (Visual Studio ships one).
setlocal
cd /d "%~dp0"

if /i "%VSCMD_ARG_TGT_ARCH%"=="x86" goto have_env
set "VSINSTALLER=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
set "VSDIR="
if not exist "%VSINSTALLER%\vswhere.exe" goto vs_fallback
for /f "usebackq delims=" %%i in (`"%VSINSTALLER%\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
:vs_fallback
if not defined VSDIR set "VSDIR=%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools"
if exist "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" goto vs_found
echo build.cmd: no vcvars32.bat under "%VSDIR%"; install the Visual Studio C++ build tools 1>&2
exit /b 1
:vs_found
rem vcvars runs vswhere itself and complains on stderr when it is not on PATH.
set "PATH=%VSINSTALLER%;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
:have_env

set "NINJA=ninja"
if exist "C:\ninja.exe" set "NINJA=C:/ninja.exe"
if not defined BUILD_TYPE set "BUILD_TYPE=RelWithDebInfo"

if exist out\build.ninja goto build
cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=%BUILD_TYPE% "-DCMAKE_MAKE_PROGRAM=%NINJA%" -DCMAKE_CXX_COMPILER=cl
if errorlevel 1 exit /b 1
:build
cmake --build out
if errorlevel 1 exit /b 1
echo built out\XINPUT1_3.dll
if /i not "%~1"=="test" exit /b 0
ctest --test-dir out --output-on-failure
if errorlevel 1 exit /b 1
