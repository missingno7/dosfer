# Constant-memory PLANE streaming design

## Observed baseline

The current sender is not a streaming PLANE implementation:

- `Window` owns `MAX_WINDOW` frame metadata and two instances are retained for
  current/previous replay.
- `fill_window_spooled()` generates a complete logical window before playback.
- `PlaneQueue` is a second owner of group state and correction storage.
- DGROUP is `0xF620` bytes in the current DOS map, including the `0x0800`
  stack. It has only about 2.5 KiB of headroom before the 64 KiB segment limit.

The disk read-ahead already is bounded: two 16 KiB buffers in `Producer`.
That component becomes the source for the replacement producer.

## Ownership and memory map for the first streaming version

All sizes are fixed for a V40-L payload and are independent of file length or
logical window size.

| Owner | Storage | Lifetime |
| --- | ---: | --- |
| `Producer` | 2 x 16 KiB disk read buffers | transfer |
| `StreamRecord` | one 2,904-byte far payload plus compact metadata | transfer |
| QR workspace | existing raw frame, codewords, and one 8,000-byte raster | transfer |
| `PlaneWork` | four 3,706-byte codeword buffers, header deltas, one correction workspace | one filling group |
| VGA | 8 slots x 4 planes x 8 KiB, resident video memory | transfer |
| Slot descriptors | eight compact descriptors plus compact correction patches | transfer |
| Replay spool | exact fixed-size records on disk, no RAM index | current/previous logical windows |

No `window_size * payload_size`, record array, QR matrix array, or RAM spool is
permitted. The existing `Window` type is not part of the live producer once the
first version is complete.

## State machines

### Protocol producer

```
RECORD_READY -> GROUP_ACCUMULATING -> RECORD_READY
                  | last member / EOF
                  v
             WINDOW_FINALIZING -> WINDOW_ADVANCE -> RECORD_READY
```

`StreamRecord` has `window_id`, `window_index`, `global_index`, stream/file
metadata, exact payload length, and a far payload buffer. It is overwritten
only after its QR raster was uploaded to a VGA plane.

At logical-window boundaries, a fixed set of payload-sized parity accumulators
is emitted and cleared. The first version retains the existing PLANE group
parity; it does not claim recovery beyond the equations actually transmitted.

### VGA slots

```
FREE -> FILLING -> READY -> PLAYING -> FREE
```

Only `FILLING` permits writes. `READY` is immutable. A consumer transition
always selects a `READY` slot and only changes CRTC start/Color Plane Enable.
If no next slot is ready, the current valid symbol remains selected and the
underrun counter increments.

For PLANE4 the four basis payloads are constructed serially and uploaded to
planes 0..3 of one slot. CF is derived from those planes plus the fixed base
correction and the slot's compact patch list. PLANE3 uses planes 0..2 and CPE
mask `0x07` directly.

## Scheduling

The foreground loop owns all DOS and QR work:

1. perform a due CRTC/CPE transition;
2. finish a bounded upload for a FILLING slot;
3. build one next basis payload and update parity accumulators;
4. encode/render/upload that basis;
5. refill the disk ring below its low watermark.

The timer is a timestamp source only. It does not call DOS, allocate, read a
file, build records, or generate QR/ECC data.

## Replay

Every produced basis record is appended to a fixed-record disk spool while it
is still in the one reusable payload buffer. A record position is calculated
from `(window_id, window_index)`; no in-RAM frame table is retained. At a
window boundary, rotate the current and previous spool files. This preserves
exact current/previous replay without tying memory to window size. Selective
rescue reads the requested fixed record from the spool and uses the explicit
canonical fallback; those frames are excluded from PLANE playback metrics.

## Incremental implementation order

1. Replace live `Window` ownership with one `StreamRecord` and an append-only
   current-window spool, while retaining one-slot PLANE playback for validation.
2. Introduce slot descriptors whose producer consumes `StreamRecord` directly;
   prove no write occurs to READY/PLAYING slots.
3. Enable the eight-slot circular queue and cooperative fill during holds.
4. Raise `/WINDOW` to protocol-safe values after rescue parsing is converted
   from `selected[MAX_WINDOW]` to an iterator over ranges.

The first implementation intentionally keeps the existing protocol's u16
window count limit. Larger configured values are a logical-window feature,
not a reason to allocate more RAM.

## Current implementation cut

`plane-stream-r1` implements the one-slot validation stage for production
transfer: it pre-counts manifest records, retains one reusable `PendingFrame`,
prepares a complete resident PLANE group, and uses only CRTC/CPE changes while
that group is shown. Basis symbols use group whitening; PLANE4 parity remains
unwhitened because the four common streams cancel. The display is blanked
before the temporary correction is restored, so the restore cannot appear as a
corrupted parity blink.

This cut does not claim the eight-slot producer queue or exact replay yet.
Those require the disk spool and slot-state work in steps 1-3 above; keeping
that limitation explicit is preferable to reporting generic-render timing as
plane-playback throughput.
