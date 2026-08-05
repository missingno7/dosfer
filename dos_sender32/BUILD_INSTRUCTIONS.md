# DOSFER32 Build Instructions with QR Fix

## Quick Build
Run the build script from the `dos_sender32` directory:
```cmd
build32.bat
```

The build script will automatically find Watcom in these locations:
- `..\tools\watcom`
- `C:\tmp\watcom` 
- `C:\WATCOM`
- Or set `%WATCOM%` environment variable manually

## Manual Build
If you prefer to build manually or need to customize:
```cmd
cd dos_sender32
set WATCOM=C:\tmp\watcom
set PATH=%WATCOM%\binnt64;%PATH%
set INCLUDE=%WATCOM%\h;..\dos_sender\include;..\dos_sender\third_party
wcl386 -q -bt=dos -mf -3s -ot -ol -oi -or -oh -za99 -s -DDOSFER32 -I..\dos_sender\third_party -l=dos4g -fe=build\DOSFER32 -fm=build\DOSFER32 main.c platform_vga.c protocol32.c ..\dos_sender\third_party\qrcodegen.c
```

## What Was Fixed
The QR code positioning bug that caused corruption in the left quarter of all QR codes has been fixed:

### Before
- QR codes started at wrong horizontal position (pixel 0)
- Left quarter of QR codes was corrupted
- QR codes were not properly centered

### After
- QR codes properly centered at pixel 71 (horizontal)
- Correct quiet zones: 67 pixels left/right, 4 pixels top/bottom
- No corruption in any portion of QR codes

## Testing
After building, test with:
```cmd
cd build
DOSFER32.EXE testfile.bin /RE:PLANE4 /WINDOW:32 /HOLD:100
```

## Expected Results
- QR codes should be perfectly centered in the 320x200 display
- No visual corruption in any portion of the QR code
- Android receiver should be able to decode all QR codes successfully
- Plane streaming should work smoothly with proper delta updates

## Files Modified
- `main.c`: Fixed `qr_raster_from_matrix()` and `qr_prepare_delta_map()` functions
- Both functions now use correct horizontal offset of 71 pixels

## Technical Details
- QR size: 177x177 pixels
- Display: 320x200 pixels
- Total QR width with quiet zone: 185 pixels
- Left margin: (320-185)/2 = 67 pixels
- QR start position: 67+4 = 71 pixels

## Troubleshooting
If build fails:
1. Ensure Watcom is installed at C:\tmp\watcom
2. Check that all source files are present
3. Verify qrcodegen.c is accessible at ..\dos_sender\third_party\

If QR codes still appear corrupted:
1. Verify you're using the newly built executable
2. Check that the build completed successfully
3. Test with a simple file first