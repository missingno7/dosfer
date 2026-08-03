"""Create a bootable FreeDOS disk matching Phoenix BIOS drive type 5."""

from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "pydeps"))

from fs.copy import copy_fs  # noqa: E402
from pyfatfs.PyFat import PyFat  # noqa: E402
from pyfatfs.PyFatFS import PyFatFS  # noqa: E402


VM = ROOT / "tools" / "86box" / "vm" / "dosfer-486"
SOURCE = VM / "FD14LITE.img"
TARGET = VM / "DOSFER46.img"

# Phoenix drive type 5: 940 cylinders, 6 heads, 17 sectors/track.
CYLINDERS = 940
HEADS = 6
SECTORS = 17
TOTAL_SECTORS = CYLINDERS * HEADS * SECTORS
PARTITION_START = SECTORS  # Cylinder 0, head 1, sector 1.
PARTITION_SECTORS = TOTAL_SECTORS - PARTITION_START
SOURCE_PARTITION_START = 63


def chs(cylinder: int, head: int, sector: int) -> bytes:
    return bytes((head, sector | ((cylinder >> 2) & 0xC0), cylinder & 0xFF))


def main() -> None:
    source = SOURCE.read_bytes()
    source_boot = SOURCE_PARTITION_START * 512

    TARGET.write_bytes(b"")
    fat = PyFat(offset=PARTITION_START * 512)
    fat.mkfs(
        str(TARGET),
        PyFat.FAT_TYPE_FAT16,
        size=PARTITION_SECTORS * 512,
        label="DOSFER46",
        media_type=0xF8,
    )
    fat.close()

    with TARGET.open("r+b") as image:
        # Reuse the proven FreeDOS MBR loader and create one active FAT16 entry.
        image.seek(0)
        image.write(source[:446])
        entry = bytearray(16)
        entry[0] = 0x80
        entry[1:4] = chs(0, 1, 1)
        entry[4] = 0x06
        entry[5:8] = chs(CYLINDERS - 1, HEADS - 1, SECTORS)
        struct.pack_into("<II", entry, 8, PARTITION_START, PARTITION_SECTORS)
        image.seek(446)
        image.write(entry)
        image.write(bytes(16 * 3))
        image.write(b"\x55\xAA")

        # Keep the freshly generated BPB, but use FreeDOS's FAT16 boot loader.
        boot_offset = PARTITION_START * 512
        image.seek(boot_offset)
        boot = bytearray(image.read(512))
        boot[24:26] = struct.pack("<H", SECTORS)
        boot[26:28] = struct.pack("<H", HEADS)
        boot[28:32] = struct.pack("<I", PARTITION_START)
        boot[62:512] = source[source_boot + 62:source_boot + 512]
        image.seek(boot_offset)
        image.write(boot)

    old_fs = PyFatFS(str(SOURCE), offset=SOURCE_PARTITION_START * 512, read_only=True)
    new_fs = PyFatFS(str(TARGET), offset=PARTITION_START * 512, read_only=False)
    try:
        copy_fs(old_fs, new_fs, preserve_time=True)
    finally:
        old_fs.close()
        new_fs.close()

    expected = TOTAL_SECTORS * 512
    if TARGET.stat().st_size != expected:
        raise SystemExit(f"bad image size: {TARGET.stat().st_size}, expected {expected}")
    print(
        f"Created {TARGET}: {TARGET.stat().st_size} bytes, "
        f"CHS {CYLINDERS}/{HEADS}/{SECTORS}"
    )


if __name__ == "__main__":
    main()
