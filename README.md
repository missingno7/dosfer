# DOSfer

DOSfer is an offline optical file-transfer system for recovering files from an
80386 DOS PC with a VGA CRT. `DOSFER.EXE` shows a manually acknowledged stream
of binary QR frames; the Android app scans, validates, persists, and reconstructs
the files through Android's Storage Access Framework.

Start with [docs/QUICKSTART.md](docs/QUICKSTART.md). The wire format is defined
authoritatively in [protocol/PROTOCOL.md](protocol/PROTOCOL.md).

## Repository

- `dos_sender`: C89/Open Watcom sender, VGA planar renderer and benchmark
- `android_receiver`: native Java/Camera2/ZXing receiver
- `protocol`: wire-format constants and specification
- `tools`: host reference codec, vector generator and payload replay tools
- `test_vectors`: deterministic conformance fixtures
- `docs`: build, operation, calibration, performance and recovery guides

## One-command verification

```powershell
python tools\generate_vectors.py --check
python -m unittest discover -s tools\tests -v
android_receiver\gradlew.bat :app:testDebugUnitTest :app:assembleDebug
dos_sender\build.bat
```

The DOS and Android artifacts are copied to `build/artifacts` by `build.ps1`.

