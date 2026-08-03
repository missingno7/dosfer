# DOS memory report

The sender is a 16-bit large-model executable and does not load a DOS extender.
Major bounded allocations are: VGA packed image 38,400 bytes (far heap), two
16,384-byte disk buffers, current and previous windows about 51 KiB total, QR
work/matrix buffers under 2.5 KiB for the configured maximum version, and one
268-byte manifest entry. Code and static QR tables are reflected in the linker
map. The final fixed-mask executable is 81,770 bytes.

Conservative peak working-set estimate is below 220 KiB plus DOS C runtime and
file buffers, safely below both conventional 640 KiB expectations and the 2 MiB
requirement. Directory size does not increase RAM: entries are streamed through
the temporary `DOSFER.$$$` manifest. Files are read through alternating 16 KiB
buffers and never loaded whole. A final target-machine `MEM /C` before/while
running remains part of the hardware checklist because DOS drivers vary.
