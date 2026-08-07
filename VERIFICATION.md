# RGB3 verification summary

Validated from the packaged source tree on 2026-08-07.

## Passed

- `git diff --check`: no whitespace errors.
- `dos_sender_legacy/tests/run_host_tests.sh`, run with both GCC and Clang
  under `-Wall -Wextra -Werror`:
  - fixed V40-L packing, Reed-Solomon, sequential emission and raster-delta
    oracle: PASS;
  - stride-3 RGB3 whitening, wire XOR and affine codeword derivation oracle:
    PASS;
  - both host oracles also complete cleanly under AddressSanitizer and
    UndefinedBehaviorSanitizer.
- GCC and Clang syntax checks passed for the complete release, developer and
  profiling DOS translation-unit sets using DOS API stubs. Host-only warnings
  from ignored Open Watcom `#pragma aux` bodies are expected and do not occur in
  the real Watcom build.
- `python tools/generate_vectors.py --check`: 2 deterministic vector files
  verified.
- `python -m unittest discover -s tools/tests -v`: 16 protocol tests passed.
- Android-independent Java/JVM suite: 42 tests passed, covering camera geometry,
  protocol framing/recovery, RGB3 de-duplication and mode detection, YUV-to-RGB
  channel separation, and DOSfer extraction from QR decoder output. The colour
  converter includes 200 deterministic odd/even crop-origin, dimension,
  row-stride, pixel-stride and non-zero-buffer-position cases checked against a
  scalar BT.601 reference.

The JVM suite was compiled directly with Java 21 using `--release 17` against
lightweight Android/JUnit stubs because the Gradle wrapper could not download
its first-use distribution in the packaging environment. The
Android-independent production classes were compiled unchanged. The production
`CameraScanner.java` was additionally smoke-compiled against API-shaped
Camera2, CameraX and ZXing-C++ stubs.

## Not produced or hardware-validated here

- `DOSFER.EXE`: Open Watcom was not installed in the packaging environment.
- APKs/full Android Gradle build: Gradle 8.11.1 could not be downloaded because
  `services.gradle.org` was unreachable from the environment.
- Real EGA/VGA planar readback, CRT/camera optical reliability and target 386
  throughput measurements.

Build the DOS executable with `dos_sender_legacy/build.bat` under Open Watcom 2.
Build the app with the checked-in Android Gradle wrapper once dependency access
is available. Target-hardware checks are listed in
`docs/REAL_HARDWARE_CHECKLIST.md`.
