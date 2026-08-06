# Legacy V40-L optimization notes

The guiding rule for this branch is: preserve the working legacy transport and
optimize only stages that can be proved independently against the old/canonical
implementation.

## Fixed sender scope

Removed from the sender configuration and hot path:

- QR versions other than 40
- ECC M/Q/H selection; the sender is permanently V40-L
- `/ECC`
- module scaling and `/SCALE`
- VGA 640x480 Mode 12h backend
- `/V` / `/VERSION`

The generic third-party QR implementation remains available as an oracle, but it
is not used by normal sender frame encoding.

## QR encoder optimization

`qrcodegen_dosferEncodeFrameV40L()` is now the normal frame encoder. It is
byte-for-byte equivalent to the former call to
`qrcodegen_encodeBinaryAligned(... V40, ECC L ...)`, but it skips:

- segment structure construction
- version search
- ECC-level selection
- generic V40 block-layout discovery
- clearing the full generic QR buffer when only 2956 data codewords are needed

The fixed encoder uses the existing proven V40-L layout:

- 25 RS blocks
- 19 x 118 data bytes
- 6 x 119 data bytes
- 30 ECC bytes per block
- 25-byte interleave stride

The existing 16-bit `dosferRs30PairAsm()` recurrence is retained unchanged.
Only the surrounding dispatch/interleave path was specialized.

## VGA delta-map optimization

The old map stored, for every codeword bit:

- a 16-bit raster byte offset
- a separate 8-bit raster mask

That required roughly 88,944 bytes. The new map stores one 16-bit packed entry:

- low 13 bits: 0..7999 shadow-raster byte offset
- high 3 bits: pixel selector 0..7

Total map size is 59,296 bytes. This also removes one far-memory lookup for every
changed module in the raster delta loop.

## Display timing optimization

Previously the transfer did:

    encode/upload
    wait until HOLD milliseconds elapsed
    wait for next vertical retrace
    BIOS page flip

At 60 Hz this can turn a nominal 50 ms hold into roughly 66.7 ms when the 50 ms
wait misses the retrace edge.

The new path does:

    encode/render
    upload hidden page immediately
    wait for first retrace at-or-after absolute visibility deadline
    direct CRTC page flip

Therefore `/HOLD:50` on the ~59.94 Hz mode normally uses exactly three refresh
periods (~50.05 ms) when the producer is ready in time.

The CRTC page-start values are not hardcoded. `vga_enter()` asks BIOS page select
for pages 0 and 1 once, reads their CRTC start values, restores page 0, and then
uses those measured values for direct streaming flips.

## Preserved behavior

Unchanged:

- record/wire format
- CRCs and whitening
- C2 affine XOR semantics
- block and chain redundancy semantics
- replay/rescue behavior
- disk producer and read-ahead model
- 16-bit large-memory Open Watcom build
