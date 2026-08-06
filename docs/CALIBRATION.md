# CRT calibration and benchmarking

Run `DOSFER /CAL`. The sender continuously emits numbered calibration frames.
The phone classifies captures as **RELIABLE**, **MARGINAL**, **TOO FAST**,
**QR TOO DENSE**, **EXCESSIVE DUPLICATES**, or **FRAMES SKIPPED**.

Keys: `+/-` changes hold time by 50 ms and I tests inversion. Esc exits.
Calibration deliberately uses the same V40-L geometry as a normal transfer;
advanced geometry experiments belong in `/BENCH`. Change one variable per run
and observe at least 100 unique frames.

Use a tripod/stand, dim reflections, keep the phone parallel to the CRT, keep
the code near screen center, and lock focus/exposure after alignment. A hold of
at least several camera exposures reduces CRT rolling-band misses. Inversion is
experimental and normally off. The alternating marks outside the quiet zone
help visual stability without changing QR pixels.

`DOSFER /BENCH file` separately reports sequential disk+CRC bytes/s and QR
encode time. Android reports requested capture resolution, zero-copy centered
decode crop, requested and measured sensor/ImageReader FPS, decode attempts,
duplicate and missing counts, busy-worker drops, exposure time, sensor frame
duration, and decode latency. The camera section lists discovered YUV modes,
their theoretical maximum FPS, measured status, and whether automatic or
manual mode selection is active. Record manual window wait time separately.
