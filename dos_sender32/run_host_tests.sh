#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
CC=${CC:-gcc}
BUILD=.host_build
rm -rf "$BUILD"
mkdir -p "$BUILD"
CFLAGS=(-std=c99 -O2 -Wall -Wextra -Werror -Wno-unused-function
        -DDOSFER_RS30_FORCE_C -I. -Ithird_party -Ihost_tests/include)

"$CC" "${CFLAGS[@]}" protocol32.c protocol32_selftest.c -o "$BUILD/protocol32_selftest"
"$CC" "${CFLAGS[@]}" third_party/qrcodegen.c qr_raster_selftest.c -o "$BUILD/qr_raster_selftest"
"$CC" "${CFLAGS[@]}" protocol32.c third_party/qrcodegen.c qrcode_affine_selftest.c -o "$BUILD/qrcode_affine_selftest"
"$CC" "${CFLAGS[@]}" -DQRCODEGEN_TEST third_party/qrcodegen.c qrcode_incremental_selftest.c -o "$BUILD/qrcode_incremental_selftest"
"$CC" "${CFLAGS[@]}" protocol32.c third_party/qrcodegen.c host_pipeline_selftest.c -o "$BUILD/host_pipeline_selftest"

"$BUILD/protocol32_selftest"
"$BUILD/qr_raster_selftest"
"$BUILD/qrcode_affine_selftest"
"$BUILD/qrcode_incremental_selftest"

for mode in PLANE3 PLANE4; do
    "$BUILD/host_pipeline_selftest" "$mode" 0 32
    "$BUILD/host_pipeline_selftest" "$mode" 1 4
    "$BUILD/host_pipeline_selftest" "$mode" 85000 32
    "$BUILD/host_pipeline_selftest" "$mode" 131071 32
    "$BUILD/host_pipeline_selftest" "$mode" 250000 5
    "$BUILD/host_pipeline_selftest" "$mode" 250000 31
done

echo "All DOSFER32 host tests passed."
