@echo off
setlocal
if "%WATCOM%"=="" set WATCOM=C:\WATCOM
if exist "%WATCOM%\binnt64\wcl.exe" set PATH=%WATCOM%\binnt64;%PATH%
if exist "%WATCOM%\binnt\wcl.exe" set PATH=%WATCOM%\binnt;%PATH%
set INCLUDE=%WATCOM%\h;include;third_party
if not exist build mkdir build
wcl -q -bt=dos -ml -3 -ot -ol -oi -or -oh -s -za99 -Iinclude -Ithird_party ^
  -fe=build\DOSFERX.EXE -fm=build\DOSFERX.MAP ^
  src\sender.c src\protocol.c src\vga.c src\timing.c third_party\qrcodegen.c
if errorlevel 1 exit /b 1
echo Built build\DOSFERX.EXE
