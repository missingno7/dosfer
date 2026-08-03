# Quick start

## Real PC to phone

1. Install `DOSFER-Receiver-debug.apk` and open **DOSfer Receiver**.
2. Grant camera access; choose a destination directory.
3. Copy `DOSFER.EXE` to the DOS machine. In an empty writable directory run:
   `DOSFER.EXE C:\PATH\FILE.DAT` (multiple paths are accepted).
4. Aim the phone squarely at the centered QR. Keep the full quiet zone visible.
   Tap **Lock focus/exposure** when the image is sharp.
5. At each window end, read **Missing** on the phone. On DOS press Enter if it
   shows `-`, `R` for all, or `M` and type ranges such as `2,5-7`.
6. When all frames exist, tap **Reconstruct**. A green `VALID` result is the
   only completion signal. Do not treat `.partial` files as recovered files.

Start conservatively: QR v15-M, 4-pixel modules, 364-byte record payload, 750
ms hold, 32 frames/window, one repetition. Run `DOSFER /CAL` before valuable
data and shorten the hold only after the phone reports **RELIABLE**.

When `M` selects only part of a window, rescue frames are automatically held
2-4 times longer and displayed with another standard QR mask. Each subsequent
`R` rotates to another mask while retaining the same selection. Blank input
after `M` clears the selection and replays the complete window. Normal transfer
timing is unchanged.

All transfer settings can be supplied without calibration, for example:
`DOSFER /V:15 /ECC:M /SCALE:4 /PAYLOAD:364 /HOLD:750 /W:32 /R:1 FILE.DAT`.
Run `DOSFER /?` for ranges, aliases, polarity, and beep options. Regardless of
these settings, DOSfer always waits for an explicit decision between batches.

Use `DOSFER /FPS FILE.DAT` for the measured v12-M 10+ FPS preset, or
`DOSFER /BULK FILE.DAT` for v40-M maximum generated payload throughput. Both
use zero additional hold; add `/HOLD:n` after the preset when the camera needs
more exposure time (for example `/FPS /HOLD:100 FILE.DAT`).

Use `DOSFER /TURBO FILE.DAT` with the current receiver for the optimized 386
mode: v25-L, 1,225-byte DATA payloads, adjacent-frame XOR recovery, and an
anchor every 16 DATA frames. It measured 9.93 displayed FPS at 3000 DOSBox-X
cycles. `/NOCHAIN` after the preset disables recovery frames for compatibility.

## Local optical test

Connect and authorize an Android phone, then:

```powershell
powershell -ExecutionPolicy Bypass -File tools\adb_install.ps1
powershell -ExecutionPolicy Bypass -File tools\run_dosbox_test.ps1
```

DOSBox-X mounts an isolated staging directory as `C:` and runs exactly:
`DOSFER.EXE SAMPLE.TXT`.

## Sender keys

- Enter: confirm complete window / advance
- R: replay the remembered missing set, or the whole window when none is set
- M: set/replay missing indices; blank input clears the set and replays all
- B: replay previous window without losing current state
- `+` / `-`: 50 ms faster/slower, then replay the current window
- Esc: pause; then C cancels or another key resumes

All controls are read only after the end-of-window prompt appears. Windows run
uninterrupted, and keys typed while frames move cannot affect the next prompt.
