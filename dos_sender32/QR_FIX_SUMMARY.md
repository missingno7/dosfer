# DOSFER32 QR Code Corruption Fix

## Problem
The left quarter of all QR codes produced by DOSFER32.exe was corrupted.

## Root Cause
The QR code positioning logic was incorrect in two places:

### 1. Main QR Raster Generation (`qr_raster_from_matrix`)
The function was writing QR code data starting at byte offset 0 of each row, instead of positioning the QR code correctly within the 320-pixel wide display.

**Incorrect behavior:**
- QR code was written starting at pixel position 0 of each row
- This caused the left portion of the QR code to be cut off and misaligned

**Correct behavior:**
- QR code should be centered horizontally with quiet zones
- For a 177x177 QR code: total width = 177 + 8 (quiet zone) = 185 pixels
- Left margin = (320 - 185) / 2 = 67 pixels
- QR code start position = 67 + 4 (quiet zone) = 71 pixels

### 2. Delta Map Preparation (`qr_prepare_delta_map`)
The delta map used for efficient QR code updates was also using incorrect positioning, using `(x + 4)` instead of the correct `(x + 71)` offset.

## Solution
Fixed both functions to use the correct horizontal offset of 71 pixels:

### Fix 1: QR Raster Generation
Changed the positioning logic to:
```c
unsigned px = 71 + i * 8u;
unsigned byte_offset = px >> 3;
unsigned bit_offset = px & 7;
if (bit_offset == 0) {
    dst[byte_offset] |= v;
} else {
    dst[byte_offset] |= (uint8_t)(v >> bit_offset);
    dst[byte_offset + 1] |= (uint8_t)(v << (8 - bit_offset));
}
```

This correctly handles the fact that the QR code doesn't start at a byte boundary by splitting 8-bit chunks across bytes when necessary.

### Fix 2: Delta Map Preparation
Changed the offset calculation from:
```c
off = (unsigned)(y + 4) * 40u + (unsigned)((x + 4) >> 3);
w->delta_entries[i] = (uint32_t)off | ((uint32_t)(0x80u >> ((x + 4) & 7)) << 16);
```

To:
```c
off = (unsigned)(y + 4) * 40u + (unsigned)((x + 71) >> 3);
w->delta_entries[i] = (uint32_t)off | ((uint32_t)(0x80u >> ((x + 71) & 7)) << 16);
```

## Testing
After applying these fixes, the QR codes should be correctly positioned within the 320x200 display, with no corruption in the left quarter.

## Files Modified
- `dos_sender32/main.c`: Fixed `qr_raster_from_matrix()` and `qr_prepare_delta_map()` functions

## References
The correct positioning logic was derived from the original `dos_sender/src/vga.c` implementation, specifically the `build_qr_image_320()` and `prepare_delta()` functions.