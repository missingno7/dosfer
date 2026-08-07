# DOSfer protocol v1 (authoritative)

All multi-byte integers are **unsigned, big-endian**. Byte offsets below are
zero-based. Receivers must reject an unknown version, invalid length, CRC,
reserved value, or magic before using any other field.

## QR frame (`DQR1`)

Every QR contains one binary byte-mode segment: a 48-byte header followed by
`payload_length` bytes. No Base64 or character conversion is used.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII magic `DQR1` |
| 4 | 1 | protocol version, `1` |
| 5 | 1 | kind: `1` DATA, `2` END_WINDOW, `3` CALIBRATION, `4` CHAIN_XOR, `5` BLOCK_XOR, `6` PLANE_CODED |
| 6 | 2 | flags (bit 0 repeated, bit 2 paired whitening, bit 3 payload whitening, bit 4 plane-group whitening, bit 5 strided DATA-group XOR whitening; other bits zero) |
| 8 | 4 | session ID (non-zero random/time-derived value) |
| 12 | 4 | zero-based window ID |
| 16 | 4 | zero-based global DATA frame index |
| 20 | 2 | zero-based index in window |
| 22 | 2 | number of unique DATA frames in window (1..256) |
| 24 | 4 | stream/file ID (`0` for non-file records) |
| 28 | 4 | file offset for FILE_DATA; record ID otherwise |
| 32 | 2 | payload length |
| 34 | 2 | header length, exactly `48` |
| 36 | 4 | IEEE CRC-32 of payload |
| 40 | 4 | IEEE CRC-32 of header with bytes 40..43 set to zero |
| 44 | 4 | reserved, zero |

`END_WINDOW` has no payload and repeats while the sender waits. Its window
fields describe the just-sent window. It is informational: only valid DATA
frames count toward completion. A replay uses identical indices and bytes; the
repeated flag may differ, so receivers deduplicate by `(session, global index)`
and verify that a duplicate's payload is identical.

When flag bit 3 is set, the wire payload is XOR-whitened using the session and
global frame index as described below. Payload CRC is calculated over these
transmitted (whitened) bytes. The receiver validates both CRCs before reversing
the whitening and parsing the `DQRC` record. This removes long visual patterns
from arbitrary low-entropy files without changing payload size.

Whitening seeds a 32-bit unsigned state as
`session XOR (global_index * 0x9E3779B9) XOR 0xD05FE123` (all arithmetic modulo
2^32; replace a zero seed with `0xA5A5A5A5`). Repeatedly apply xorshift32
(`s ^= s << 13; s ^= s >> 17; s ^= s << 5`) and XOR the low-to-high four bytes
of each resulting state with the next four payload bytes. Applying it twice
restores the original payload.

The CRC is the reflected IEEE polynomial `0xEDB88320`, initial/final XOR
`0xFFFFFFFF` (the common ZIP/zlib CRC-32).

`CHAIN_XOR` protects two adjacent DATA frames in the same window. Its payload
is the XOR of their complete, dewhitened `DQRC` payloads, padding the shorter
one with zero bytes. `global_index` and `window_index` identify the left frame;
the right indices are each one greater. `stream_id` packs the left payload
length in its high 16 bits and right length in its low 16 bits. `window_count`
still counts DATA frames only. Bit 2 uses the XOR of the left and right DATA
whitening streams. For equal-length frames this makes the transmitted chain
payload exactly the XOR of the two transmitted DATA payloads, while decoding
still yields the XOR of their plain payloads. CRCs always cover transmitted
bytes. If either adjacent DATA payload is known, a
receiver XORs it with the equation, truncates to the missing length, validates
the recovered `DQRC` CRC, and may continue peeling forward or backward.

`BLOCK_XOR` carries one XOR equation over DATA frames in the same window. Its
payload is the XOR of complete, dewhitened `DQRC` payloads, with shorter members
zero-padded to the longest member. `global_index` and `window_index` identify
the first member, `stream_id` contains the member count, and `window_count`
still counts DATA frames only.

The ordinary representation uses flag bit 3, requires `stream_offset == 0`, and
protects contiguous members at offsets `0,1,...,stream_id-1`. The equation is
whitened using the first member's normal DATA whitening stream.

The optimized RGB3 representation uses flag bit 5. It permits one through three
members and stores their positive logical stride in `stream_offset`. Member `i`
is therefore identified by:

```
global_index + i * stream_offset
window_index + i * stream_offset
```

The wire equation is whitened with the XOR of those members' DATA whitening
streams. For equal-length members, its transmitted payload is byte-for-byte the
XOR of the corresponding transmitted DATA payloads. This permits the DOS sender
to derive the parity QR from already encoded V40-L codeword streams and patch
only the DOSfer header plus the affected first Reed-Solomon block.

The legacy RGB3 default `/RE:3` uses stride 3 across three successive physical
DATA images. Given `[D0,D1,D2]`, `[D3,D4,D5]`, and `[D6,D7,D8]`, the following
physical parity image carries:

```
red:   D0 XOR D3 XOR D6
green: D1 XOR D4 XOR D7
blue:  D2 XOR D5 XOR D8
```

Thus loss of any one physical DATA image leaves exactly one unknown in each of
three independent equations. A final incomplete set uses valid one- or
two-member equations with the same stride; repeated colour channels used only
for optical tuple padding do not become extra equation members.

When exactly one member of any `BLOCK_XOR` equation is absent, a receiver XORs
every available member into the equation. It reads the missing record's true
length from the recovered `DQRC` header, requires all remaining padding bytes
to be zero, truncates to that length, and validates the normal record CRC.

`PLANE_CODED` is the separate experimental four-plane equation format used by
the protected-mode sender. Flag bit 4 selects its group whitening. Its detailed
coefficient and grouping rules are defined in [PLANE_XOR.md](PLANE_XOR.md).

## Container record (`DQRC`)

Each DATA payload contains exactly one whole record. This deliberate v1 rule
makes every QR independently addressable and eliminates cross-frame parser
state. The 24-byte record header is:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII magic `DQRC` |
| 4 | 1 | container version, `1` |
| 5 | 1 | type (table below) |
| 6 | 2 | flags, zero in v1 |
| 8 | 4 | monotonically increasing record ID |
| 12 | 4 | file ID, or zero |
| 16 | 4 | body length |
| 20 | 4 | IEEE CRC-32 of body |

Types and bodies:

| Type | Name | Body |
|---:|---|---|
| 1 | SESSION | `created_u32, root_len_u16, root_utf8` |
| 2 | DIRECTORY | metadata prefix + path |
| 3 | FILE_BEGIN | metadata prefix + `size_u32, path` |
| 4 | FILE_DATA | `offset_u32, data...` |
| 5 | FILE_END | `size_u32, file_crc32_u32` |
| 6 | TRANSFER_END | `file_count_u32, dir_count_u32, bytes_hi_u32, bytes_lo_u32` |

The DIRECTORY metadata prefix is `attributes_u8, reserved_u8, dos_date_u16,
dos_time_u16, path_len_u16`. FILE_BEGIN uses the same first 8 bytes, followed by
`size_u32` and `path_len` path bytes. Paths use UTF-8; the DOS sender emits the
ASCII subset and `/` separators. File-data records for one file are ordered and
non-overlapping. FILE_END CRC covers only file bytes.

## Path safety and commit rules

Android rejects empty paths, NUL/control characters, backslashes, absolute
paths, drive-letter prefixes, empty/`.`/`..` components, components over 255
UTF-8 bytes, or a total path over 1024 bytes. It creates a uniquely named
`.partial` document, streams into it, verifies size and CRC, then renames it.
Existing files are never silently replaced; a numeric suffix is chosen.

## RGB3 optical multiplex

RGB3 does not change the `DQR1` wire format or the QR standard. One physical
Mode-0Dh image carries three complete QR symbols in independent packed
bitplanes: plane 2/red, plane 1/green, and plane 0/blue; plane 3 remains zero.
The Android receiver converts the camera crop into R, G, and B grayscale views
and runs each through the ordinary QR decoder. Every successfully decoded
channel therefore enters this protocol layer as a normal `DQR1` frame.

Duplicate frame IDs are collapsed. A legacy monochrome image naturally decodes
to the same ID in all three channels, while an RGB3 data image normally yields
three different IDs. A short final RGB tuple may repeat its final valid frame
in unused channels.

A physical RGB3 image is homogeneous: it carries either DATA symbols or
parity/equation symbols, never an intentional DATA/DATA/PARITY mixture. The
legacy sender's default 66-DATA window therefore uses 22 physical DATA images.
`/HOLD` applies to one physical image.

## Sessions, windows, and recovery

Session IDs separate restarts. DATA indices are stable for retransmission.
Protocol receivers accept windows up to 128 DATA frames; the optimized 16-bit
legacy sender currently supports 4..66 and defaults to 66. The last window may
be shorter. The sender never advances without Enter unless continuous mode is
explicitly enabled. Receivers keep valid frames across replays/restarts, ignore
other sessions while one is active, and report missing in-window indices.
Frames may arrive out of order. Unsupported versions and all invalid frames
increment diagnostics only.

The legacy RGB3 default is `/RE:3`, with the specialized stride-3 physical-image
schedule described above. `/RE:0` disables parity. Other `/RE:n` values select
ordinary contiguous independent block equations; three equation QRs are batched
into each parity-only RGB image where possible.

`/RE:Ck` selects an even chain width from C2 through the sender's window limit,
with `k/2` smaller than the configured window size. Let `h=k/2`. For starts
`s=0,h,2h...` where `s+h < window_count`, an equation covers DATA
`s..min(s+k,window_count)-1`. C4 therefore covers `P0-3, P2-5, P4-7...`, while
C6 covers `P0-5, P3-8, P6-11...`. These overlapping equations are repeatedly
reconsidered by the receiver: whenever an equation has exactly one unknown,
that DATA frame is recovered and may make neighboring equations solvable. C2
retains the specialized `CHAIN_XOR` representation and fast DOS encoder; C4
and longer use `BLOCK_XOR`. In RGB3 output DATA and equation symbols are placed
in separate homogeneous physical phases. Manual `M` rescue remains DATA-only
and works independently of either recovery schedule.
