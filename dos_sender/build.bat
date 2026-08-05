@echo off
setlocal
if "%WATCOM%"=="" (
  if exist "C:\tmp\watcom\binnt64\wcl.exe" set WATCOM=C:\tmp\watcom
  if exist "C:\tmp\watcom\binnt\wcl.exe" set WATCOM=C:\tmp\watcom
)
if "%WATCOM%"=="" set WATCOM=C:\WATCOM
if not exist "%WATCOM%\binnt64\wcl.exe" if not exist "%WATCOM%\binnt\wcl.exe" (
  echo Open Watcom not found. Set WATCOM to its installation directory.
  exit /b 1
)
if exist "%WATCOM%\binnt64\wcl.exe" set PATH=%WATCOM%\binnt64;%PATH%
if exist "%WATCOM%\binnt\wcl.exe" set PATH=%WATCOM%\binnt;%PATH%
set INCLUDE=%WATCOM%\h;include;third_party
if not exist build mkdir build
wcl -q -bt=dos -ml -3 -ot -ol -oi -or -oh -s -za99 -Iinclude -Ithird_party ^
  -k4096 -fe=build\DOSFER.EXE -fm=build\DOSFER.MAP ^
  src\sender.c src\protocol.c src\vga.c src\timing.c third_party\qrcodegen.c
if errorlevel 1 exit /b 1
echo Built build\DOSFER.EXE
certutil -hashfile build\DOSFER.EXE SHA256 > build\DOSFER.SHA256
type build\DOSFER.SHA256
