# PLANE_CODED 4+1 parity (experimental)

`/RE:PLANE` is being built around one four-frame VGA slot:

| Coefficient | Symbol |
|---:|---|
| `0x1` | A (plane 0) |
| `0x2` | B (plane 1) |
| `0x4` | C (plane 2) |
| `0x8` | D (plane 3) |
| `0xF` | A XOR B XOR C XOR D (all planes, corrected) |

The four basis symbols are ordinary valid `DQR1` frames of kind
`PLANE_CODED = 6`.  They have the same fixed V40-L payload length and common
group identity; only their coefficient differs.  The parity has the same
layout and coefficient `0xF`.  It recovers any one missing basis payload.

## Phase 1: ordinary unwhitened bases

The initial contract intentionally uses flags `0`.  For complete equal-length
groups this makes

```text
delta = parityRaw XOR rawA XOR rawB XOR rawC XOR rawD
```

zero after byte 47.  `delta` is only the required `DQR1` header/CRC
correction.  The fixed V40-L QR encoder is affine, so

```text
Q(parity) = Q(A) XOR Q(B) XOR Q(C) XOR Q(D) XOR Q(delta)
```

The DOS benchmark proves equality of the 3,706 codewords, the resulting
Mode-0Dh planar raster, Color Plane Enable `0x0F`, and restoration of plane 3
after the temporary correction.  It does not yet claim an optical capture
test.

The receiver identifies a group by `(session, window, groupGlobal)` and keeps
the coefficient. `streamOffset` carries the explicit group width (`3` or `4`),
because basis coefficients `0x1`, `0x2`, and `0x4` are shared by both modes.
Group-global index zero is valid for the first group. It must never treat coefficient `0xF` as an ordinary file
record.  Once three bases and the parity are known it reconstructs the fourth
by XOR, validates its record and CRC, then supplies only validated bases to
the existing file reconstruction path.

## Whitening follow-up

Normal per-frame whitening is deliberately excluded from Phase 1: an even
four-way XOR cancels a common whitening stream and makes the parity correction
dense.  A later revision may add coefficient-linear whitening with a new
explicit flag after its affine and recovery tests pass.

## Storage and timing

One Mode-0Dh slot contains the four basis rasters.  The parity is not stored
or QR-encoded separately: the sender temporarily XORs its compact correction
raster into plane 3, displays all four planes with the odd-parity palette and
Color Plane Enable `0x0F`, then XORs the correction back.  Eight slots still
hold 32 independent basis frames.  A 32-data-frame window emits 40 symbols,
not 64 independent payloads.
