@echo off
rem Build depcheck.dll (payload) and depcheck.exe (launcher), x64 static CRT.
setlocal
cd /d %~dp0..
call msvc-env.cmd || exit /b 1

if not exist depcheck\obj mkdir depcheck\obj
if not exist depcheck\bin mkdir depcheck\bin

set CPPFLAGS=/nologo /W3 /EHsc /O2 /MT /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /Ivendor\detours\include
set DTOURS=vendor\detours\lib.X64\detours.lib

cl %CPPFLAGS% /LD depcheck\src\payload.cpp ^
   /Fe:depcheck\bin\depcheck.dll /Fo:depcheck\obj\ ^
   /link %DTOURS% /def:depcheck\src\depcheck.def
if errorlevel 1 exit /b 1

cl %CPPFLAGS% depcheck\src\launcher.cpp ^
   /Fe:depcheck\bin\depcheck.exe /Fo:depcheck\obj\ ^
   /link %DTOURS%
if errorlevel 1 exit /b 1

del depcheck\obj\*.obj 2>nul
echo depcheck: built bin\depcheck.dll, bin\depcheck.exe
endlocal
