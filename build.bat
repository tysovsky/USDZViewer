@echo off
rem Builds USDZ Viewer with the Visual Studio C++ toolchain (CMake + Ninja bundled with Visual Studio).
rem Usage: build.bat [Release|Debug]
setlocal
set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=Release

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (
    echo Visual Studio Installer not found. Install Visual Studio 2022 with the "Desktop development with C++" workload.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if "%VSDIR%"=="" (
    echo No Visual Studio installation with the C++ tools was found.
    exit /b 1
)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set SRC=%~dp0
set OUT=%~dp0build\%CONFIG%
cmake -S "%SRC%." -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% || exit /b 1
cmake --build "%OUT%" || exit /b 1
echo.
echo Built: %OUT%\USDZViewer.exe
