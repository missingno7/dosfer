# Real-hardware checklist

- Back up source media if possible; run `CHKDSK` read-only first if disk health is suspect.
- Confirm DOS 5/6, a 386 or newer CPU, working EGA/VGA Mode 0Dh, and inspect free conventional memory with `MEM /C`.
- Build the developer binary and run `DOSFERD /BENCH representative.bin`; record disk, QR, RGB3 delta and planar-upload timings.
- Warm up the CRT and phone, clean the glass/lens, disable phone battery saver and lock display rotation.
- Run `DOSFERD /CAL` at the intended video mode and cadence; lock focus/exposure only after the framing is stable.
- Test `/BW` first, then `/RGB3`; verify that the app reports BW and RGB3 classification correctly.
- Test a known small file; compare final size and CRC on a third machine.
- Test current-window replay (`R`), selected rescue (`M`), and Esc pause/resume/cancel. There is no previous-window `B` replay.
- In RGB3, deliberately obscure one complete physical DATA image and verify stride-3 parity recovery of all three logical frames.
- Test a short final RGB tuple, where the unused colour channel(s) repeat the last valid logical frame.
- Test a directory containing empty, read-only and hidden files plus nested directories.
- Test low Android storage and an existing destination name.
- Run at least 1,000 physical images per candidate `/HOLD` value and record physical FPS, logical QR/s, decode success by channel and reconstructed payload bytes/s.
- Inspect the Open Watcom map for DGROUP/stack headroom, then repeat with the resident DOS drivers and TSRs used on the target machine.
- For valuable recovery, retain Android private session frames until two independently verified copies exist.
