# Quick start

## Real PC to phone

1. Build/install the Android receiver and open **DOSfer Receiver**.
2. Grant camera access and choose a destination directory.
3. Copy `DOSFER.EXE` from `dos_sender_legacy` to the DOS machine. In an empty
   writable directory run `DOSFER.EXE C:\PATH\FILE.DAT`.
4. Aim the phone squarely at the centred QR image and keep the quiet zone
   visible. Tap **Lock focus/exposure** once it is sharp.
5. At the end of each window, read **Missing** on the phone. Press Enter when it
   shows `-`, `R` to replay, or `M` and enter ranges such as `2,5-7`.
6. When all logical frames exist, tap **Reconstruct**. A green `VALID` result is
   the completion signal.

The legacy sender defaults to RGB3: V40-L, 320 × 200 planar output, 2,904-byte
logical DATA payloads, 66 logical frames per window and `/RE:3`. One physical
colour image contains three normal QR codes, so a full data window uses 22
physical images. `/HOLD:n` is a minimum hold for one physical RGB image.

The Android receiver initially decodes R, G and B separately. Different frame
IDs select RGB3. Two distinct images with three equal IDs select the direct
one-channel BW path for the rest of that window. Use `/BW` explicitly on DOS to
force monochrome output.

Useful commands:

```text
DOSFER.EXE FILE.DAT
DOSFER.EXE /RGB3 /HOLD:50 /RE:3 FILE.DAT
DOSFER.EXE /BW /VIDEO:320_70 FILE.DAT
```

When `M` selects only part of a window, rescue images are held longer and use a
rotated standard QR mask. Blank input after `M` clears the selection and replays
the complete current window.

In RGB3, the optimized default `/RE:3` protects three successive physical DATA
images with one physical parity image. Its channels form three independent
stride-3 equations: `D0^D3^D6`, `D1^D4^D7`, and `D2^D5^D8`. Losing any one of
the three DATA images therefore leaves one recoverable missing member in each
equation. A short tail uses one- or two-member equations. A physical image is
never a DATA/DATA/PARITY mixture. `/RE:0` disables parity. Other `/RE:n` values
retain ordinary contiguous block parity, while `/RE:C2`, `/RE:C4`, and other
even chain widths remain available; their equations are also batched into
parity-only RGB images.

## Sender keys

- Enter: commit the current window and advance
- R: replay the current remembered selection, or the whole window
- M: enter/replay missing logical frame indices; blank input means all
- Esc: pause; then C cancels or another key resumes

Controls are read only after END_WINDOW is visible.

## Local optical test

```powershell
powershell -ExecutionPolicy Bypass -File tools\adb_install.ps1
powershell -ExecutionPolicy Bypass -File tools\run_dosbox_test.ps1
```
