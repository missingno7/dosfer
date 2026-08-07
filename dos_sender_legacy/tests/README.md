# Host oracle tests

Run both portable oracles from the repository root with:

```sh
dos_sender_legacy/tests/run_host_tests.sh
```

On Windows with GCC available:

```bat
dos_sender_legacy\tests\run_host_tests.bat
```

`test_v40_stream.c` validates fixed V40-L packing, ECC, sequential emission,
persistent raster deltas and optimized XOR3 codeword derivation against the
canonical QR encoder for every mask.

```sh
cc -O2 -std=c99 -Ithird_party tests/test_v40_stream.c \
  third_party/qrcodegen.c -o test_v40_stream
./test_v40_stream
```

`test_rgb3_protocol.c` validates the cross-language whitening vector, proves
that an equal-length stride-3 parity wire payload equals the XOR of the three
transmitted DATA payloads, and compares optimized XOR3 V40-L codewords with
canonical encoding. Its representative equation covers logical DATA frames
`120`, `123`, and `126`, matching the RGB3 schedule across three physical
images.

```sh
cc -O2 -std=c99 -DDOSFER_HOST_TEST -Iinclude -Ithird_party \
  tests/test_rgb3_protocol.c src/protocol.c third_party/qrcodegen.c \
  -o test_rgb3_protocol
./test_rgb3_protocol
```

The DOS release executable does not include these host tests.
