# Build profiles

DOSfer remains a 16-bit Open Watcom DOS program.

## Release

`build.bat` and `build_release.bat` build `build\DOSFER.EXE`.

The release profile defines `NDEBUG` and does **not** define `DOSFER_DEVTOOLS`
or `DOSFER_PROFILE`. The executable therefore contains only the transfer path:
manifest/producer logic, protocol framing, V40-L QR generation, redundancy,
VGA output, current-window replay/rescue, and timing.

Not compiled into the release executable:

- `/CAL`
- `/BENCH`
- benchmark loops
- VGA readback/hash verification helpers
- refresh-rate benchmark helper
- profiling counters
- C `assert()` checks

## Developer

`build_dev.bat` builds `build\DOSFERD.EXE` with `DOSFER_DEVTOOLS`.

This adds `/CAL`, `/BENCH` and diagnostic VGA verification helpers.

## Profiling

`build_profile.bat` builds `build\DOSFERP.EXE` with both
`DOSFER_DEVTOOLS` and `DOSFER_PROFILE`.

This is the only profile that accumulates QR/protocol/VGA timing counters.
