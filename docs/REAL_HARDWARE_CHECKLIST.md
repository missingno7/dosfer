# Real-hardware checklist

- Back up source media if possible; run `CHKDSK` read-only first if disk health is suspect.
- Confirm DOS 5/6, 386, VGA mode 12h, at least 300 KiB conventional memory free.
- Run `DOSFER /BENCH representative.bin`; record disk+CRC and QR timings.
- Warm up CRT/phone, clean glass/lens, disable phone battery saver/rotation.
- Run 100-frame calibration at defaults; lock focus/exposure; require RELIABLE.
- Test a known small file; compare final size and CRC on a third machine.
- Test duplicates (R), a selected range (M), previous window (B), pause/restart.
- Test a directory with empty/read-only/hidden files and nested directories.
- Test low Android storage and an existing destination name.
- Run 1,000 calibration frames per candidate speed and record all four throughput metrics.
- For valuable recovery, retain Android private session frames until two copies are verified.

