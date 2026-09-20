@echo off
rem Short Windows scenario, mirroring linux/tests/run_scenario.sh:
rem build the interposer, run a build and a runtime trace under it, then
rem assert the expected events landed in the merged depcheck report.
rem Exit codes: 0 = all assertions passed, 1 = assertion failure, 2 = setup.
setlocal EnableExtensions
cd /d "%~dp0.."
set FAIL=0

echo == environment ==
ver

echo == [1/3] building depcheck (untraced) ==
call depcheck\build.cmd || exit /b 2

echo == [2/3] build trace: fwflash built under depcheck ==
if exist reports\scenario-build rmdir /s /q reports\scenario-build
depcheck\bin\depcheck.exe reports\scenario-build -- fwflash\build.cmd || set FAIL=1

echo == [3/3] runtime trace: fwflash create + flash under depcheck ==
if exist reports\scenario-flash rmdir /s /q reports\scenario-flash
depcheck\bin\depcheck.exe reports\scenario-flash -- fwflash\bin\fwflash.exe --create fwflash\bin\fwimage.bin || set FAIL=1
depcheck\bin\depcheck.exe reports\scenario-flash -- fwflash\bin\fwflash.exe --flash fwflash\bin\fwimage.bin || set FAIL=1

call :assert "build-trace cl.exe exec"       cl.exe      reports\scenario-build\report.md
call :assert "build-trace vswhere.exe exec"  vswhere.exe reports\scenario-build\report.md
call :assert "flash-trace worker.exe child"  worker.exe  reports\scenario-flash\report.md
call :assert "flash-trace device.dll load"   device.dll  reports\scenario-flash\report.md

if not "%FAIL%"=="0" echo RESULT: FAIL& exit /b 1
echo RESULT: ALL PASS
exit /b 0

rem --- assert <label> <needle> <report-file> : grep the report, record failure
:assert
if not exist "%~3" (
    echo FAIL [%~1]: report missing: %~3
    set FAIL=1
    goto :eof
)
findstr /C:"%~2" "%~3" >nul
if errorlevel 1 (
    echo FAIL [%~1]: no '%~2' found in %~3
    set FAIL=1
) else (
    echo PASS [%~1]
)
goto :eof
