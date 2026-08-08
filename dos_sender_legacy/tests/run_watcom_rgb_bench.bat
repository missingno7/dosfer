@echo off
setlocal
if "%WATCOM%"=="" set WATCOM=C:\tmp\WATCOM
if exist "%WATCOM%\binnt64\wcl.exe" set PATH=%WATCOM%\binnt64;%PATH%
if exist "%WATCOM%\binnt\wcl.exe" set PATH=%WATCOM%\binnt;%PATH%
set INCLUDE=%WATCOM%\h;include;third_party
if not exist build mkdir build
wcl -q -bt=dos -ml -3 -ot -ol -oi -or -oh -s -za99 -dNDEBUG -dDOSFER_DEVTOOLS -dDOSFER_DIRECT_BENCH -Iinclude -Ithird_party ^
  -fe=build\RGBBENCH.EXE tests\bench_watcom_rgb3.c src\protocol.c src\vga.c src\timing.c third_party\qrcodegen.c
if errorlevel 1 exit /b 1
echo Built build\RGBBENCH.EXE
