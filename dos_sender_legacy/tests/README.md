# Host oracle test

`test_v40_stream.c` validates the fixed V40-L packing, block-major ECC output,
sequential codeword emitter, compact placement map and persistent raster update
against the canonical full QR encoder.

Example on Linux/macOS with GCC or Clang:

```sh
cc -O2 -std=c99 -Ithird_party tests/test_v40_stream.c \
  third_party/qrcodegen.c -o test_v40_stream
./test_v40_stream
```

The DOS release build does not include this test.
