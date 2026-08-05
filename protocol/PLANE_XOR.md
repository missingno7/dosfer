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

## Group whitening

PLANE bases carry flag bit 4 (`PLANE_WHITENED`) and use one whitening stream
keyed by `(session, groupGlobal)`, rather than a separate stream per
coefficient.  This removes the highly regular QR produced by short records
followed by fixed padding while preserving the XOR equation.  PLANE3's parity
also has bit 4 because it is an odd three-way equation.  PLANE4's `0xF` parity
has flags `0`: the four identical basis streams cancel, so whitening it again
would make the affine correction dense.

For complete PLANE4 groups this makes

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

The Android parser reverses the group stream before record validation, and
stores/reconstructs equations in the same dewhitened payload domain.

## Storage and timing

One Mode-0Dh slot contains the four basis rasters.  The parity is not stored
or QR-encoded separately: the sender temporarily XORs its compact correction
raster into plane 3, displays all four planes with the odd-parity palette and
Color Plane Enable `0x0F`, then XORs the correction back.  Eight slots still
hold 32 independent basis frames.  A 32-data-frame window emits 40 symbols,
not 64 independent payloads.
