# DOSFER32 r24 regression fix report

## Why the newer sender became slower

The previous 32-bit branch combined several architectural changes at once:
new protocol construction, a scalar delta-first RS path, a large RAM raster
queue, delayed VGA uploads and a generic pipeline scheduler. The individual
ideas looked attractive, but together they removed the optimized legacy hot
path and increased memory traffic.

The main regressions were:

1. **Delta-first RS was not sparse.** Compressed input changes nearly every
   byte, so the code still advanced a 30-byte RS state for all 2,956 data
   bytes. It replaced the optimized two-byte RS recurrence without avoiding
   meaningful work.
2. **Prepared rasters were copied twice.** Every 8 KiB basis raster was copied
   into a RAM queue and later copied again into VGA at group handoff.
3. **Producer work blocked playback.** One nominal worker step could encode and
   render a complete QR before the display deadline was checked again.
4. **Protocol handling made several full passes.** Record clearing, history
   `memmove`, whitening and CRC added unnecessary scans and copies.
5. **Partial VGA mode still transferred too much and implicitly verified.** It
   copied full 40-byte rows and triggered an 8 KiB readback.
6. **The old generic pipeline duplicated buffers and state while the active
   sender also maintained its own queue.**
7. **Late deadlines drifted.** Scheduling from `now + interval` permanently
   accumulated producer delay.
8. **Final short windows could deadlock.** A remainder smaller than the PLANE
   width could leave the last resident parity symbol waiting forever.
9. **Tail output could overwrite the currently visible slot.** The fixed tail
   slot was unsafe when that slot held the current PLANE group.
10. **The final parity raster was restored while still visible.** The final
    linger could therefore show an invalid parity image.

## Implemented repair

### QR and protocol hot path

- Restored full fixed V40-L encoding followed by codeword-delta raster updates.
- Added a cooperative 25-block encoder; the main loop processes five blocks per
  bounded step.
- Added a flat 32-bit two-input-byte RS30 recurrence equivalent to the useful
  legacy optimization.
- Preserved the specialized 25-stride V40-L interleave.
- Added bounded 512-codeword raster-delta steps.
- Fused payload copy, whitening and CRC.
- Restored slicing-by-four CRC32.
- Replaced record-history shifting with a three-entry ring.
- Removed full record pre-clears when all bytes are overwritten.

### VGA storage and playback

- Removed the 8×4×8 KiB RAM raster queue.
- Uploads each finished basis directly into a free VGA plane/slot.
- Default QR-only upload writes 24 bytes × 185 rows, or 4,440 bytes.
- `/PARTIALVGA` no longer performs a verification readback.
- VGA readback occurs only under `/VERIFY`.
- PLANE header/template correction is accumulated by VGA byte rather than by a
  quadratic patch-list search.
- Correction is applied to hidden plane 0 while C2 is visible, so the parity
  transition itself remains register-only.
- The final parity remains intact until text mode is restored.
- Tail DATA frames dynamically choose a hidden free slot instead of overwriting
  the currently visible page.

### Scheduler and queue

- Display deadlines are checked before each bounded producer step.
- Deadlines advance on an absolute timeline.
- Late work extends the current symbol instead of creating short catch-up
  flashes.
- Slot handoff is ordered by monotonic group ordinal.
- Short final groups overlap exact preceding records when possible; otherwise
  they are emitted as ordinary DATA tails without inventing records.
- Added persistent tail-pending handling to prevent the short-window deadlock.
- `/HOLD` now controls the normal playback interval.

### Correctness and diagnostics

- Correct process exit codes.
- Complete allocation checks, including record and keystream buffers.
- Actual displayed-symbol count is used in the profile instead of prepared
  group count.
- Dump filename buffers and group accounting were corrected.
- Build script deletes stale artifacts before compiling and uses only active
  sources.
- Obsolete `pipeline.c/.h` and misleading old build artifacts were removed from
  the active package.

## Verification completed

Host tests passed for:

- canonical protocol and whitening
- matrix/raster equivalence
- PLANE3 and PLANE4 affine parity, codeword-for-codeword and raster-for-raster
- cooperative encoder versus canonical V40-L interleave across 128 randomized
  cases
- full simulated VGA producer/playback scenarios
- a separate 840-case boundary matrix across both PLANE widths, many window
  sizes and EOF remainder positions
- detection that the visible VGA raster is never modified between display
  selections

A real Open Watcom DOS executable still needs to be built and benchmarked in
DOSBox-X/86Box or on the target machine. Host tests prove bit-level and state
machine correctness, not the final fixed-3000-cycle throughput.
