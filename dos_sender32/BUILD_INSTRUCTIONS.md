# Building DOSFER32 r24

## Open Watcom / DOS4GW build

From a Windows command prompt:

```bat
cd dos_sender32
set WATCOM=C:\WATCOM
build32.bat
```

The script also recognizes `..\tools\watcom` and `C:\tmp\watcom` when
`WATCOM` is not set. It deletes stale executables, maps and object files before
building, so an old `DOSFER32.EXE` cannot be mistaken for the new source.

Equivalent compiler invocation:

```bat
wcl386 -q -bt=dos -mf -3s -ot -ol -oi -or -oh -za99 -s ^
  -DDOSFER32 -DDOSFER_RS30_FORCE_C -Ithird_party ^
  -l=dos4g -fe=build\DOSFER32.EXE -fm=build\DOSFER32.MAP ^
  main.c platform_vga.c protocol32.c timing32.c third_party\qrcodegen.c
```

`DOSFER_RS30_FORCE_C` selects the optimized flat 32-bit two-byte RS recurrence.
It does not select the old scalar byte-at-a-time implementation.

Keep `DOS4GW.EXE` next to `DOSFER32.EXE` when running it.

## Host tests before a DOS benchmark

```bash
./run_host_tests.sh
```

For the exhaustive window/EOF boundary matrix:

```bash
./run_host_matrix.sh
```

The exhaustive matrix currently runs 840 end-to-end cases across both PLANE
widths, record-boundary sizes and window sizes from 4 through 128.

## Required DOS benchmark

Use the same input and emulator configuration when comparing builds. A useful
baseline command is:

```bat
DOSFER32.EXE TEST.ZIP /RE:PLANE4 /WINDOW:32 /HOLD:0 /NOFOCUS
```

For a fixed-3000-cycle CPU benchmark, record at least:

- build ID and executable SHA-256
- emulator core, CPU type and fixed cycles
- `DOSFER32.PRO`
- displayed symbols/s
- useful file bytes/s
- producer starvation and underrun duplicates
- max protocol, encode, delta, upload and correction step times

Do not compare a `/VERIFY`, `/FULLVGA` or `/DUMP` run with a normal run. Those
modes intentionally add readback, full-page transfers or disk output.
