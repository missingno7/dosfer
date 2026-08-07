> Historical pre-RGB3 cleanup record. The current sender defaults to `/RGB3 /WINDOW:66 /RE:3`; see `README.md`, `../RGB3_IMPLEMENTATION.md` and `docs/VALIDATION.txt`.

# Production cleanup

This pass removes obsolete runtime behavior and separates production from
diagnostic code.

## Runtime cleanup

- Removed `+` / `-` hold adjustment at the end of a window.
  `/HOLD:n` is now the only transfer-rate control.
- Removed stale keyboard mappings for the already-deleted previous-window `B`
  command.
- END_WINDOW UI now shows only `Enter R M Esc`.
- Removed the unused `filtered` argument from END_WINDOW rendering.
- Removed the legacy `/SPEED:n` alias; use `/HOLD:n`.
- At that cleanup revision, the one-window default remained `/WINDOW:64`.

Detailed per-frame status strings (`TRANSFER ...`, XOR ranges, rescue details)
are developer-only now. The release build keeps only user-facing prompts that
matter operationally (initial camera-focus prompt and END_WINDOW controls).
This removes per-frame `sprintf()`/font work from the production hot path.

## Build separation

The normal build is now the release build. Developer-only calibration,
benchmarking, VGA readback/hash checks, and profiling are compile-time gated by
`DOSFER_DEVTOOLS` / `DOSFER_PROFILE`.

This keeps the production executable focused on the actual optical-transfer
path while retaining the diagnostic tools in separate developer binaries.
