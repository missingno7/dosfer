# DOS memory report

The sender is a 16-bit large-model executable and does not load a DOS extender.
With the default V40-L/32-frame configuration, the two replay windows hold at
most about 186 KiB of payload data. Other major bounded allocations are two
16,384-byte disk buffers, the 8,000-byte 320x200 RAM page, QR/codeword buffers,
the affine C2 cache, and one 268-byte manifest entry. Code and static QR tables
are reflected in the linker map.

The default remains designed for a 640 KiB conventional-memory environment,
but actual free conventional memory depends on DOS drivers and the selected
window size. Directory size does not increase RAM: entries are streamed through
the temporary `DOSFER.$$$` manifest. Files are read through alternating 16 KiB
buffers and never loaded whole. A final target-machine `MEM /C` before/while
running remains part of the hardware checklist because DOS drivers vary.
