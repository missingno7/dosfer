# Optimization pass summary

This pass intentionally stays on the 16-bit legacy/Open Watcom architecture.

## Implemented

1. **Sender fixed to V40-L**
   - removed runtime ECC selection and `/ECC`
   - normal sender path no longer calls generic QR version/ECC dispatch

2. **Dedicated V40-L frame encoder**
   - fixed 2956 data-codeword capacity
   - fixed 25-block V40-L RS layout
   - retains proven `dosferRs30PairAsm()`
   - fixed-stride interleave directly into 3706 codewords
   - canonical matrix path retained only when a full redraw is required

3. **Faster full DATA steady-state path**
   - 2904-byte DATA payloads build the 2952-byte transport frame directly in
     the prepacked V40 workspace
   - this now also applies to repeated/rescue DATA frames after delta mode is
     established

4. **Packed VGA delta map**
   - old: 29648 x (u16 offset + u8 mask) ~= 88.9 KiB
   - new: 29648 x u16 packed entry = 59.3 KiB
   - one less far-memory access per changed module

5. **Retrace-aware HOLD scheduling**
   - hidden page is rendered/uploaded before the hold deadline
   - page flips on the first retrace at-or-after the absolute deadline
   - avoids `HOLD + next retrace` behavior
   - `/HOLD:50` at ~59.94 Hz naturally targets three refreshes (~50.05 ms)

6. **Direct CRTC page flips**
   - BIOS AH=05h is used only at VGA startup to discover page 0/1 CRTC start
     values
   - the measured values are written directly to CRTC 0Ch/0Dh during streaming

7. **Calibration uses the real streaming path**
   - same V40-L encoder
   - same delta renderer
   - same retrace/deadline scheduling

8. **Benchmark improvement**
   - `/BENCH` now measures and reports the actual VGA refresh rate

## Deliberately not implemented

- no 32-bit/DOS4GW port
- no new delta-first Reed-Solomon mathematics
- no PLANE4 protocol redesign
- no speculative block-parity codeword algebra
- no change to DOSfer wire protocol, CRC or whitening

These were excluded because they would mix architectural risk into a branch
whose purpose is a fast but dependable legacy sender.
