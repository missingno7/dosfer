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
- current-window replay and selective missing-frame rescue
- fixed QR mask selection and rescue mask rotation
- inversion and end-of-window beep
- calibration and benchmark modes in developer builds only


## Single-window memory model

The sender retains only the **current unacknowledged window** in DOS memory.
`/WINDOW:64` is the default. `R` and `M` can replay or selectively rescue the
current window until Enter is pressed. Enter commits that window; the same 64
payload buffers are then recycled in place for the next window while the last
END_WINDOW QR remains visible in VGA memory.

There is intentionally no previous-window `B` replay command. Runtime `+/-`
hold adjustment has also been removed; transfer cadence is configured only with
`/HOLD:n`. This changes the largest payload allocation from two replay windows
to one and makes one 64-frame window use approximately the same payload RAM
that two 32-frame windows used.

## Safe hot-path optimizations

The sender remains a 16-bit Open Watcom DOS program. No DOS/4GW or 32-bit port
was introduced.

- A dedicated **fixed V40-L frame encoder** replaces generic segment/version/ECC
  dispatch in the sender hot path.
- V40-L Reed-Solomon uses the existing proven degree-30 two-byte assembly
  recurrence and the fixed 25-block layout.
- After the first canonical QR, the encoder computes only the 2956 data
  codewords plus 750 block-major ECC bytes held in the unused tail of the
  existing QR workspace. A sequential emitter updates one persistent
  current-codeword stream and the RAM shadow raster directly; it no longer
  creates a second complete 3706-byte output and scans it again.
- Full 2952-byte DATA frames use the prepacked ECI/byte representation directly,
  including repeated/rescue DATA frames once delta rendering is active.
- The first-render placement cache stores one 16-bit linear module index per
  codeword bit. VGA takes that allocation and converts it in place to the final
  packed delta map, removing the old 8-bit mask table and duplicate 59 KiB map
  allocation.
- Production DATA/XOR frames with no status label skip status-row clearing and
  the associated 320-byte VGA upload; the focus and END_WINDOW prompts remain.
- `/HOLD` no longer waits for the millisecond target and then waits for another
  retrace. The next page is uploaded early and flips on the **first retrace at or
  after the requested absolute deadline**. On `320_60`, `/HOLD:50` therefore
  naturally lands on the third refresh (~50.05 ms).
- BIOS page switching is used only during VGA initialization to discover the
  adapter's actual page start addresses. Streaming flips pages by writing those
  measured CRTC start values directly, avoiding `INT 10h AH=05h` per QR.
- VGA planar write mode and the all-plane map mask are programmed once when the
  graphics mode is entered instead of repeating identical port writes for every
  hidden-page upload.
- Calibration uses the same delta renderer and retrace/deadline scheduling as a
  real transfer, so camera tuning reflects actual playback cadence.


## Production UI

The release build does not render diagnostic text under every DATA/XOR QR.
It keeps only the initial camera-focus prompt and the END_WINDOW controls.
Detailed per-frame labels remain available in developer/profile builds.
Runtime `+/-` cadence changes are removed; use `/HOLD:n` before starting.

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

Open Watcom is required.

Release build (default):

```bat
build.bat
```

Developer build with `/CAL` and `/BENCH`:

```bat
build_dev.bat
```

Profiling build:

```bat
build_profile.bat
```

See `BUILD_PROFILES.md` for the exact compile-time split. The release EXE does
not contain calibration/benchmark loops, VGA readback/hash verification, or
profiling counters.

Validation notes and host-side oracle tests are kept under `docs/` and
`tests/`.

## Examples

Default 320x200 ~60 Hz:

```bat
DOSFER.EXE /WINDOW:64 /HOLD:50 /RE:C2 FILE.ZIP
```

Original ~70 Hz Mode 0Dh timing:

```bat
DOSFER.EXE /VIDEO:320_70 /WINDOW:64 /HOLD:50 FILE.ZIP
```
