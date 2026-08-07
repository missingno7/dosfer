# Performance report

> **Historical measurements:** the results below describe earlier monochrome
> sender revisions and are retained as engineering history. The current
> `dos_sender_legacy` defaults to RGB3, `/WINDOW:66`, and `/RE:3`; new target
> hardware measurements have not yet been added. See
> [../RGB3_IMPLEMENTATION.md](../RGB3_IMPLEMENTATION.md).

## Generalized XOR parity release 1.2 (2026-08-04)

That monochrome revision defaulted to `/RE:7`, an independent seven-DATA XOR group. The
compact redundancy parameter now represents the actual equation geometry:

| Parameter | Schedule | Long-run overhead | Recovery without replay |
|---|---|---:|---|
| `/RE:15` | 15 DATA + 1 parity | 6.25% | one missing DATA per group |
| `/RE:7` | 7 DATA + 1 parity | 12.5% | one missing DATA per group |
| `/RE:3` | 3 DATA + 1 parity | 25% | one missing DATA per group |
| `/RE:C2` | adjacent pair equations | approximately 100% | peeling from a known neighbor |
| `/RE:C4` | width 4, stride 2 | approximately 50% | fixed-point peeling across overlaps |
| `/RE:C8` | width 8, stride 4 | approximately 25% | fixed-point peeling across overlaps |
| `/RE:0` | DATA only | 0% | none |

Partial groups at a window boundary also receive parity, including a final
one-DATA group. This slightly raises overhead for short windows but protects
every record. Recovery uses the existing record length and CRC, so unequal
payload lengths do not require a length table in the parity frame.

At fixed 3000 cycles, the default `/RE:7` measured 32 DATA plus five
parity displays in 4,280 ms: **8.64 displayed FPS, 7.47 DATA/s, and 21,502
file-data B/s**. The `/RE:C2` schedule remains available, but measured
only 16,802 file-data B/s because nearly half its displays are equations. A
block parity QR currently uses a full QR parity/interleave pass; optimizing a
multi-input affine parity construction is a possible future improvement.

### Mode 0Dh 32-frame bitplane store experiment (2026-08-04)

Mode 0Dh has four independent 64 KiB planes. Its 8 KiB display-page stride
gives eight slots per plane, so it can hold a complete 32-frame monochrome
window as `8 slots x 4 planes`, with frame A/B/C/D at the same offset in
planes 0/1/2/3. The Sequencer Map Mask writes one plane at a time. For
playback, the CRTC start address selects a slot and the DAC palette exposes
one selected plane, so no bitmap upload occurs between already-stored frames.

At fixed 3000 DOSBox-X cycles, the experiment wrote all 32 8,000-byte images
to VGA (256,000 bytes) in **54 ms** and read every plane/slot back exactly.
The actual CRTC page increment was verified as **8,192 bytes**. A controller-
only sequence of 24 x 32 frame selections took **114 ms** (about 6,700
selections/s); the same 32 selections synchronised to vertical retrace took
**456 ms**, or about **70.2 stable FPS**. Thus 20+ FPS burst playback is
comfortably possible once frames are staged.

This is a storage/playback mechanism, not a free sustained-throughput gain:
every QR must still be encoded, rendered, and staged once. For the normal
`/RE:7` 32-DATA window there are 37 displayed frames, so it also needs either
two playback batches or a 28-DATA window (exactly 32 displays). The current
sparse delta map remains necessary to construct QR pixels efficiently; VGA
storage can remove only the 8 KiB RAM shadow after a direct-to-plane renderer
has been proven faster and bit-exact. Therefore this experiment is retained
as a verified benchmark, not yet the default transfer path.

The corresponding four-frame batch test used four distinct, valid V40-L DATA
symbols. Its cold batch took 1,101 ms because it also built the placement map.
The relevant warm batch took **223 ms** to construct four QR codeword streams,
**200 ms** to advance the existing sparse renderer four times, and **4 ms** to
write the four 8 KiB plane images: **427 ms per four-frame batch (9.37
frames/s)**. Each stored plane read back exactly. Controller-only playback of
24 x 4 selections took 14 ms, but a 20 FPS producer would need to prepare the
next four frames in at most 200 ms. It is currently more than twice that
budget, before display holding. The batch therefore improves timing stability
and permits a fast pre-rendered burst, but cannot sustain 20 FPS or improve
long-run throughput on its own.

The fixed QR geometry is already shared in the current design: the matrix
template/placement map is built once, then reused by the sparse delta
renderer. A future four-wide renderer can avoid repeating its placement-map
walk, but it must beat the 200 ms four-frame render cost substantially; it
also cannot remove the 223 ms QR-codeword construction measured above.

#### Four-plane odd-XOR symbols: logical 64-symbol window proof

For a fixed V40-L mask, a QR raster is an affine function of its encoded
codewords. Therefore an odd XOR of complete QR rasters retains the one fixed
template, while an even XOR cancels it. One 8 KiB VGA slot can consequently
store four basis frames `A`, `B`, `C`, and `D` in planes 0..3 and display the
eight valid symbols `A`, `B`, `C`, `D`, `ABC`, `ABD`, `ACD`, and `BCD`. Across
eight slots this is **32 independent basis frames plus 32 derived equations**,
or 64 displayed symbols; it is explicitly not 64 independent DATA frames.

The DOSBox-X proof generated four distinct V40-L frames and all four
three-way combinations. Every combination had **matching QR codewords and a
bit-exact 8,000-byte canonical raster**, including function patterns, format,
version and fixed-mask geometry. The display experiment programmed the DAC
once with an odd-parity black/white palette and changed only Attribute
Controller Color Plane Enable (index 12h) plus CRTC start address. All four
Color Plane Enable masks read back correctly. Register-only playback of 24 x
32 selections dropped from 114 ms with a DAC rewrite per frame to **18 ms**;
24 x 4 selections dropped from 14 ms to **2 ms**. Vertical-retrace playback
remains bounded by the approximately 70 Hz display refresh, as expected.

This establishes the VGA half of a possible logical `/WINDOW:64` mode, but
not a protocol change yet. The present DOSfer header/CRC is not affine as a
*valid parsed record*: a three-way raw-frame XOR encodes and rasterizes
perfectly, but its existing kind, fields and checksums are not an accepted
receiver record. A new single coded-symbol wire format must make the shared
fields identical, carry a four-bit coefficient vector, use fixed lengths, and
validate the equation with affine CRC corrections. The Android receiver must
then solve the four-variable GF(2) system. The eight symbols form a systematic
binary [8,4,4] code (basis frames plus the four three-way equations), so they
provide useful erasure recovery but impose 100% displayed-symbol overhead.

The alternative template-plus-three-basis layout also has eight subsets, but
only three independent payload vectors. It offers no capacity advantage over
the four-basis odd-XOR layout and is not the preferred next implementation.

#### Rejected temporary Chain-4 upload

A temporary Chain-4 prototype was also tested rather than assumed. Its
32-bit packed store appeared fast in DOSBox-X—24 four-plane uploads took
80 ms versus 325 ms using four Map-Mask copies—but independent readback of
every plane failed for both slots 0–1 through the 64 KiB aperture and slots
2–3 through the 128 KiB aperture. In other words, the VGA-compatible path
did not treat the bytes of one 32-bit CPU store as four independent
Chain-4-addressed transactions. The prototype was removed. Even if it had
worked, Chain-4 exposes only two 8 KiB slots through a 64 KiB aperture and
four through the 128 KiB aperture, not a full 32-frame window.

## Foundation and timer audit (2026-08-04)

Before further renderer experiments, the transfer lifecycle and benchmark
instrumentation were audited. The retained changes are:

- record construction is capacity-checked before writing;
- loaded configuration is validated transactionally, including the exact QR
  data-codeword capacity and screen fit;
- source size/date/time are checked again before a file is finalized, and
  short reads are treated as transfer errors;
- both replay windows are reserved before graphics mode, so an oversized
  `/WINDOW` fails before transmission instead of halfway through it;
- cleanup is idempotent and owns all producer files, replay buffers, chain
  cache memory, and VGA restoration; and
- pacing/profiling now uses PIT channel 0 plus the BIOS tick, with a monotonic
  rollover accumulator and approximately 13.4 microsecond resolution.

The PIT implementation deliberately preserves `DS` around its BIOS Data Area
read. OpenWatcom's large-model far dereference otherwise left `DS=0040h`, which
made the profiling build corrupt the near Reed-Solomon table while the release
build happened to hide the fault. The benchmark includes a 100 ms pacing
self-test and a guarded record-builder test to prevent both regressions.

At `core=normal`, `cputype=386`, and `cycles=fixed 3000`, the corrected release
and profiling builds both complete the real 24-display DATA/XOR schedule in
2,054 ms: **85.6 ms/display, 11.68 FPS, and 16,802 file-data B/s**. All
codeword, matrix, affine-XOR, framebuffer-redraw, displayed-page, and rescue-mask
oracles match.

The higher-resolution profile makes the next bottleneck unambiguous:

| Real-schedule stage, 24 displays | Total | Per display |
|---|---:|---:|
| Protocol header/payload/CRC | 234 ms | 9.8 ms |
| QR pack + RS/interleave | 464 ms | 19.3 ms |
| RAM delta render | **1,210 ms** | **50.4 ms** |
| VGA upload/retrace/flip/status | 90 ms | 3.8 ms |

These buckets contain instrumentation overhead and some stages overlap in the
end-to-end path, so their sum is not a replacement for the 2,054 ms wall-clock
measurement. They are sufficiently separated to prioritize renderer work.

## Default V40-L fixed-3000-cycle optimization (2026-08-04)

The current 320x200 V40-L path was profiled with DOSBox-X's normal 386 core at
`cycles=fixed 3000`. The benchmark uses representative whitened DATA/XOR
traffic and no artificial hold. Its real schedule is `D0, X01, D1, X12...`,
including the same lookahead and page flipping used by a transfer.

The old steady path built every protocol frame separately, copied a 2,953-byte
raw frame through the generic QR segment packer, ran degree-30 QR RS for every
DATA and XOR symbol, toggled changed modules in a RAM shadow, uploaded the
8,000-byte hidden page, waited for retrace, and flipped it.

The optimized path uses:

- a standard ECI-3 plus Byte segment whose V40 payload starts on a byte
  boundary (2,904 protocol bytes; one byte less than the old maximum);
- a fixed four-byte segment prefix, with `make_frame()` writing directly into
  the 2,956 QR data-codeword buffer for steady full DATA frames;
- one-frame lookahead retaining two 3,706-byte encoded DATA buffers;
- paired whitening, which makes an equal-length transmitted XOR payload equal
  to the XOR of the two transmitted DATA payloads;
- affine XOR codeword derivation with only the first shortened RS correction,
  instead of a full 25-block XOR-frame RS calculation;
- header-only construction for derived XOR frames, including the affine CRC32
  identity `CRC(A xor B) = CRC(A) xor CRC(B) xor CRC(zeros)`;
- the existing two-input-byte degree-30 Reed-Solomon recurrence plus a
  register-only, constant-25-stride V40-L data/ECC interleave loop;
- the verified two-codeword-unrolled C delta renderer and double-buffered
  retrace flip; and
- full initialization of both VGA pages followed by uploads of only the QR
  rectangle and status rows; and
- direct 8x8 ROM-font status rendering into the hidden RAM page instead of one
  BIOS teletype interrupt per character.

### Before/after exclusive timing

The BIOS tick is about 54.9 ms, so stage totals are accumulated over 24 frames
and individual averages are quantized. The wall-clock result is authoritative.

| Stage per displayed frame | Reviewed baseline | Final DATA/XOR schedule |
|---|---:|---:|
| Protocol/whitening/CRC | 20.6 ms | 11.4 ms |
| QR fixed pack / affine combine | 11.4 ms | 2.3 ms |
| QR Reed-Solomon + interleave | 43.5 ms | 13.7 ms |
| RAM delta render | 54.9 ms | **52.6 ms** |
| VGA upload | ~4.5 ms | below timer resolution in this run |
| Retrace + page flip | ~11 ms | 2.3 ms in this run |
| Status draw | ~2 ms | below timer resolution |
| Measured wall clock | **141 ms, 7.04 FPS** | **2,032/24 = 84.7 ms, 11.81 FPS** |

At a 64-display schedule for a 32-DATA window (31 XOR frames plus one periodic
anchor), the final measured rate is about 5.90 DATA frames/s and
**17.0 KB/s of FILE_DATA body** before optical losses. At 70 Hz, six to seven
refreshes are exactly 100 ms, so start/end retrace phase and the 54.9 ms DOS
timer quantum make adjacent identical runs report on either side of 10.0 FPS.
A configured `/HOLD` can intentionally
cap this lower; `/HOLD:0` exposes the generated maximum.

### V40 Reed-Solomon experiments

The Reed-Solomon profile bucket includes both parity generation and QR block
interleaving. All candidates produced codewords and matrices identical to the
canonical encoder. Totals below deliberately cover many frames because one
BIOS tick is about 54.9 ms.

| Degree-30 strategy | Full encode ECC, 25 frames | DATA/XOR schedule ECC, 24 displays | Result |
|---|---:|---:|---|
| One input byte per assembly iteration | 1,153 ms | 604 ms | slower |
| Two-byte recurrence, generic interleave | 933 ms | 494 ms | previous path |
| Aligned loads plus `SHRD` sliding row | 1,263 ms | 604 ms | slower |
| Two-byte recurrence plus specialized C interleave | 823 ms | 439 ms | faster |
| Two-byte recurrence plus register-only strided assembly interleave | 768-823 ms | 329-494 ms | **selected** |

The parity recurrence itself was already efficient. The useful gain came from
V40-L's fixed layout: 25 blocks, 118 common data bytes, six extra data bytes,
and 30 parity bytes. The selected loop copies each common column directly at a
25-byte destination stride, so it avoids the generic per-codeword short-block
test and loop arithmetic. Two consecutive cleaned end-to-end runs measured
2,032 ms for 24 displays (11.81 FPS), compared with 2,197 ms (10.92 FPS)
before the interleave specialization.

### Renderer and orientation experiments

All successful RAM variants were checked against a forced canonical redraw.
The grouped and direct-page experiments are intentionally not retained in the
release source.

| Renderer strategy | Representative render time | Result |
|---|---:|---|
| Two-codeword-unrolled sparse C, separate far offset/mask | **52.6 ms** | MATCH, selected |
| Two-codeword-unrolled assembly, absolute near destination | 62.9 ms | MATCH, 10.01 FPS, slower |
| V40-L destination-byte rebuild, pair LUT + sequential stores | 125.8 ms | MATCH, 6.33 FPS, slower |
| Fixed-geometry four-row/two-column codeword tiles | 93.8 ms | MATCH, 7.04 FPS, slower |
| Two-module pair dispatch with combined `11` RMW | 59.5 ms | MATCH, 9.93 FPS, slower |
| Previous one-codeword sparse C | 54.9 ms | MATCH, 10.16 FPS |
| Prepared 320 assembler | 66 ms | MATCH, slower |
| Packed offset/selector C | 80 ms | MATCH, slower |
| Paired A/X/B with packed map and dense second delta | 87 ms | MATCH, slower |
| Codeword grouped, 256-entry contribution LUT | 93 ms | MATCH, slower |
| Codeword grouped, nibble LUT | 135 ms | MATCH, slower |
| Sequential group accumulation | 212 ms | MATCH, slower |
| Set-bit position list | 130 ms | MATCH, slower |
| Direct hidden-VGA-page RMW | ~82 ms | framebuffer FAIL, slower |
| Full canonical packed rebuild | 235-325 ms | MATCH, much slower |

| QR rotation | Stateful wall time | Delta render |
|---:|---:|---:|
| 0 degrees | 141 ms/frame | 54 ms |
| 90 degrees | 141 ms/frame | 54 ms |
| 180 degrees | 144 ms/frame | 54 ms |
| 270 degrees | 141 ms/frame | 54 ms |

Rotation did not improve the selected renderer, so the normal orientation is
kept for predictable camera framing.

The paired renderer traversed the map once for `A -> XOR` and cached a dense
`XOR -> B` bitmap. It lost because clearing and applying the bitmap plus packed
selector arithmetic cost more than a second pass through the compact sparse
loop. Processing two source bytes per sparse-loop iteration was the only new
candidate that improved the real schedule: it halves loop and far-pointer
update overhead without changing destination locality or adding RAM.

A matching two-codeword assembly candidate was retested after the PIT profiler
made sub-tick comparisons reliable. It packed the destination as an absolute
near address and removed the C loop's base-address arithmetic, but its far
offset/mask loads and dense conditional branches raised the real-schedule
render bucket from 1,210 to 1,510 ms. End-to-end time rose from 2,054 ms
(11.68 FPS) to 2,397 ms (10.01 FPS), so the experiment was reverted.

The destination-byte experiment inverted the V40 placement map and rebuilt
4,071 framebuffer bytes directly, with exactly one sequential store per byte
and no previous-frame comparison. The centered symbol lets 3,137 bytes use
four two-module lookup contributions; 934 bytes at QR/function boundaries use
an eight-bit fallback recipe. Pointer unrolling reduced its isolated redraw
from 201 to 146 ms, and a 4 KB pair-transformation LUT reduced it to 112-119 ms.
On the real DATA/XOR schedule it spent 3,020 ms rendering 24 displays and
finished in 3,789 ms (**6.33 FPS, 9,108 file-data B/s**). The sparse renderer
spent 1,208 ms rendering the same 24-display schedule and finished in 2,032 ms
(**11.81 FPS, 16,984 file-data B/s**). Both produced identical canonical,
affine, dirty-redraw, displayed-page, and rescue-mask framebuffer results.
Sequential output therefore does not compensate for reconstructing every
module through scattered codeword reads on this 386; the experiment is not
retained in the release renderer.

Two higher-level V40 experiments also tried to preserve the QR zigzag instead
of flattening it to independent modules. Pair dispatch halved the number of
bit decisions and combined a changed `11` module pair into one framebuffer
RMW when both modules shared a byte. Its extra three-way control flow still
raised real-schedule rendering from 1,208 to 1,428 ms per 24 displays and
reduced the complete rate from 11.81 to 9.93 FPS.

The more architectural tile renderer recognized **3,277 of 3,706 codewords**
as regular four-row/two-column V40 tiles. Each regular tile replaced eight
offset/mask lookups with one base offset, a fixed +40/-40 row step and four
two-bit LUT operations; only 429 codewords used sparse fallback. Despite 88%
coverage, it took 2,251 ms to render the 24-display schedule at 3000 cycles,
for **7.04 FPS and 10,135 file-data B/s**. At 6000 cycles it reached 14.10 FPS,
still behind sparse delta's 19.86 FPS. This confirms the loss is CPU-side tile
decode/dispatch rather than VGA upload, retrace, or insufficient regular-tile
coverage. Both variants matched every framebuffer and VGA-page oracle and are
not retained in the release source.

At `cycles=fixed 6000`, the selected renderer took 659 ms for 24 displays
(27.5 ms/frame) and the complete schedule reached **19.86 FPS**. The nearly
linear renderer scaling confirms that RAM delta construction remains CPU-bound;
the 70 Hz retrace becomes more visible only after the CPU work is reduced.

### Correctness and memory

The DOS benchmark reports all of the following as `MATCH`: aligned-fast versus
canonical V40 codewords and matrix, paired-whitening raw payload XOR, affine
derived versus canonical XOR codewords, affine versus canonical framebuffer,
dirty versus full redraw, displayed page versus RAM shadow, and rescue mask
round-trip. The Android paired-whitening and recovery tests pass as part of
`:app:testDebugUnitTest`.

Lookahead adds two 3,706-byte far codeword buffers and one 48-byte cached
header, approximately **7.5 KB**. The 8 KB degree-30 RS table is shared with
normal V40 encoding. No per-frame allocation occurs, and the change remains
compatible with the 640 KB conventional-memory target.

## Historical pre-RGB3 build-host measurements

The figures in this subsection describe the earlier monochrome/C2 build that
produced the profiling results above; they are retained only for comparison:

- DOS binary: 115,056 bytes (Open Watcom v2, 16-bit large model).
- Host protocol tests: 8 tests completed in about 2 ms of measured test time.
- Android: 37 Gradle build/test tasks completed successfully in 21 s after
  dependency setup, with 8 protocol unit tests in that historical rerun.

Current RGB3 source validation is summarized in `../VERIFICATION.md`. These are
build-host measurements, not optical throughput. A VM LCD and an Android phone
are not substitutes for the target 386/VGA/CRT combination.

## Phone-free DOSBox profiling (2026-08-03)

The former `/TURBO` experiment was compared at the same fixed 3000-cycle 386 setting,
ECC L, scale 2 and capacity-filled frames. It includes a specialized 386
Reed-Solomon loop, persistent QR placement state, and retrace-synchronized
partial VGA copies. Full and dirty redraw hashes matched for every candidate.

| Profile | Payload | Display FPS | Chained effective file B/s |
|---:|---:|---:|---:|
| v25-L | 1,225 B | **9.93** | 5,945 |
| v30-L | 1,684 B | 6.42 | 5,321 |
| v35-L | 2,255 B | 5.02 | 5,593 |
| v40-L | 2,905 B | 3.97 | 5,714 |

The v30-v40 figures predate the final fused framing/partial-copy pass, so their
absolute throughput is conservative. At that stage V25 was selected because it reached the
10 FPS objective and is materially easier to decode optically; v40's earlier
effective advantage was small. Chained throughput counts 32 DATA frames, 31
adjacent XOR equations, and one 16-frame anchor per window.

The current transport whitens DATA payloads before QR encoding so arbitrary
low-entropy files do not create long module runs. At 3000 cycles/ms, v40-M,
mask 0, a full 2,283-byte payload measured 32 ms for framing, CRC and whitening.
The full stateful path measured 199 ms/frame (5.02 generated FPS) and the dirty
redraw framebuffer matched a forced full redraw exactly. The DOS fixed-vector
whitening self-test passed. This is the reliability-oriented result; the older
table below predates whitening and is retained as the raw speed baseline.

`DOSFER /BENCH file [options]` profiles protocol framing/CRC, QR construction,
packed VGA expansion, retrace/copy, and status text without Android. At
DOSBox-X 3000 cycles/ms, ECC M, fixed standard mask 0, capacity-filled frames,
and zero artificial hold, the final curve is:

| Version | Modules | Scale | Protocol payload | Total ms/frame | Generated FPS | Useful file B/s |
|---:|---:|---:|---:|---:|---:|---:|
| 10 | 57 | 4 | 165 B | 86 | 11.63 | 1,593 |
| 12 (former `/FPS`) | 65 | 4 | 239 B | 98 | **10.20** | 2,153 |
| 15 | 77 | 4 | 364 B | 136 | 7.35 | 2,470 |
| 20 | 97 | 4 | 618 B | 207 | 4.83 | 2,850 |
| 25 | 117 | 2 | 949 B | 254 | 3.94 | 3,625 |
| 30 | 137 | 2 | 1,322 B | 338 | 2.96 | 3,828 |
| 35 | 157 | 2 | 1,761 B | 439 | 2.28 | 3,947 |
| 40 (former `/BULK`) | 177 | 2 | 2,283 B | 553 | 1.81 | **4,077** |

Historically, `/FPS` was the largest measured capacity above 10 generated FPS
and `/BULK` maximized useful bytes/s. Those presets are retired. v40 uses a 370x370 quiet-zone-inclusive
image in 640x480. This has the same physical horizontal module size as a
one-pixel v40 symbol in 320x200, but gives much squarer CRT pixels and avoids a
mode change.

Final instrumented QR phase totals (the timing hooks add a little overhead):

| QR phase | v15-M | v40-M |
|---|---:|---:|
| Byte-mode pack | 2 ms | 9 ms |
| Reed-Solomon + interleave | 46 ms | 279 ms |
| Masked template copy | 4 ms | 33 ms |
| Unrolled codeword placement | 18 ms | 90 ms |
| Separate mask pass | 0 ms | 0 ms |

The original v15 path took 1,353 ms for QR construction and 810 ms for VGA
image construction. The final non-instrumented path is about 70 ms and 43 ms
respectively. Improvements include direct byte packing, GF and RS feedback
tables, cached block divisors, a bit-identical masked function template,
precomputed placement maps, unrolled byte scattering, and 2/4-pixel VGA
expansion tables. Full framebuffer copy is only 15-17 ms, so dirty rectangles
are no longer the primary opportunity.

Fixed mask 0 can look conspicuously regular for repeated data and benchmark
patterns such as `A5`. Optimized v8/v15/v40 matrices were compared bit-for-bit
with the generic encoder, and a captured VGA symbol decoded independently with
ZXing. The appearance is not matrix corruption, but physical CRT reliability
still requires the long optical qualification below.

Specialized tables are allocated once at the selected version rather than at
the v40 maximum. Historical dynamic working memory was 88 KB for `/FPS` and
285 KB for `/BULK`, including two retransmission windows and the 38,400-byte
VGA back buffer. The 93.5 KB executable and 32 KB producer disk buffer are in
addition. Payload buffers are allocated once per window slot and reused; there
is no per-frame heap allocation in the display loop.

## Live optical validation (2026-08-03)

DOSBox-X 2022.09.0 at 3000 cycles/ms on an LCD was scanned by a Samsung Galaxy
S24 Ultra. The v15-M/320-byte/750-ms configuration transferred a 110-byte file
in five unique frames. Android validated 5/5 frames, 0 invalid, deduplicated
1,398 repeated decodes, reported about 20.1 decode results/s and 19.5 ms decode
latency, reconstructed through SAF, and produced a byte-identical SHA-256
`1A3868F556553CDC4F735834F1F8009878B7E53AA4871B8B7947DEBD7511EAE7`.

With the original automatic QR-mask scorer, first-to-last unique capture took
64.452 s: 532 raw QR bytes / 292 unique container-payload bytes / 110 useful
file bytes, or 8.25 / 4.53 / 1.71 B/s respectively. This isolated QR generation
as the bottleneck; Android camera/decoder headroom was ample. The sender now
uses standards-compliant fixed mask 0 so it does not score eight masks per QR.

The fixed-mask rerun captured the five frames at 12:46:43.632, 46.240, 49.048,
51.920 and 54.740: 11.108 s first-to-last. That is 47.89 raw QR B/s, 26.29
unique container-payload B/s, and 9.90 final useful file B/s, a 5.80x useful
throughput gain. It again produced 5/5 unique, 0 invalid and a byte-identical
file. The measured operator/tooling interval from first capture through SAF
commit was 41.096 s, or 2.68 useful B/s including the deliberately slow manual
acknowledgement/reconstruction steps. The receiver also correctly created
`SAMPLE (1).TXT` rather than overwriting the first run.

## Throughput worksheet

The receiver exposes the first three values; use a stopwatch around manual
acknowledgements for the fourth.

| Configuration | Raw QR payload B/s | Unique captured B/s | Useful file B/s | Including acknowledgements B/s |
|---|---:|---:|---:|---:|
| v15-M auto mask, 320 B, 750 ms, DOSBox 3000 cycles/ms | 8.25 | 4.53 | 1.71 | not representative (interactive test) |
| v15-M fixed mask 0, same setup | **47.89** | **26.29** | **9.90** | **2.68** |

Do not claim a “maximum reliable” number until a run of at least 1,000 frames
has zero unrecovered frames and the final file CRC passes. The likely baseline
raw rate is bounded near 427 record bytes/s before headers/duplicates, but that
is a configuration calculation, not a measurement.

Thus the maximum *observed reliable* result in the tested configurations is
47.89 raw / 26.29 unique payload / 9.90 useful file B/s. It is not yet the
maximum qualified for the target physical 386/CRT; the 1,000-frame real-CRT
qualification remains mandatory.

Measured stages available: DOS traversal, disk+CRC, QR encode, retrace/hold;
Android camera acquisition, decode latency/rate, unique payload rate, missing
and duplicate rate; final reconstruction size/CRC. Logging stays in memory/UI
so it does not perturb the DOS optical loop.
