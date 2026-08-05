# DOSFER32

This is the protected-mode DOS bring-up, built with Open Watcom and DOS/4GW.
It is deliberately separate from the legacy 16-bit executable while the
platform boundary is validated.

```bat
set WATCOM=C:\WATCOM
build32.bat
```

The resulting `build\DOSFER32.EXE` is an MZ DOS executable containing an LE
protected-mode image. It enters Mode 0Dh, clears/read-verifies all eight slots
in all four planes, encodes four consecutive V40-L payloads, stores C1/C2/C4/C8
in plane 0/1/2/3, displays the five masks (including CF), and restores text
mode on exit.

`build32.bat` also copies the required `DOS4GW.EXE` runtime beside the
executable. `/NOFOCUS` disables the initial Enter pause for benchmarks. Each
run writes `DOSFER32.PRO` (8.3 filename) with preparation/QR/upload/correction
timings and `DOSFER32.TRC` with the parity transition order.

Runtime prints the build identifier and the workspace allocation. Press Enter
while the real C1 is visible to continue; Escape exits cleanly.

The flat protocol layer can also be checked on the host without DOSBox:

```bat
gcc -std=c99 -I. protocol32.c protocol32_selftest.c -o protocol32_selftest.exe
protocol32_selftest.exe
```

The self-test verifies PLANE width/group fields, group whitening round-trip and
the unwhitened parity equation payload.
