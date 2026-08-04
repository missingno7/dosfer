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

The default is the optimized 386 path: V40-L, one-pixel modules in 320x200,
2,904-byte records, zero artificial hold, 32-frame windows, and `/RE:7`.
Run `DOSFER /CAL` before valuable data. Add `/HOLD:n` only if the phone reports
misses; generation and rendering already keep each QR visible for substantial
time on a 386.

When `M` selects only part of a window, rescue frames are automatically held
2-4 times longer and displayed with another standard QR mask. Each subsequent
`R` rotates to another mask while retaining the same selection. Blank input
after `M` clears the selection and replays the complete window. Normal transfer
timing is unchanged.

A typical explicit command is `DOSFER /HOLD:0 /W:32 /RE:C8 FILE.DAT`.
Run `DOSFER /?` for ranges, advanced diagnostic overrides, polarity, and beep
options. DOSfer always waits for an explicit decision between batches.

`/RE:7` is the default and sends seven DATA frames followed by one XOR parity
frame. It recovers any one missing DATA frame in each group. Use `/RE:15` for
lower overhead, `/RE:3` for more protection, or `/RE:0` for none. `/RE:C2`
selects adjacent chaining. Longer even chains overlap by half their width:
`/RE:C4`, `/RE:C6`, `/RE:C8`, up to C64. The spaced forms such as `/RE C4`
are also accepted. A chain's half-width must be smaller than `/WINDOW`.

At 3000 DOSBox-X cycles the default 7+1 schedule measured 8.64 displayed FPS,
7.47 DATA frames/s, and about 21.5 KB/s before optical losses. `/RE:C8`
measured 8.59 displayed FPS and 20.3 KB/s. `/RE:C2` uses the specialized affine
encoder and reaches a higher display rate, at the cost of almost one parity QR
per DATA QR.

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
