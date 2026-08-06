# DOSFER32 Producer Path Audit

## Existing data flow (per PLANE4 basis frame)

1. **stream_next()** reads file bytes into `w->body`, wraps them in a
   `dos32_record()` → `w->records[0]`.
   - Copies: disk → `body` → `records[0]`
   - `dos32_record` clears the record internally.

2. **dos32_plane_frame_whitened()** builds the wire frame at `w->input + 4`,
   applying the pre-generated keystream.
   - Copies: `records[0]` payload → `w->input + 4 + DOS32_FRAME_HEADER`

3. **qr_render()** sets the ECI/Byte header at `w->input[0..3]`, then calls
   `qrcodegen_dosferEncodePrepackedV40L(w->input, out_codewords)`.
   - **FULL RS ENCODE**: processes all 2956 data bytes through 25 RS blocks.
   - **FULL INTERLEAVE**: materializes the complete 3706-byte codeword stream.

4. If `delta_ready`: **dosfer_delta32()** XORs changed codeword bits into
   `w->raster` using the `delta_entries` map.
   - This is the optimized delta-render path.

5. If `!delta_ready` (first frame): **qrcodegen_dosferBuildMatrixV40L()**
   builds the full 177×177 matrix, then **qr_raster_from_matrix()** draws
   the full raster.

6. **vga32_store_fast()** copies 8000 bytes to VGA memory for each plane.

## Violations against the target architecture

| # | Violation | Target |
|---|-----------|--------|
| 1 | Full RS encode per basis frame (`qrcodegen_dosferEncodePrepackedV40L`) | Delta-first RS: only process changed bytes |
| 2 | Full interleave materialized (3706 bytes) | No complete interleave buffer in fast path |
| 3 | Full matrix construction on first frame and when delta_ready is false | Matrix path cold after first QR |
| 4 | Full VGA upload (8000 bytes per plane) | Partial upload of QR rectangle only |
| 5 | No deadline-driven scheduling; fills entire queue before playback | 50 ms deadlines; bounded producer steps |
| 6 | `clock()` with ~55ms quantization | PIT channel-0 timer (microsecond scale) |
| 7 | Multiple buffer copies in data path | Read directly into final QR input buffer |
| 8 | CF correction still calls `qrcodegen_dosferHeaderCorrectionV40L` when header_basis not ready | Precompute all 384 header-bit corrections at startup |
| 9 | `xor_raster` accumulation and `last_basis_raster` backup/restore (removed in some paths but structurally still present) | Remove unless proven necessary |

## Target steady-state pipeline (per basis QR after the first)

```
build next_input directly (patch only changing fields)
    ↓
input_delta = next_input XOR previous_input
    ↓
for each of 25 blocks:
    feed delta bytes through fixed V40-L RS recurrence
    apply changed data bits directly to raster
    apply changed ECC bits directly to raster
    ↓
partial VGA upload of updated raster rectangle
    ↓
reuse buffers for the next symbol
```

## Incremental refactoring plan

1. **Add `previous_input` buffer** — maintain the previous 2956-byte input across frames.
2. **Implement delta-first RS** — new function that processes only changed bytes per block, applies data and ECC deltas directly to raster.
3. **Switch release path to delta-first** — keep old `qr_render` as oracle for `/VERIFY`.
4. **Remove full RS encode from release path** — release path no longer calls `qrcodegen_dosferEncodePrepackedV40L`.
5. **Direct input construction** — read file bytes directly into QR input buffer; build wire frame at `qr_input + 4`.
6. **Partial VGA upload** — upload only QR rectangle.
7. **Deadline-driven scheduling** — split producer into resumable stages; 50 ms playback deadlines.
