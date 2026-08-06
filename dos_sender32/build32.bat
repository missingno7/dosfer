@echo off
setlocal EnableExtensions
pushd "%~dp0"

if "%WATCOM%"=="" if exist "..\tools\watcom\binnt64\wcl386.exe" set "WATCOM=..\tools\watcom"
if "%WATCOM%"=="" if exist "C:\tmp\watcom\binnt64\wcl386.exe" set "WATCOM=C:\tmp\watcom"
if "%WATCOM%"=="" set "WATCOM=C:\WATCOM"

if exist "%WATCOM%\binnt64\wcl386.exe" set "PATH=%WATCOM%\binnt64;%PATH%"
if exist "%WATCOM%\binnt\wcl386.exe" set "PATH=%WATCOM%\binnt;%PATH%"
if not exist "%WATCOM%\binnt64\wcl386.exe" if not exist "%WATCOM%\binnt\wcl386.exe" (
    echo Open Watcom 32-bit compiler not found. Set WATCOM to the installation root.
    popd
    exit /b 1
)

set "INCLUDE=%WATCOM%\h;third_party"
if not exist build mkdir build

del /q build\DOSFER32.EXE build\DOSFER32.MAP build\DOSFER32.SHA256 build\BUILD_ID.TXT 2>nul
del /q *.obj third_party\*.obj 2>nul

wcl386 -q -bt=dos -mf -3s -ot -ol -oi -or -oh -za99 -s ^
  -DDOSFER32 -DDOSFER_RS30_FORCE_C -Ithird_party ^
  -l=dos4g -fe=build\DOSFER32.EXE -fm=build\DOSFER32.MAP ^
  main.c platform_vga.c protocol32.c timing32.c third_party\qrcodegen.c
if errorlevel 1 goto :failed
if not exist build\DOSFER32.EXE goto :failed

if exist "%WATCOM%\binw\dos4gw.exe" copy /y "%WATCOM%\binw\dos4gw.exe" build\DOS4GW.EXE >nul
if not exist build\DOS4GW.EXE if exist DOS4GW.EXE copy /y DOS4GW.EXE build\DOS4GW.EXE >nul

findstr /c:"DOSFER32_BUILD_ID" main.c > build\BUILD_ID.TXT
where certutil >nul 2>nul
if not errorlevel 1 certutil -hashfile build\DOSFER32.EXE SHA256 > build\DOSFER32.SHA256

del /q *.obj third_party\*.obj 2>nul
echo.
echo Built build\DOSFER32.EXE
if exist build\DOSFER32.SHA256 type build\DOSFER32.SHA256
popd
exit /b 0

:failed
echo DOSFER32 build failed.
del /q *.obj third_party\*.obj 2>nul
popd
exit /b 1
