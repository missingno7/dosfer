@echo off
setlocal
set ROOT=%~dp0..
set BUILD=%ROOT%\build_host
if "%CC%"=="" set CC=gcc
if not exist "%BUILD%" mkdir "%BUILD%"

%CC% -O2 -std=c99 -Wall -Wextra -Wno-unused-function -I"%ROOT%\third_party" ^
  "%ROOT%\tests\test_v40_stream.c" "%ROOT%\third_party\qrcodegen.c" ^
  -o "%BUILD%\test_v40_stream.exe"
if errorlevel 1 exit /b 1
"%BUILD%\test_v40_stream.exe"
if errorlevel 1 exit /b 1

%CC% -O2 -std=c99 -Wall -Wextra -Wno-unused-function -DDOSFER_HOST_TEST ^
  -I"%ROOT%\include" -I"%ROOT%\third_party" ^
  "%ROOT%\tests\test_rgb3_protocol.c" "%ROOT%\src\protocol.c" ^
  "%ROOT%\third_party\qrcodegen.c" -o "%BUILD%\test_rgb3_protocol.exe"
if errorlevel 1 exit /b 1
"%BUILD%\test_rgb3_protocol.exe"
if errorlevel 1 exit /b 1

exit /b 0
