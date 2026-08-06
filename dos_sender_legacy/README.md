# DOSfer legacy V40-L sender

This tree keeps the proven 16-bit legacy sender architecture and specializes it
for the actual optical-transfer target instead of carrying old generic modes.

## Fixed scope

- **QR Version 40-L only** (`177 x 177` modules)
- one VGA pixel per QR module
- VGA BIOS Mode `0Dh`, `320 x 200`, 16-color planar memory
- `/VIDEO:320_60` is the default
- `/VIDEO:320_70` keeps the original BIOS timing
- maximum DOSfer frame payload: 2904 bytes
- no 640x480 renderer
- no QR version/ECC/scale selection

## Retained transfer features

- ordinary DATA frames
- block XOR redundancy (`/RE:n`)
- overlapping chain redundancy (`/RE:C2`, `/RE:C4`, ...)
- the optimized affine C2 shortcut
- optional chain anchors
- window replay and selective missing-frame rescue
- fixed QR mask selection and rescue mask rotation
- inversion and end-of-window beep
- calibration and benchmark modes

## Safe hot-path optimizations

The sender remains a 16-bit Open Watcom DOS program. No DOS/4GW or 32-bit port
was introduced.

- A dedicated **fixed V40-L frame encoder** replaces generic segment/version/ECC
  dispatch in the sender hot path.
- V40-L RS/interleave uses the existing proven degree-30 two-byte assembly
  recurrence and fixed 25-block/25-byte-stride layout directly.
- Full 2952-byte DATA frames use the prepacked ECI/byte representation directly,
  including repeated/rescue DATA frames once delta rendering is active.
- The VGA codeword-to-pixel map is packed into one 16-bit entry per codeword bit,
  reducing it from about 89 KiB to about 59 KiB and removing one far-memory load
  per changed module.
- `/HOLD` no longer waits for the millisecond target and then waits for another
  retrace. The next page is uploaded early and flips on the **first retrace at or
  after the requested absolute deadline**. On `320_60`, `/HOLD:50` therefore
  naturally lands on the third refresh (~50.05 ms).
- BIOS page switching is used only during VGA initialization to discover the
  adapter's actual page start addresses. Streaming flips pages by writing those
  measured CRTC start values directly, avoiding `INT 10h AH=05h` per QR.
- Calibration uses the same delta renderer and retrace/deadline scheduling as a
  real transfer, so camera tuning reflects actual playback cadence.

## Source layout

- `src/sender.c` - transfer/session orchestration and redundancy scheduling
- `src/producer.c` - manifest scanning, disk read-ahead and record production
- `src/sender_config.c` - command-line parsing for the fixed V40-L sender
- `src/protocol.c` - DOSfer records, frames, CRC and whitening
- `src/vga.c` - V40 320x200 delta renderer, deadline-aware retrace and page flip
- `src/timing.c` - PIT-based high-resolution timing
- `third_party/qrcodegen.*` - canonical QR library plus fixed V40-L fast helpers

The upstream QR implementation remains in the tree as the correctness oracle,
but normal sender encoding uses only the fixed V40-L helper.

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
