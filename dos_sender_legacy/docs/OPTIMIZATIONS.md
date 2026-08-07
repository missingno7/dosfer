# Optimization pass summary

This pass intentionally stays on the 16-bit legacy/Open Watcom architecture.

## Implemented

1. **Sender fixed to V40-L**
   - removed runtime ECC selection and `/ECC`
   - normal sender path no longer calls generic QR version/ECC dispatch

2. **Dedicated V40-L frame encoder and fused steady-state emitter**
   - fixed 2956 data-codeword capacity
   - fixed 25-block V40-L RS layout
   - retains proven `dosferRs30PairAsm()`
   - recurring frames compute 750 block-major ECC bytes and emit final
     interleaved values directly into one persistent current-codeword buffer
   - codeword differences update the RAM shadow raster during that same pass
   - canonical matrix path retained only when a full redraw is required

3. **Faster full DATA steady-state path**
   - 2904-byte DATA payloads build the 2952-byte transport frame directly in
     the prepacked V40 workspace
   - this now also applies to repeated/rescue DATA frames after delta mode is
     established

4. **Single transferable packed VGA delta map**
   - old bootstrap peak: 59.3 KiB byte map + 29.6 KiB mask map + 59.3 KiB VGA map
   - new bootstrap/steady map: one 59.3 KiB u16 module map converted in place
   - peak map-memory reduction: about 88.9 KiB
   - one less far-memory access per changed module

5. **Status upload suppression**
   - normal production DATA/XOR frames do not redraw status text
   - the 320-byte status area is copied only when a prompt is added or cleared
   - focus and END_WINDOW controls remain visible

6. **Retrace-aware HOLD scheduling**
   - hidden page is rendered/uploaded before the hold deadline
   - page flips on the first retrace at-or-after the absolute deadline
   - avoids `HOLD + next retrace` behavior
   - `/HOLD:50` at ~59.94 Hz naturally targets three refreshes (~50.05 ms)

7. **Direct CRTC page flips and persistent write setup**
   - BIOS AH=05h is used only at VGA startup to discover page 0/1 CRTC start
     values
   - the measured values are written directly to CRTC 0Ch/0Dh during streaming
   - planar write mode/map mask are configured once at VGA entry rather than on
     every QR upload

8. **Calibration uses the real streaming path**
   - same V40-L encoder
   - same delta renderer
   - same retrace/deadline scheduling

9. **Benchmark improvement**
   - `/BENCH` now measures and reports the actual VGA refresh rate

## Deliberately not implemented

- no 32-bit/DOS4GW port
- no new delta-first Reed-Solomon mathematics
- no PLANE4 protocol redesign
- no speculative block-parity codeword algebra
- no change to DOSfer wire protocol, CRC or whitening

These were excluded because they would mix architectural risk into a branch
whose purpose is a fast but dependable legacy sender.

## Single-window replay storage

The sender now keeps only one 64-frame `Window`. The current window remains
fully replayable (`R`) and selectively recoverable (`M`) until Enter commits it.
After Enter, `producer_fill_window()` reuses the same far payload allocations for
the next window, so no second 64-frame replay buffer is needed. The removed `B`
command was the only feature that required retaining the previous window.
