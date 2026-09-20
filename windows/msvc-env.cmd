@echo off
rem Common MSVC dev-environment entry point for all build scripts.
rem Finds the latest VS with the C++ toolchain via vswhere (covers VS Build
rem Tools in windows containers), falls back to the dev-machine install.
setlocal EnableExtensions
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo msvc-env: no vcvars64.bat found via vswhere or fallback path 1>&2
    exit /b 1
)
endlocal & call "%VCVARS%" %*
