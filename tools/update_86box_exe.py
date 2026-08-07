"""Replace DOSFER.EXE in the bootable Phoenix type-5 86Box disk image."""

from hashlib import sha256
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "pydeps"))

from pyfatfs.PyFatFS import PyFatFS  # noqa: E402


IMAGE = ROOT / "tools" / "86box" / "vm" / "dosfer-486" / "DOSFER46.img"
EXE = ROOT / "dos_sender_legacy" / "build" / "DOSFER.EXE"
PARTITION_OFFSET = 17 * 512


def put_bytes(fs: PyFatFS, name: str, data: bytes) -> None:
    if fs.exists(name):
        fs.remove(name)
    with fs.openbin(name, "w") as target:
        target.write(data)


def main() -> None:
    if not IMAGE.is_file() or not EXE.is_file():
        raise SystemExit("DOSFER46.img or the DOSFER.EXE build is missing")

    new_exe = EXE.read_bytes()
    fs = PyFatFS(str(IMAGE), offset=PARTITION_OFFSET, read_only=False)
    try:
        if fs.exists("DOSFER.EXE"):
            with fs.openbin("DOSFER.EXE", "r") as source:
                put_bytes(fs, "DOSFER.OLD", source.read())
        put_bytes(fs, "DOSFER.EXE", new_exe)
        with fs.openbin("DOSFER.EXE", "r") as installed:
            installed_exe = installed.read()
    finally:
        fs.close()

    expected = sha256(new_exe).hexdigest().upper()
    actual = sha256(installed_exe).hexdigest().upper()
    if installed_exe != new_exe:
        raise SystemExit(f"Verification failed: expected {expected}, read back {actual}")
    print(f"Updated {IMAGE}")
    print(f"DOSFER.EXE: {len(new_exe)} bytes, SHA-256 {actual}")
    print("Previous executable saved inside the image as DOSFER.OLD")


if __name__ == "__main__":
    main()
