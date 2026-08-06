# Reproducible builds

## DOS

Tested with Open Watcom v2 Current-build dated 2026-08-03. Set `WATCOM` and run
`dos_sender\build.bat`. The exact compiler invocation is in that file: 80386
code, 16-bit DOS large model, execution-time/loop/inlining optimization, C99
syntax, no DOS extender.
The output is `dos_sender\build\DOSFER.EXE` and its linker map.

The QR encoder is Nayuki's MIT-licensed C library. Its sources and license
notices are under `dos_sender/third_party`; `third_party/README.md` documents
DOSfer's GF(256) lookup-table and cached-divisor speed path.

## Android

Requirements: JDK 17+, Android SDK platform 36/build-tools 36, and internet for
the first dependency resolution. The wrapper pins Gradle 8.11.1, the project
pins Android Gradle Plugin 8.10.1 and the ZXing-C++ Android/JNI wrapper 3.1.0.
The scanner discovers YUV_420_888 modes through Camera2, prefers a mode that
can theoretically sustain fixed 60 FPS with a shorter side of at least 1000
pixels, and can be switched to a persisted manual camera/FPS/resolution choice
in the UI. The longer side is represented to ZXing-C++ by a centered square
crop rectangle; the Camera2 Y plane remains retained and is never copied or
converted to RGB. Runtime stats compare Image timestamps with
SENSOR_FRAME_DURATION/SENSOR_EXPOSURE_TIME and separately report sensor and
ImageReader FPS.

The decoder is specialized for DOSfer's fixed QR format: QR-only, one symbol,
no rotation/inversion/downscale/denoise search, no normal `tryHarder`, and an
early `DQR1` payload gate. After a successful full-detector read, the receiver
tracks the reported QR bounds. It enables ZXing-C++ pure mode only for a large,
centered, near-square, upright-looking tracked crop; any failure resets the
tracker and returns to the normal QR detector, with occasional `tryHarder`
recovery. The installed 3.1.0 Android wrapper exposes pure mode and result
corner positions, but not decoded QR version or a reusable perspective/module
sampling API, so exact V40 version inspection and direct 177-module reuse are
not claimed at this layer. The UI reports fast/reuse, full-detector, recovery,
and busy-drop metrics separately.

The receiver UI always uses a fixed square preview. `CameraCrop` is the single
raw-buffer crop definition shared by the ImageProxy handed to ZXing-C++ and the
TextureView transform, so a 1920x1080 stream displays only its centered
1080x1080 region. The full capture buffer remains the SurfaceTexture buffer;
only the view-to-buffer transform selects the crop. Preview diagnostics report
the capture size, crop coordinates, sensor/display rotation, relative rotation,
and uniform scale.

```powershell
cd android_receiver
.\gradlew.bat :app:testDebugUnitTest :app:assembleDebug :app:assembleRelease
```

The installable debug APK is under `app/build/outputs/apk/debug`.
The optimized release APK is under `app/build/outputs/apk/release`, but remains
unsigned until a project-specific Android signing key is supplied.

## Everything

Set `WATCOM`, then run `build.ps1`. It verifies deterministic vectors, runs host
and Android unit tests, and copies both binaries to `build/artifacts`.
