@echo off
rem Build fwflash.exe, device.dll and worker.exe (x64, static CRT).
setlocal
cd /d %~dp0..
call msvc-env.cmd || exit /b 1

if not exist fwflash\obj mkdir fwflash\obj
if not exist fwflash\bin mkdir fwflash\bin

set CFLAGS=/nologo /W4 /O2 /MT /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS

rem fwflash.exe — bcrypt.dll is a DELAY import (loads only when hashing runs)
cl %CFLAGS% fwflash\src\main.c fwflash\src\fwimage.c fwflash\src\crc32.c ^
   /Fe:fwflash\bin\fwflash.exe /Fo:fwflash\obj\ ^
   /link /DELAYLOAD:bcrypt.dll delayimp.lib bcrypt.lib
if errorlevel 1 exit /b 1

rem device.dll — the runtime-loaded plugin
cl %CFLAGS% /LD fwflash\device\device.c ^
   /Fe:fwflash\bin\device.dll /Fo:fwflash\obj\
if errorlevel 1 exit /b 1

rem worker.exe — the child verify process
cl %CFLAGS% fwflash\worker\worker.c fwflash\src\crc32.c ^
   /Fe:fwflash\bin\worker.exe /Fo:fwflash\obj\
if errorlevel 1 exit /b 1

del fwflash\obj\*.obj 2>nul
echo fwflash: built bin\fwflash.exe, bin\device.dll, bin\worker.exe
endlocal
