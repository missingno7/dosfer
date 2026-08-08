# DOSfer legacy RGB3 V40-L sender

This tree keeps the proven 16-bit Open Watcom sender architecture and extends
it with an optimized RGB3 optical transport for real 386-class DOS machines.
Every colour channel remains a complete standard QR code.

## Fixed scope

- QR Version 40-L only (`177 × 177` modules)
- one display pixel per QR module
- planar EGA/VGA BIOS Mode `0Dh`, `320 × 200`
- `/VIDEO:320_60` default; VGA receives custom ~59.94-Hz timing
- real EGA retains native Mode 0Dh timing
- maximum DOSfer frame payload: 2,904 bytes
- no custom colour barcode and no 8-bpp chunky framebuffer

## Output modes

`/RGB3` is the default:

- plane 2 = red QR
- plane 1 = green QR
- plane 0 = blue QR
- plane 3 = zero

The hidden page receives three packed 1-bpp uploads and is then flipped once.
`/BW` writes one QR to all display planes and remains wire-compatible with the
existing receiver.

The default logical window contains 66 frames, exactly 22 RGB images. If a tuple
is incomplete, the final valid logical QR is repeated in unused channels.

## Retained transfer features

- ordinary DATA frames
- block XOR redundancy (`/RE:n`), default `/RE:3`
- overlapping chain redundancy (`/RE:C2`, `/RE:C4`, ...)
- optimized affine C2 and XOR3 codeword derivation
- optional chain anchors
- current-window replay and selective missing-frame rescue
- fixed QR mask selection and rescue mask rotation
- inversion and end-of-window beep
- calibration and benchmark modes in developer builds

## Direct rendering hot paths

BW and RGB3 share one packed V40 placement map and one fixed function-pattern
raster template. Every steady image restores only the 185-by-24-byte QR strip,
then scatters the complete interleaved codeword stream directly into the shadow
raster. There is no previous-codeword comparison in the production path.

The specialized 386 BW loop handles two codewords per iteration with all eight
module operations unrolled. RGB3 preclassifies uninterrupted two-column QR
stripes once when constructing the placement map. Standard ungrouped lanes
use one contribution-table dword per R/G/B codeword to update their four
successive rows; irregular lanes retain the fixed-mask pair kernel. Together
these paths avoid per-module placement loads for 3,283 of 3,706 codewords.
Six fully aligned sets of four neighboring stripes use a
derived 8 KB lane-contribution table to construct each complete framebuffer
byte with four dword lookups instead of normalizing and transposing the bits
at runtime. Six more four-stripe regions overlap after a
two-row phase shift; phase-specific entries use the surviving nibbles from
adjacent codewords as their key, eliminating the normalization, join and
transpose together. The 48 boundary contributions are handled by a small
RGB-aware 386 loop. Three uninterrupted 0C/30/C0 lane sets are also transposed
through the same contribution table over their complete 168-row intervals;
their interrupted fourth lanes remain on the run path. Function-pattern crossings use a parameterized exact
386 codeword scatter over the original placement map. The older delta
renderers remain verification oracles.

For the optimized RGB3 `/RE:3` schedule, three successive physical DATA images
`[D0,D1,D2]`, `[D3,D4,D5]`, and `[D6,D7,D8]` produce one parity image containing
`D0^D3^D6`, `D1^D4^D7`, and `D2^D5^D8`. Each parity QR is derived from the three
already encoded QR codeword streams. The common QR prefix and Reed-Solomon
codewords combine linearly; only the DOSfer header difference and its
first-block RS correction are patched. Unequal or short groups fall back to
canonical V40-L encoding.

See the repository-level `RGB3_IMPLEMENTATION.md` for the complete pipeline and
memory budget.

## Single-window memory model

Only the current unacknowledged window is retained. `/WINDOW:66` is the default.
`R` and `M` replay or selectively rescue it until Enter commits it. The same 66
far payload allocations are then recycled in place while the END_WINDOW image
remains visible.

There is no previous-window `B` command and no runtime `+/-` timing change.
Configure cadence with `/HOLD:n` before transfer.

## Existing V40 optimizations retained

- dedicated fixed V40-L packing and block layout;
- degree-30 four-input-byte Open Watcom Reed-Solomon kernel;
- direct BW and RGB3 codeword-to-raster 386 scatter loops;
- shared fixed V40-L raster template with QR-strip-only restoration;
- one 59,296-byte placement map instead of duplicate maps;
- status-row upload suppression in release builds;
- direct CRTC page flips and retrace-aware absolute `/HOLD` deadlines;
- 32-bit sequential VGA copies on 386+.

## Build

Open Watcom is required.

```bat
build.bat          rem release DOSFER.EXE
build_dev.bat      rem DOSFERD.EXE with /CAL and /BENCH
build_profile.bat  rem DOSFERP.EXE with timing counters
```

## Examples

Default RGB3 transfer:

```bat
DOSFER.EXE FILE.ZIP
```

Explicit 50-ms physical hold:

```bat
DOSFER.EXE /RGB3 /WINDOW:66 /HOLD:50 /RE:3 FILE.ZIP
```

Legacy monochrome:

```bat
DOSFER.EXE /BW /VIDEO:320_70 /WINDOW:66 /HOLD:50 FILE.ZIP
```

## Host oracle tests

```sh
cc -O2 -std=c99 -Ithird_party tests/test_v40_stream.c \
  third_party/qrcodegen.c -o test_v40_stream
./test_v40_stream

cc -O2 -std=c99 -DDOSFER_HOST_TEST -Iinclude -Ithird_party \
  tests/test_rgb3_protocol.c src/protocol.c third_party/qrcodegen.c \
  -o test_rgb3_protocol
./test_rgb3_protocol
```
