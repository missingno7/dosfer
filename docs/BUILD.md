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
pins Android Gradle Plugin 8.10.1 and ZXing core 3.5.3.

```powershell
cd android_receiver
.\gradlew.bat :app:testDebugUnitTest :app:assembleDebug
```

The installable debug APK is under `app/build/outputs/apk/debug`.

## Everything

Set `WATCOM`, then run `build.ps1`. It verifies deterministic vectors, runs host
and Android unit tests, and copies both binaries to `build/artifacts`.
