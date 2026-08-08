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

## DOS/Open Watcom verification and benchmarks

The Open Watcom oracles exercise the 16-bit far-memory RS and VGA paths:

```bat
tests\run_watcom_rs.bat
tests\run_watcom_rgb_vga.bat
```

The renderer benchmarks build standalone DOS executables. Run them in DOSBox-X
with `cpu cycles=3000` to compare the production direct paths against their
bit-identical delta and portable-C oracles. The RGB benchmark also reports the
run-level fallback, aligned four-stripe groups, phase-shifted groups, and their
predecoded boundary-edge loop separately. It also isolates the three-lane
groups, their production contribution-LUT kernels, and the exact C/386
function-crossing range scatter:

```bat
tests\run_watcom_bw_bench.bat
tests\run_watcom_rgb_bench.bat
```
