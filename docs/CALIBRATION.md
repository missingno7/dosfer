# CRT calibration and benchmarking

Run `DOSFER /CAL`. The sender continuously emits numbered calibration frames.
The phone classifies captures as **RELIABLE**, **MARGINAL**, **TOO FAST**,
**QR TOO DENSE**, **EXCESSIVE DUPLICATES**, or **FRAMES SKIPPED**.

Keys: `+/-` changes hold time by 50 ms, V cycles QR versions, E cycles L/M/Q,
P changes payload, I tests inversion, and S writes `DOSFER.CFG`. Begin at the
defaults. First shorten hold time, then increase payload/version; change only
one variable per run and observe at least 100 unique frames.

Use a tripod/stand, dim reflections, keep the phone parallel to the CRT, keep
the code near screen center, and lock focus/exposure after alignment. A hold of
at least several camera exposures reduces CRT rolling-band misses. Inversion is
experimental and normally off. The alternating marks outside the quiet zone
help visual stability without changing QR pixels.

`DOSFER /BENCH file` separately reports sequential disk+CRC bytes/s and QR
encode time. Android reports decoded fps, useful payload B/s, duplicate/missing
counts and decode latency. Record manual window wait time separately.

