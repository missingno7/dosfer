# DOSFER32 migration report

## Selected target

The first protected-mode target uses Open Watcom `wcl386` with the DOS/4GW
link target (`-bt=dos -mf -l=dos4g`). The toolchain installed in this workspace
does not include a DOS/32A or CauseWay runtime, while it does include the
DOS/4GW startup and linker libraries. The resulting executable is a DOS MZ
program with an LE protected-mode image, not a Windows PE program. DOS/32A can
be substituted later at the linker boundary without changing the flat-memory
core.

Minimum target: 386, VGA, DOS and 2 MiB of available protected-mode memory.
The bring-up reports free memory and rejects failed workspace allocation before
entering graphics mode.

## Current platform boundary

The existing `dos_sender/src` tree is a 16-bit compatibility implementation;
its `far`, DGROUP and BIOS-page paths are intentionally not included in
DOSFER32. The new target currently has only three modules:

- `dos_sender32/main.c`: flat-memory QR/plane bring-up and ownership;
- `dos_sender32/platform_vga.c`: Mode 0Dh, ports, DAC, CRTC and plane I/O;
- `dos_sender32/../dos_sender/third_party/qrcodegen.c`: shared pure QR
  algorithm compiled with `DOSFER32`, which disables segmented copies and the
  16-bit inline assembly paths.

Protocol framing now lives in `dos_sender32/protocol32.c` and the sender uses a
single file stream plus reusable record/QR/raster workspaces. No allocation is
performed by the producer while streaming; eight prepared slot descriptors
and correction rasters form the resident queue.

## Memory map

The current proof allocates one reusable workspace for each item:

| Workspace | Size |
|---|---:|
| V40 input workspace | 3,919 bytes |
| V40 matrix buffer | 3,919 bytes |
| 320x200 packed raster | 8,000 bytes |
| VGA readback scratch | 8,000 bytes |

The stream adds a 32 KiB disk read-ahead ring, four 2,904-byte record buffers,
one parity buffer, one disk record body and eight 8,000-byte correction
buffers. The configured logical window changes metadata only; it does not
multiply these allocations.

The VGA card remains the prepared-output queue. The production design will add
a 32--64 KiB disk ring, one protocol frame buffer, one parity accumulator and
eight slot descriptors; it will not allocate `window_size * payload_size`.

## VGA access

DOS/4GW supplies a flat data selector, and the bring-up uses the conventional
linear VGA aperture at `A0000h` after entering Mode 0Dh. Every store selects
one Sequencer Map Mask, writes a complete 8,000-byte raster, then selects the
Graphics Controller read plane and compares all bytes. Color Plane Enable is
written through Attribute Controller index `12h`, with the `3DAh` flip-flop
reset before every write. Startup clears and verifies all 8 KiB slots in all
four planes.

If a target extender does not identity-map the VGA aperture, the only required
change is the platform mapping function; QR and stream modules remain flat.

## Timer and scheduler

The bring-up uses bounded polling/retrace waits. It deliberately installs no
timer ISR while the memory and VGA proof is being validated. The streaming
phase will add a polling scheduler first, then an optional protected-mode IRQ
backend once vector ownership and PIC acknowledgement are documented and
tested.

## Staged plan

1. **Bring-up (current):** enter Mode 0Dh, encode four consecutive fixed V40-L
   payloads, store/read back C1/C2/C4/C8 in one slot, select C1/C2/C4/C8/CF,
   restore text mode.
2. **Pure protocol:** completed in `protocol32.c`; the host self-test checks
   PLANE fields, group whitening and parity payload semantics.
3. **Streaming producer:** completed for a single-file constant-memory stream;
   a 32 KiB read-ahead ring, logical windows and incomplete tails are handled
   without payload arrays.
4. **Eight-slot consumer:** completed with FREE/FILLING/READY/PLAYING states;
   future groups are prepared during visible holds. Parity correction is
   applied to safe plane 0 only while the display is blank, and is restored
   only after parity is blanked; read-map selection and composed-raster
   readback are verified.
5. **Protected-mode profiling:** measure QR, RS, raster, upload, selector and
   disk costs at fixed DOSBox cycles; report starvation instead of claiming a
   rate the producer cannot sustain.
6. **Extender validation:** run the same VGA self-test on a real 386 and under
   DOSBox, then evaluate DOS/32A as a drop-in linker target.

## Risks and fallbacks

- A flat pointer at `A0000h` is validated by full per-plane readback; if the
  selected extender maps physical memory differently, isolate that mapping in
  `platform_vga.c`.
- DOS/4GW is larger than DOS/32A on some systems. The build remains a clean
  DOS/4GW baseline, with the linker target kept in one script option.
- Direct BIOS calls from protected mode may differ across extenders. The
  bring-up confines BIOS use to mode entry/exit; normal playback uses ports.
- If a 386 cannot sustain V40 preparation, the scheduler will expose queue
  starvation and inter-group gaps rather than hiding them in hold timing.
