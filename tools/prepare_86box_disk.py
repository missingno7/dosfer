"""Put the current DOSFER build and repeatable test data on the 86Box disk."""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "pydeps"))

from pyfatfs.PyFatFS import PyFatFS  # noqa: E402


IMAGE = ROOT / "tools" / "86box" / "vm" / "dosfer-486" / "FD14LITE.img"
EXE = ROOT / "dos_sender_legacy" / "build" / "DOSFER.EXE"
DEV_EXE = ROOT / "dos_sender_legacy" / "build" / "DOSFERD.EXE"
PARTITION_OFFSET = 63 * 512
SAMPLE_SIZE = 4 * 1024 * 1024


def put_bytes(fs: PyFatFS, name: str, data: bytes) -> None:
    if fs.exists(name):
        fs.remove(name)
    with fs.openbin(name, "w") as target:
        target.write(data)


def main() -> None:
    if not IMAGE.is_file() or not EXE.is_file():
        raise SystemExit("FreeDOS image or DOSFER.EXE is missing")

    fs = PyFatFS(str(IMAGE), offset=PARTITION_OFFSET, read_only=False)
    try:
        put_bytes(fs, "DOSFER.EXE", EXE.read_bytes())
        if DEV_EXE.is_file():
            put_bytes(fs, "DOSFERD.EXE", DEV_EXE.read_bytes())
        elif fs.exists("DOSFERD.EXE"):
            fs.remove("DOSFERD.EXE")

        if fs.exists("SETUP.BAT") and not fs.exists("SETUP.OLD"):
            fs.move("SETUP.BAT", "SETUP.OLD")

        put_bytes(
            fs,
            "RUN.BAT",
            b"@ECHO OFF\r\nDOSFER.EXE SAMPLE.BIN\r\n",
        )
        if DEV_EXE.is_file():
            put_bytes(
                fs,
                "BENCH.BAT",
                b"@ECHO OFF\r\nDOSFERD.EXE /BENCH SAMPLE.BIN > BENCH.TXT\r\n"
                b"TYPE BENCH.TXT\r\n",
            )
        elif fs.exists("BENCH.BAT"):
            fs.remove("BENCH.BAT")
        put_bytes(
            fs,
            "README.TXT",
            b"DOSFER 86Box test disk\r\n\r\n"
            b"RUN.BAT   - optical transfer, default RGB3 V40-L mode\r\n"
            + (
                b"BENCH.BAT - developer CPU/VGA benchmark to BENCH.TXT\r\n"
                if DEV_EXE.is_file()
                else b"BENCH.BAT - unavailable; build DOSFERD.EXE first\r\n"
            )
            + b"DOSFER.EXE /? shows production command-line options.\r\n",
        )

        if fs.exists("SAMPLE.BIN"):
            fs.remove("SAMPLE.BIN")
        pattern = bytes(range(256)) * 256
        with fs.openbin("SAMPLE.BIN", "w") as sample:
            for _ in range(SAMPLE_SIZE // len(pattern)):
                sample.write(pattern)
    finally:
        fs.close()

    installed = EXE.name + (" and " + DEV_EXE.name if DEV_EXE.is_file() else "")
    print(f"Prepared {IMAGE} with {installed} and {SAMPLE_SIZE} byte SAMPLE.BIN")


if __name__ == "__main__":
    main()
