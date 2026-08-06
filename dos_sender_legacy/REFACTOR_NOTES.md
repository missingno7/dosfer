# Legacy sender cleanup

The cleanup intentionally removes configuration dimensions that were no longer
part of the real transfer target instead of carrying generic branches through
every hot path.

## Removed

- QR versions 5..39 and `/V` / `/VERSION`
- module scaling and `/SCALE`
- VGA 640x480 Mode 12h backend
- generic 640x480 raster builder
- 640x480 delta renderer and its selector tables/assembly
- marker rendering used only by the old high-resolution path
- second status line used only in 640x480
- stale video-mode patch/build-note files
- large historical benchmark/oracle schedule experiments that duplicated the
  actual transfer code

## Refactored

- configuration moved to `sender_config.c`
- file/manifest/disk producer moved to `producer.c`
- `Config` now contains only live sender options
- QR mask and chain-anchor settings moved into `Config` instead of globals
- VGA API is now explicitly V40/320x200 instead of passing size/scale on every
  call
- VGA state contains only the 8 KiB 320x200 shadow raster, two-page flip state
  and the V40 codeword delta map
- delta acceleration is optional; allocation failure falls back to canonical
  full redraw instead of aborting the transfer
- benchmark now measures only the current fixed-V40 pipeline

## Preserved

The wire protocol, record format, whitening, replay-window behavior and XOR
redundancy semantics are unchanged. The `third_party/qrcodegen` library remains
generic because it is also the canonical QR oracle; only the sender-facing
configuration and renderer are specialized.

## Additional cleanup validation

- protocol code was decomposed into named xorshift/seed/CRC helpers instead of
  dense one-line loops; a host-side equivalence test compared 500 randomized
  frames (plain, whitened and pair-whitened) against the pre-refactor module
  byte-for-byte
- transfer control flow was expanded into explicit branches and a single
  `enter_transfer_vga()` helper, removing ambiguous one-line statements while
  retaining the existing replay/rescue state machine
- fixed V40 constants are shared through `dosfer.h`; redundant runtime
  `qr_codeword_bytes()` plumbing was removed
- source-only syntax checks pass for all sender modules with DOS API stubs;
  a real DOS executable still needs to be built with Open Watcom
