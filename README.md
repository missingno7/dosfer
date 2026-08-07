# DOSfer

DOSfer is an offline optical file-transfer system for recovering files from an
80386 DOS PC through a CRT and an Android phone. The DOS sender shows standard
binary QR frames; the Android app scans, validates, persists, repairs, and
reconstructs the original files through Android's Storage Access Framework.

## RGB3 legacy sender

`dos_sender_legacy` now defaults to **RGB3**. One physical EGA/VGA image carries
three independent standard QR Version 40-L symbols in the red, green and blue
bitplanes. The Android receiver separates the camera image into R/G/B grayscale
views and sends each view to ZXing-C++. Legacy monochrome remains available with
`/BW` and is detected automatically by three equal frame IDs.

Current legacy defaults are:

- QR V40-L, 177 × 177 modules, one display pixel per module;
- EGA/VGA Mode 0Dh, 320 × 200;
- 2,904-byte logical DATA payload;
- 66 logical frames per acknowledged window;
- `/RE:3` stride-3 RGB parity across three physical DATA images;
- `/RGB3`, with `/HOLD` applying to one physical colour image.

A normal run is:

```text
DOSFER.EXE FILE.DAT
```

Use `DOSFER.EXE /BW FILE.DAT` for the original monochrome optical path. The
architecture, bitplane mapping and optimized XOR3 derivation are documented in
[RGB3_IMPLEMENTATION.md](RGB3_IMPLEMENTATION.md).

Start with [docs/QUICKSTART.md](docs/QUICKSTART.md). The wire format is defined
in [protocol/PROTOCOL.md](protocol/PROTOCOL.md).

## Repository

- `dos_sender_legacy`: optimized 16-bit Open Watcom V40-L sender with RGB3/BW
- `android_receiver`: Java/Camera2 receiver with native ZXing-C++ workers
- `dos_sender`, `dos_sender32`: newer experimental sender implementations
- `protocol`: wire-format constants and specification
- `tools`: host reference codec, vector generation and replay tools
- `test_vectors`: deterministic conformance fixtures
- `docs`: build, operation, calibration, performance and recovery guides

## Verification

```powershell
dos_sender_legacy\tests\run_host_tests.bat
python tools\generate_vectors.py --check
python -m unittest discover -s tools\tests -v
android_receiver\gradlew.bat :app:testDebugUnitTest :app:assembleDebug

dos_sender_legacy\build.bat
```

The C host oracles under `dos_sender_legacy/tests` can also be built with a C99
host compiler without DOS or Open Watcom. The exact checks completed for this
source package and the remaining target-hardware steps are listed in
[VERIFICATION.md](VERIFICATION.md).
