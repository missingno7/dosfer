@echo off
setlocal
if "%WATCOM%"=="" if exist "..\tools\watcom\binnt64\wcl386.exe" set WATCOM=..\tools\watcom
if "%WATCOM%"=="" if exist "C:\tmp\watcom\binnt64\wcl386.exe" set WATCOM=C:\tmp\watcom
if "%WATCOM%"=="" set WATCOM=C:\WATCOM
if not exist "%WATCOM%\binnt64\wcl386.exe" if not exist "%WATCOM%\binnt\wcl386.exe" (
  echo Open Watcom 32-bit compiler not found. Set WATCOM first.
  exit /b 1
)
if exist "%WATCOM%\binnt64\wcl386.exe" set PATH=%WATCOM%\binnt64;%PATH%
if exist "%WATCOM%\binnt\wcl386.exe" set PATH=%WATCOM%\binnt;%PATH%
set INCLUDE=%WATCOM%\h;..\dos_sender\include;..\dos_sender\third_party
if not exist build mkdir build
wcl386 -q -bt=dos -mf -3s -ot -ol -oi -or -oh -za99 -s -DDOSFER32 -I..\dos_sender\third_party ^
  -l=dos4g -fe=build\DOSFER32 -fm=build\DOSFER32 ^
  main.c platform_vga.c protocol32.c timing32.c ..\dos_sender\third_party\qrcodegen.c
if errorlevel 1 exit /b 1
if exist "%WATCOM%\binw\dos4gw.exe" copy /y "%WATCOM%\binw\dos4gw.exe" build\DOS4GW.EXE >nul
certutil -hashfile build\DOSFER32.EXE SHA256 > build\DOSFER32.SHA256
type build\DOSFER32.SHA256
