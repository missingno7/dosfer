@echo off
setlocal
if "%WATCOM%"=="" set WATCOM=C:\tmp\WATCOM
if not exist "%WATCOM%\binnt64\wcl.exe" if not exist "%WATCOM%\binnt\wcl.exe" (
  echo Open Watcom not found. Set WATCOM to its installation directory.
  exit /b 1
)
if exist "%WATCOM%\binnt64\wcl.exe" set PATH=%WATCOM%\binnt64;%PATH%
if exist "%WATCOM%\binnt\wcl.exe" set PATH=%WATCOM%\binnt;%PATH%
set INCLUDE=%WATCOM%\h;include;third_party
if not exist build mkdir build
wcl -q -bt=dos -ml -3 -ot -ol -oi -or -oh -s -za99 -Iinclude -Ithird_party ^
  -fe=build\DOSFER.EXE -fm=build\DOSFER.MAP ^
  src\sender.c src\sender_config.c src\producer.c src\protocol.c src\vga.c src\timing.c third_party\qrcodegen.c
if errorlevel 1 exit /b 1
echo Built build\DOSFER.EXE
