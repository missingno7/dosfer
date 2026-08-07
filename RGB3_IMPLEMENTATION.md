# RGB3 implementation

This revision adds an RGB3 transport to `dos_sender_legacy` and the Android
receiver without changing the QR standard. Every physical colour image still
contains three complete, independent QR Version 40-L symbols:

- EGA/VGA plane 2 / red channel: logical DOSfer frame A
- EGA/VGA plane 1 / green channel: logical DOSfer frame B
- EGA/VGA plane 0 / blue channel: logical DOSfer frame C
- EGA/VGA plane 3: always zero

The display hardware combines the three packed 1-bpp planes into one of eight
RGB colours. The phone converts the Camera2 `YUV_420_888` crop directly into
three reusable grayscale channel buffers and gives each one to ZXing-C++. The
converter shares each chroma sample across its aligned 2×2 pixel block, uses
precomputed BT.601 contribution tables and performs no per-image source-buffer
view allocation. No custom barcode, module reconstruction, ARGB bitmap, or
colour-classification table is used.

## Compatibility

`/RGB3` is the default. `/BW` retains the legacy monochrome sender.

A monochrome physical image produces the same DOSfer frame ID in R, G and B.
The receiver collapses those results to one logical frame. To avoid doing three
ZXing calls for an entire legacy window, two distinct DATA/CALIBRATION images
with identical R/G/B IDs lock the receiver to its direct Camera2 Y-plane path.
A single equal focus image does not lock the mode, so the RGB3 focus image
cannot be mistaken for a BW transfer. `END_WINDOW` resets detection for the
next window.

## Scheduling

The legacy sender defaults to a logical window of 66 frames. An RGB3 data image
carries frames `(0,1,2)`, then `(3,4,5)`, and so on, giving 22 physical data
images per full window. A short final tuple repeats its last valid logical frame
in the unused channel(s); receiver de-duplication removes those copies.

A physical image is homogeneous: it contains either three DATA frames or three
parity/equation frames. For the optimized default `/RE:3`, three successive
physical DATA images `[D0,D1,D2]`, `[D3,D4,D5]`, and `[D6,D7,D8]` are followed
by one parity image carrying `D0^D3^D6`, `D1^D4^D7`, and `D2^D5^D8`. Losing one
whole DATA image therefore removes exactly one member from each equation. A
short tail uses one- or two-member equations. `/HOLD` applies to one physical
RGB image, not to each contained logical QR.

## Sender hot path

The three QR channels share one 59,296-byte V40 placement map. Each channel has
its own persistent 3,706-byte codeword stream and packed 8,000-byte shadow
raster. The steady-state delta loop traverses every codeword and placement entry
once, computes the R/G/B changed masks together, and updates only the channel
rasters that changed.

The hidden VGA/EGA page is prepared with three sequential planar writes:

1. map mask `0x04`, copy red packed raster;
2. map mask `0x02`, copy green packed raster;
3. map mask `0x01`, copy blue packed raster;
4. one retrace wait and one CRTC page flip.

Plane 3 is cleared when each page is first initialized. VGA DAC entries are
programmed to exact black/blue/green/cyan/red/magenta/yellow/white. On a real
EGA, palette values 0..7 keep the secondary/intensity bits clear, so each off
channel remains electrically dark while the primary RGB signals provide the
three QR planes. EGA keeps its native Mode 0Dh timing; the custom 59.94-Hz CRTC
timing is applied only when VGA is detected.

## Optimized three-way parity

For each complete equal-length stride-3 equation (for example `D0`, `D3`, and
`D6`), the sender does not encode the parity QR from scratch. It XORs the three
already encoded 3,706-byte V40-L codeword streams. QR Reed-Solomon encoding is
linear over GF(256), and an odd XOR count retains the common
ECI/byte/length prefix. The sender therefore patches only:

- the 48-byte DOSfer parity header difference; and
- the corresponding degree-30 Reed-Solomon correction for the affected first
  V40-L block.

A new `FF_GROUP_XOR_WHITENED` wire flag records the equation member count and
logical stride and makes the transmitted parity payload use the XOR of those
DATA whitening streams. For equal-length members this makes the parity wire
payload byte-for-byte equal to the XOR of the transmitted DATA payloads, while
normal receiver dewhitening still yields the plain XOR equation. One- or
two-member tails and unequal groups use the canonical encoder whenever the
affine codeword shortcut's exact validity conditions are not met.

## Memory added by RGB3

Approximate steady allocations specific to RGB3 are:

- three persistent codeword streams: `3 × 3,706 = 11,118` bytes;
- three parity queue streams: `3 × 3,706 = 11,118` bytes;
- three QR workspaces: about `3 × 3,919 = 11,757` bytes;
- three packed shadow rasters: `3 × 8,000 = 24,000` bytes;
- one shared placement map: `29,648 × 2 = 59,296` bytes;
- 66 payload buffers: up to `66 × 2,904 = 191,664` bytes.

The codeword, parity and workspace channel buffers are allocated from far DOS
memory. The three 8,000-byte shadow rasters deliberately remain in near memory
for the fused random-bit update hot path; together with the 8 KiB RS step table
this makes the final Open Watcom DGROUP/linker-map check important. The
placement map is not triplicated.

## Validation

Host tests cover:

- fixed V40-L packing, ECC and sequential emission against the canonical QR
  encoder for all masks;
- persistent raster delta output against canonical matrices;
- optimized XOR3 derivation against canonical parity QR codewords;
- C/Java-compatible RGB3 whitening vectors and wire-payload XOR identity;
- Android channel de-duplication and BW/RGB3 mode detection;
- optimized direct BT.601 YUV-to-R/G/B channel conversion, including a
  deterministic matrix of odd/even crop origins, dimensions, row strides,
  pixel strides and non-zero buffer positions.

Real Open Watcom, EGA/VGA, CRT and phone-camera throughput still need to be
measured on target hardware; the source keeps profiling hooks for that step.
