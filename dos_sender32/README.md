# DOSFER32 r24

`DOSFER32` is the protected-mode 32-bit DOS sender. It displays fixed V40-L QR
symbols in VGA mode 0Dh and uses the four VGA bitplanes as resident basis
frames for PLANE3 or PLANE4 erasure groups.

The r24 rewrite restores the narrow legacy hot path while retaining resident
VGA-plane playback. The active source no longer uses the old generic
`pipeline.c` layer or a RAM queue of complete 8 KiB rasters.

## Build

Install Open Watcom v2, set `WATCOM`, and run:

```bat
cd dos_sender32
set WATCOM=C:\WATCOM
build32.bat
```

Outputs are written to `build`:

- `DOSFER32.EXE`
- `DOS4GW.EXE`
- `DOSFER32.MAP`
- `DOSFER32.SHA256`, when `certutil` is available
- `BUILD_ID.TXT`

The build uses only the self-contained sources in this directory:

```text
main.c platform_vga.c protocol32.c timing32.c third_party/qrcodegen.c
```

## Typical use

```bat
DOSFER32.EXE FILE.ZIP /RE:PLANE4 /WINDOW:32 /HOLD:50 /NOFOCUS
```

Options:

- `/RE:PLANE3` or `/RE:PLANE4`
- `/WINDOW:n`, clamped to 4–128
- `/HOLD:ms`; the default is 50 ms
- `/PARTIALVGA`, the default, writes only the 185×185 QR region
- `/FULLVGA`, diagnostic full-page uploads
- `/NOFOCUS`, skip the initial Enter prompt
- `/NORETRACE`, benchmark-only mode without retrace waits
- `/VERIFY`, expensive VGA readback and affine-parity verification
- `/DUMP[:dir]`, `/DUMPGROUPS:n`, `/DUMPEXIT`

`/HOLD:0` removes the artificial hold. With normal retrace enabled, display
changes remain limited by mode 0Dh refresh. With `/NORETRACE`, it is a producer
benchmark and not a camera-safe optical mode.

Every run writes `DOSFER32.PRO`. `/VERIFY` also writes `DOSFER32.TRC`.

## Host verification

Linux/macOS with GCC or Clang:

```bash
./run_host_tests.sh
./run_host_matrix.sh
```

Windows with MinGW-w64 GCC:

```bat
run_host_tests.bat
```

The tests cover:

- canonical protocol records and PLANE whitening
- centered matrix-to-VGA raster equivalence
- PLANE3/PLANE4 affine codeword and raster parity
- cooperative 25-block V40-L encoding against canonical interleave
- the complete producer/playback state machine with simulated VGA planes
- short windows, overlap groups, exact EOF tails, slot reuse and visible-raster
  integrity

## r24 architecture

The normal path is:

```text
record construction + fused whitening/CRC
    → cooperative fixed V40-L RS/interleave
    → legacy-style codeword delta raster
    → direct upload into a free VGA plane/slot
    → register-only C1/C2/C4/C8 playback
    → pre-applied hidden-plane correction for the parity symbol
```

Only one working raster is kept in system RAM. Prepared basis rasters are
stored directly in VGA memory. The producer is split into bounded RS and delta
steps so playback deadlines are checked before more QR work is performed.

See `FIX_REPORT.md` for the regression analysis and complete change list.
