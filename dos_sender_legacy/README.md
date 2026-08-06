# DOSfer legacy-clean sender

This tree is the cleaned V40-only branch of the legacy DOS sender.

## Deliberately fixed scope

- QR Version 40 only (`177 x 177` modules)
- one VGA pixel per QR module
- VGA BIOS Mode `0Dh`, `320 x 200`, 16-color planar memory
- `/VIDEO:320_60` is the default
- `/VIDEO:320_70` keeps the original BIOS timing
- no 640x480 renderer
- no `/SCALE` option
- no `/V` / `/VERSION` option

The sender still supports V40 ECC L/M/Q/H. The default and optimized path is
V40-L with a 2904-byte DOSfer frame payload.

## Retained transfer features

- ordinary DATA frames
- block XOR redundancy (`/RE:n`)
- overlapping chain redundancy (`/RE:C2`, `/RE:C4`, ...)
- optional chain anchors
- window replay and selective missing-frame rescue
- fixed QR mask selection
- inversion and end-of-window beep
- calibration and benchmark modes

## Source layout

- `src/sender.c` - transfer/session orchestration, QR scheduling and recovery
- `src/producer.c` - manifest scanning, disk read-ahead and record production
- `src/sender_config.c` - all command-line parsing and configuration validation
- `src/protocol.c` - DOSfer records, frames, CRC and whitening
- `src/vga.c` - V40-only 320x200 renderer, delta redraw and page flip
- `src/timing.c` - PIT-based high-resolution timing
- `third_party/qrcodegen.*` - QR library/oracle and V40 fast helpers

The old generic 640x480/scaled renderer was removed from `vga.c`; this leaves a
single raster geometry and a single codeword-to-pixel delta mapping path.

## Build

Open Watcom is required:

```bat
build.bat
```

Profile build:

```bat
build_profile.bat
```

## Examples

Default 320x200 ~60 Hz:

```bat
DOSFER.EXE /WINDOW:32 /HOLD:50 /RE:C2 FILE.ZIP
```

Original ~70 Hz Mode 0Dh timing:

```bat
DOSFER.EXE /VIDEO:320_70 /WINDOW:32 /HOLD:50 FILE.ZIP
```
