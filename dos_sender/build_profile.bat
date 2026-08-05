@echo off
setlocal
if "%WATCOM%"=="" (
  if exist "C:\tmp\watcom\binnt64\wcl.exe" set WATCOM=C:\tmp\watcom
  if exist "C:\tmp\watcom\binnt\wcl.exe" set WATCOM=C:\tmp\watcom
)
if "%WATCOM%"=="" set WATCOM=C:\WATCOM
if "%DOSFER_PROFILE_EXE%"=="" set DOSFER_PROFILE_EXE=DOSFERP
if exist "%WATCOM%\binnt64\wcl.exe" set PATH=%WATCOM%\binnt64;%PATH%
if exist "%WATCOM%\binnt\wcl.exe" set PATH=%WATCOM%\binnt;%PATH%
set INCLUDE=%WATCOM%\h;include;third_party
if not exist build mkdir build
wcl -q -bt=dos -ml -3 -ot -ol -oi -or -oh -s -za99 -dDOSFER_PROFILE %EXTRA_CFLAGS% -Iinclude -Ithird_party ^
  -k4096 -fe=build\%DOSFER_PROFILE_EXE%.EXE -fm=build\%DOSFER_PROFILE_EXE%.MAP ^
  src\sender.c src\protocol.c src\vga.c src\timing.c third_party\qrcodegen.c
if errorlevel 1 exit /b 1
echo Built build\%DOSFER_PROFILE_EXE%.EXE
certutil -hashfile build\%DOSFER_PROFILE_EXE%.EXE SHA256
