# DOSfer

DOSfer is an offline optical file-transfer system for recovering files from an
80386 DOS PC with a VGA CRT. `DOSFER.EXE` shows a manually acknowledged stream
of binary QR frames; the Android app scans, validates, persists, and reconstructs
the files through Android's Storage Access Framework.

Release 1.3 uses the optimized V40-L path by default: 320x200 output,
2,904-byte DATA payloads, zero artificial hold, 32-frame windows, and 7+1 XOR
parity. A normal transfer is simply `DOSFER.EXE FILE.DAT`. `/RE:n` selects an
independent parity group and `/RE:Ck` an overlapping even chain width.

Start with [docs/QUICKSTART.md](docs/QUICKSTART.md). The wire format is defined
authoritatively in [protocol/PROTOCOL.md](protocol/PROTOCOL.md).

## Repository

- `dos_sender`: C89/Open Watcom sender, VGA planar renderer and benchmark
- `android_receiver`: Java/Camera2 receiver with two native ZXing-C++ workers
- `protocol`: wire-format constants and specification
- `tools`: host reference codec, vector generator and payload replay tools
- `test_vectors`: deterministic conformance fixtures
- `docs`: build, operation, calibration, performance and recovery guides

## One-command verification

```powershell
python tools\generate_vectors.py --check
python -m unittest discover -s tools\tests -v
android_receiver\gradlew.bat :app:testDebugUnitTest :app:assembleDebug :app:assembleRelease
dos_sender\build.bat
```

The DOS and Android artifacts are copied to `build/artifacts` by `build.ps1`.
