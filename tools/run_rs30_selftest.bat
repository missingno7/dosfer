@echo off
setlocal
if not exist build mkdir build

echo === RS / QR host isolation tests (no DOSBox) ===

echo [1/2] QR raster: per-module vs reference...
gcc -DDOSFER32 -I..\dos_sender\third_party -o build\qr_raster_selftest.exe ..\dos_sender32\qr_raster_selftest.c ..\dos_sender\third_party\qrcodegen.c
if errorlevel 1 exit /b 1
build\qr_raster_selftest.exe
if errorlevel 1 exit /b 1

echo [2/2] RS30: portable encode + buggy-stale-EAX simulation...
gcc -DDOSFER32 -I..\dos_sender\third_party -o build\rs30_host_test.exe rs30_host_test.c ..\dos_sender\third_party\qrcodegen.c
if errorlevel 1 exit /b 1
build\rs30_host_test.exe
if errorlevel 1 exit /b 1

echo.
echo All host isolation tests passed.
echo Watcom asm-vs-portable check: run manually in DOS only if needed:
echo   wcl386 ... -DDOSFER_RS30_TEST -l=dos4g -fe=build\RS30TST ...
echo   (do not auto-launch DOSBox from scripts; it can hang if the test binary stalls)
