# Reproducible builds

## DOS

The production RGB3 sender is `dos_sender_legacy`. Set `WATCOM` to an Open
Watcom 2 installation and run `dos_sender_legacy\build.bat`. The exact
compiler invocation is in `build_release.bat`: 80386 instructions, 16-bit DOS
large model, execution-time/loop/inlining optimization, C99 syntax and no DOS
extender. The output is `dos_sender_legacy\build\DOSFER.EXE` together with
its linker map.

Developer and profiling variants are built with `build_dev.bat` and
`build_profile.bat`. The portable C oracles can be run independently with
`dos_sender_legacy\tests\run_host_tests.bat` on Windows or
`dos_sender_legacy/tests/run_host_tests.sh` on a POSIX host.

The QR encoder is Nayuki's MIT-licensed C library. Its sources and license
notices are under `dos_sender_legacy/third_party`.

## Android

Requirements: JDK 17+, Android SDK platform 36/build-tools 36, and internet for
the first dependency resolution. The wrapper pins Gradle 8.11.1, the project
pins Android Gradle Plugin 8.10.1 and the ZXing-C++ Android/JNI wrapper 3.1.0.

The scanner requests `YUV_420_888`, uses the existing centered square camera
crop, and keeps two Java decode workers, each with its own native ZXing-C++
readers. While transport mode is unknown or RGB3, a worker converts the retained
crop directly into three reusable one-byte-per-pixel R/G/B grayscale buffers and
gives those buffers to three ordinary QR readers. The hot loop shares aligned
2×2 chroma samples and uses precomputed BT.601 terms; it does not allocate
Android Bitmaps, an ARGB frame, or duplicate source-buffer views per image.
After two distinct equal R/G/B frame IDs identify a legacy monochrome window,
the worker switches to the original zero-copy Camera2 Y-plane path and one QR
read per image. `END_WINDOW` re-enables mode detection.

The decoder remains QR-only, one symbol per channel, with rotation, inversion,
downscaling and denoise search disabled. A missing channel does not discard the
other valid channels; protocol-aware frame IDs de-duplicate monochrome and tail
repetition. Runtime diagnostics report physical images, channel attempts,
logical frames, RGB conversion time, sensor FPS and ImageReader FPS.

The receiver UI uses a fixed square preview. `CameraCrop` is the single raw-
buffer crop shared by decoding and the `TextureView` transform, so a 1920×1080
stream displays and decodes its centered 1080×1080 region.

```powershell
cd android_receiver
.\gradlew.bat :app:testDebugUnitTest :app:assembleDebug :app:assembleRelease
```

The installable debug APK is under `app/build/outputs/apk/debug`. The optimized
release APK is under `app/build/outputs/apk/release`, but remains unsigned until
a project-specific Android signing key is supplied.

## Everything

Install a GCC-compatible host C compiler (`gcc` by default, or set `CC`), set
`WATCOM`, then run `build.ps1`. It runs both portable RGB3/V40 sender oracles,
builds `dos_sender_legacy`, runs Android unit tests and APK builds, verifies the
deterministic protocol vectors and Python tests, and copies the resulting
binaries to `build/artifacts`.
