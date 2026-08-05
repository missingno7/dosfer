# DOSFER32 QR Code Fix - Simplified Approach

## Problem
The left quarter of QR codes was corrupted due to incorrect positioning logic.

## Root Cause
The original batched approach was too complex and had bit alignment issues when trying to handle non-byte-aligned QR positioning.

## Solution
Switched to per-module processing, matching the original working implementation exactly.

## Key Changes

### Before (Complex Batched Approach)
```c
for (i = 0; i < 23u; ++i) {
    // Process 8 modules at a time
    // Complex bit shifting and alignment logic
}
```

### After (Simple Per-Module Approach)
```c
for (i = 0; i < QR_SIZE; ++i) {
    // Process each module individually
    unsigned bit = y * QR_SIZE + i;
    const uint8_t *src = w->matrix + 1u + (bit >> 3);
    unsigned shift = bit & 7u;
    uint8_t v = (src[0] >> shift) & 1u;
    unsigned px = 71 + i;
    unsigned byte_offset = (px >> 3);
    unsigned bit_offset = px & 7u;
    uint8_t mask = (uint8_t)(0x80u >> bit_offset);
    if (v) dst[byte_offset] |= mask;
}
```

## Advantages of Simplified Approach
1. **Exact match to original logic** - Processes each module the same way as `build_qr_image_320`
2. **No bit alignment complexity** - Each module is placed independently
3. **Easier to verify** - Direct correspondence to original working code
4. **More maintainable** - Clear and straightforward logic

## Positioning Calculation
- Display: 320x200 pixels
- QR size: 177x177 pixels
- Total width with quiet zone: 185 pixels
- Left margin: (320 - 185) / 2 = 67 pixels
- QR start position: 67 + 4 = 71 pixels

## Testing
Build and test to verify QR codes are now properly centered with no corruption.

## Files Modified
- `dos_sender32/main.c`: Simplified `qr_raster_from_matrix()` function