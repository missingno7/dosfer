# Interrupted transfers and recovery

The Android app atomically retains every validated QR frame in private storage.
After an Android-app restart it scans those files and restores the active
session and missing ranges. Keep the DOS sender running, return to the current
unacknowledged window, use `R` or `M`, and continue. A DOS-program restart creates
a new session ID; in that case reset the phone session before accepting the new
transfer.

If reconstruction stops (storage full, permission revoked, CRC mismatch), the
destination contains an explicitly named `.partial` file. Free storage or
restore permission, delete/retain that partial as desired, and reconstruct
again; existing final files are never overwritten and receive ` (n)` suffixes.

For missing data, do not restart: use M on DOS and enter the receiver's ranges.
Valid frames survive any number of replays. Only reset the Android session once
the final `VALID` result and expected files have been independently inspected.

