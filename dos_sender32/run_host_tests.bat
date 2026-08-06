@echo off
setlocal EnableExtensions
pushd "%~dp0"
where gcc >nul 2>nul || (
  echo GCC was not found in PATH. Install MinGW-w64 or run run_host_tests.sh.
  popd
  exit /b 1
)
if exist .host_build rmdir /s /q .host_build
mkdir .host_build
set CFLAGS=-std=c99 -O2 -Wall -Wextra -Werror -Wno-unused-function -DDOSFER_RS30_FORCE_C -I. -Ithird_party -Ihost_tests\include

gcc %CFLAGS% protocol32.c protocol32_selftest.c -o .host_build\protocol32_selftest.exe || goto :failed
gcc %CFLAGS% third_party\qrcodegen.c qr_raster_selftest.c -o .host_build\qr_raster_selftest.exe || goto :failed
gcc %CFLAGS% protocol32.c third_party\qrcodegen.c qrcode_affine_selftest.c -o .host_build\qrcode_affine_selftest.exe || goto :failed
gcc %CFLAGS% -DQRCODEGEN_TEST third_party\qrcodegen.c qrcode_incremental_selftest.c -o .host_build\qrcode_incremental_selftest.exe || goto :failed
gcc %CFLAGS% protocol32.c third_party\qrcodegen.c host_pipeline_selftest.c -o .host_build\host_pipeline_selftest.exe || goto :failed

.host_build\protocol32_selftest.exe || goto :failed
.host_build\qr_raster_selftest.exe || goto :failed
.host_build\qrcode_affine_selftest.exe || goto :failed
.host_build\qrcode_incremental_selftest.exe || goto :failed

for %%M in (PLANE3 PLANE4) do (
  .host_build\host_pipeline_selftest.exe %%M 0 32 || goto :failed
  .host_build\host_pipeline_selftest.exe %%M 1 4 || goto :failed
  .host_build\host_pipeline_selftest.exe %%M 85000 32 || goto :failed
  .host_build\host_pipeline_selftest.exe %%M 131071 32 || goto :failed
  .host_build\host_pipeline_selftest.exe %%M 250000 5 || goto :failed
  .host_build\host_pipeline_selftest.exe %%M 250000 31 || goto :failed
)

echo All DOSFER32 host tests passed.
popd
exit /b 0

:failed
echo DOSFER32 host test failed.
popd
exit /b 1
