# DOSFER32 buffered-pipeline repair report

This report records the repairs made on `feature/buffered-pipeline` and the
checks that can run without DOSBox, VGA hardware, or an Android camera.

## Root causes fixed

- `dos32_plane_frame_whitened()` passed `group_global`, `width`, and
  `coefficient` to `frame_header()` in a different order from the canonical
  constructor. It now uses coefficient/global, group index/count, group
  global/stream id, and PLANE width/offset exactly as the canonical path.
- Delta-first RS read and wrote `previous_codewords` as if it were the
  2,956-byte input baseline. The release path now uses `previous_input`; the
  3,706-byte buffer is explicitly debug-oracle state.
- The delta RS recurrence skipped zero deltas. It now advances all 118/119
  positions and only skips the raster toggle when the delta byte is zero.
- The two-byte RS assembly kernel used partial EAX clearing and read an
  uninitialized implicit tail. It now uses full zero extension and initializes
  the two-byte tail. The DOS32 application selects the portable C kernel
  until an assembly build is independently certified.
- Header correction is affine because the fixed QR prefix contributes
  `F(0)`. The precomputed table stores `F(bit) XOR F(0)` and starts runtime
  reconstruction from `F(0)`.
- Zero/canonical raster generation no longer overwrites the live worker
  raster. Zero and canonical rasters are written into separate storage.
- CF verification now copies the complete parity wire frame into a dedicated
  canonical QR input before encoding.
- VGA initialization uses the normal white palette background. Full upload is
  the default; rectangle upload is opt-in and readback-verified when enabled.
- The optimized batched raster write placed the first left-edge bit in the
  wrong byte mask, which produced a vertical quiet-zone artifact in captured
  QR images. The mask now uses the actual pixel offset; the host raster oracle
  compares the optimized path byte-for-byte with the reference.
- Startup focus gating is active again: C1 is shown first and playback waits
  for Enter, while `/NOFOCUS` remains available for unattended runs. The
  prompt is rasterized in the reserved rows below the QR rather than sent
  through the DOS console; the post-Enter deadline is reset so the pause
  cannot trigger an immediate burst.
- Playback is driven by a persistent one-symbol state machine. A late
  producer causes a duplicate interval and resets the next deadline one full
  interval ahead, so it cannot catch up with a burst.
- Prepared groups now retain all four completed rasters in RAM before upload.
  `pipeline.c` is compiled and the executable routes input building and V40
  worker steps through its bounded stage API.

## Host oracle results

| Check | Result |
|---|---|
| Protocol canonical vs specialized constructor, C1/C2/C4/C8/CF | PASS, all 2,952 frame bytes per case |
| Canonical raster vs optimized batched raster | PASS, 8,000 bytes |
| V40-L delta codewords vs canonical(next) XOR canonical(previous) | PASS, 989 cases, all 3,706 bytes each |
| Affine header correction | PASS, zero header, 384 basis bits, 32 random headers |
| DOS32 Open Watcom build | PASS; `pipeline.c` included |
| DOSBox-X at 3000 cycles, default path | PASS; 37 groups/148 symbols, 3.592 symbols/s, zero starvation/underrun/catch-up |
| DOSBox-X at 3000 cycles, `/HEADERBASIS` | PASS; 3.619 symbols/s, no throughput improvement over default |

The protocol mismatch in the pre-repair specialized constructor began at
header offset 16 (coefficient/global), then affected offsets 24..31
(stream-id and stream-offset), followed by the header CRC at 40..43; the
2,904-byte payload itself was not the source of that constructor mismatch.
The pre-repair batched raster oracle first differed at raster byte 160
(row 4, byte 0); the repaired path has no differing byte.

The host checks are `dos_sender32/protocol32_selftest.c`,
`dos_sender32/qr_raster_selftest.c`, `tools/v40_delta_selftest.c`, and
`tools/header_affine_selftest.c`.

## Runtime queue shape

```text
2 × 32 KiB disk buffers → 2 × InputGroup → 1 persistent V40 worker
                                      → 4 prepared RAM groups → 8 VGA slots
                                                                  ↓
                                                       one-symbol scheduler
```

## Target-emulator benchmark

DOSBox-X was available with `C:\DOSBox-X\dosbox-x.conf` configured for
`cycles = 3000`, `core = normal`, and `turbo = false`. Two unattended PLANE3
runs completed successfully with `/NOFOCUS /NORETRACE`; the profiles are kept
in `_dosbox_bench3000/DOSFER32-default.PRO` and
`_dosbox_bench3000/DOSFER32-headerbasis.PRO`. The default run measured
41.199 seconds in the program profile and 3.592 sustained symbols/s; the
full-basis run measured 40.890 seconds and 3.619 symbols/s. The remaining hot
spot is delta-first RS work (29.784 seconds of the default profile), so the
executable is genuinely CPU-bound at 3000 cycles rather than waiting on the
disk or losing frames.

This does not claim hardware VGA readback, Android acceptance, or receiver
reconstruction. Those still require `/VERIFY /DUMP` on the target emulator or
hardware and feeding the dumps to `tools/verify_dosfer32_dump.py`.
