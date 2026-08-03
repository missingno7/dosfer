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
| 5 | 1 | kind: `1` DATA, `2` END_WINDOW, `3` CALIBRATION, `4` CHAIN_XOR |
| 6 | 2 | flags (bit 0 repeated, bit 1 continuous, bit 2 inverted, bit 3 payload whitened, other bits zero) |
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
still counts DATA frames only. The chain payload itself is normally whitened
and CRC-protected like DATA. If either adjacent DATA payload is known, a
receiver XORs it with the equation, truncates to the missing length, validates
the recovered `DQRC` CRC, and may continue peeling forward or backward.

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

## Sessions, windows, and recovery

Session IDs separate restarts. DATA indices are stable for retransmission.
Windows contain 4..64 DATA frames; the last may be shorter. The sender
never advances without Enter unless continuous mode is explicitly enabled.
Receivers keep valid frames across replays/restarts, ignore other sessions while
one is active, and report missing in-window indices. Frames may arrive out of
order. Unsupported versions and all invalid frames increment diagnostics only.

The optional chained schedule is `D0, X01, D1, X12, D2...`. Duplicate DATA
anchors periodically break long camera-loss propagation. Manual `M` rescue
remains DATA-only and therefore works with receivers that ignore kind 4.
