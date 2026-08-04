# Known limitations

- The successful live test used DOSBox-X/LCD, not a physical 386/VGA/CRT. Its
  measured values validate the pipeline but do not qualify real-CRT maximums.
- DOS source and relative paths are limited to 127 bytes and cumulative transfer
  accounting to 4 GiB in v1. Individual FAT file sizes are 32-bit.
- Android SAF cannot portably restore DOS attributes or modification timestamps;
  they are validated/preserved in protocol metadata but not applied everywhere.
- Previous-window replay retains one prior window in RAM. Older windows require
  restarting the corresponding selection/session; v1 does not spool all QR data.
- XOR block and overlapping-chain parity recover solvable missing frames, with
  manual replay as the fallback. There is no compression, fountain code, or
  multi-QR mode.
- Camera2 requests 640×480 but a few devices may reject that exact stream pair.
  Such devices need a supported-size selection enhancement.
- The Android app rebuilds only after all frames are present; it stores chunks on
  disk incrementally but deliberately avoids presenting partial output as final.
