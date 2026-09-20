@echo off
rem End-to-end demo: build depcheck, then build fwflash UNDER the interposer
rem (build-process trace), then trace a fwflash run (runtime trace).
rem Run inside a VS dev environment (msvc-env.cmd) so dumpbin is available
rem for the static-vs-runtime cross-check in the report.
setlocal
cd /d %~dp0
call msvc-env.cmd || exit /b 1

echo === [1/2] building depcheck (untraced) ===
call depcheck\build.cmd || exit /b 1

echo.
echo === [2/2] building fwflash UNDER depcheck (build-process trace) ===
depcheck\bin\depcheck.exe reports\build -- fwflash\build.cmd || exit /b 1

echo.
echo === bonus: fwflash runtime trace (create + flash) ===
depcheck\bin\depcheck.exe reports\flash -- fwflash\bin\fwflash.exe --create fwflash\bin\fwimage.bin
depcheck\bin\depcheck.exe reports\flash -- fwflash\bin\fwflash.exe --flash fwflash\bin\fwimage.bin

echo.
echo Reports:
echo   reports\build\report.md   (exec calls during the build)
echo   reports\flash\report.md   (runtime DLL/file/process deps)
endlocal
