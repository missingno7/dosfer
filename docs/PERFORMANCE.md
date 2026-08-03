# Performance report

## Measured in this build environment

- DOS binary: 93,520 bytes (Open Watcom v2, 16-bit large model).
- Host protocol tests: 5 tests complete in under 1 ms of measured test time.
- Android: 37 Gradle build/test tasks completed successfully in 21 s after
  dependency setup; all protocol unit tests passed.

These are build-host measurements, not optical throughput. A VM LCD and an
Android phone are not substitutes for the target 386/VGA/CRT combination.

## Phone-free DOSBox profiling (2026-08-03)

The new `/TURBO` path was compared at the same fixed 3000-cycle 386 setting,
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
absolute throughput is conservative. V25 is the default because it reaches the
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
| 12 (`/FPS`) | 65 | 4 | 239 B | 98 | **10.20** | 2,153 |
| 15 | 77 | 4 | 364 B | 136 | 7.35 | 2,470 |
| 20 | 97 | 4 | 618 B | 207 | 4.83 | 2,850 |
| 25 | 117 | 2 | 949 B | 254 | 3.94 | 3,625 |
| 30 | 137 | 2 | 1,322 B | 338 | 2.96 | 3,828 |
| 35 | 157 | 2 | 1,761 B | 439 | 2.28 | 3,947 |
| 40 (`/BULK`) | 177 | 2 | 2,283 B | 553 | 1.81 | **4,077** |

`/FPS` is the largest measured capacity that remains above 10 generated FPS;
`/BULK` maximizes useful bytes/s. v40 uses a 370x370 quiet-zone-inclusive
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
the v40 maximum. Approximate dynamic working memory is 88 KB for `/FPS` and
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
