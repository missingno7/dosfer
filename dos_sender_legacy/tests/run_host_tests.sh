#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/build_host"
CC=${CC:-cc}
CFLAGS=${CFLAGS:--O2 -std=c99 -Wall -Wextra -Wno-unused-function}

mkdir -p "$BUILD"

"$CC" $CFLAGS -I"$ROOT/third_party" \
  "$ROOT/tests/test_v40_stream.c" \
  "$ROOT/third_party/qrcodegen.c" \
  -o "$BUILD/test_v40_stream"
"$BUILD/test_v40_stream"

"$CC" $CFLAGS -DDOSFER_HOST_TEST -I"$ROOT/include" -I"$ROOT/third_party" \
  "$ROOT/tests/test_rgb3_protocol.c" \
  "$ROOT/src/protocol.c" \
  "$ROOT/third_party/qrcodegen.c" \
  -o "$BUILD/test_rgb3_protocol"
"$BUILD/test_rgb3_protocol"
