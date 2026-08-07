# DOS memory report

`dos_sender_legacy` remains a 16-bit Open Watcom large-model executable and does
not load a DOS extender. The current default is one 66-DATA-frame RGB3 window.
Its reusable payload buffers consume at most
`66 × 2,904 = 191,664` bytes; there is no second replay window.

Major RGB3 allocations are bounded:

- three persistent QR codeword streams: 11,118 bytes;
- three queued parity codeword streams: 11,118 bytes;
- three V40 workspaces: approximately 11.8 KiB;
- three packed 320×200 1-bpp shadow rasters: 24,000 bytes;
- one shared codeword-bit-to-raster placement map: 59,296 bytes;
- two 16 KiB producer read buffers;
- the manifest/selection state and optional C2 affine cache.

The RGB codeword/workspace/parity buffers and window payloads use far DOS
memory. The three 8,000-byte shadow rasters intentionally use near memory so
the fused placement loop can update them without three far-pointer operations;
the existing 8 KiB Reed-Solomon step table is also near. The three channels
deliberately share the placement map rather than triplicating it.

Directory size does not increase RAM: entries are streamed
through the temporary `DOSFER.$$$` manifest, and files are never loaded whole.

The design still targets a 640 KiB conventional-memory machine, but the exact
free amount depends on DOS drivers and TSRs. A final Open Watcom linker-map
check plus `MEM /C` on target hardware remains required. `/BW`, a smaller
`/WINDOW`, or disabled redundancy can be used on a tighter machine.
