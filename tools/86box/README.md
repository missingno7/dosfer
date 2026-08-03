# DOSFER 86Box hardware-realism test

The emulator, ROM set, FreeDOS media and generated VM disk images are local
dependencies and are intentionally not stored in this repository. Install
86Box under `tools/86box/app`, place the VM disk images under
`tools/86box/vm/dosfer-486`, and keep the checked-in `86box.cfg` in that VM
directory. The disk-image helper scripts require the packages listed in
`tools/requirements-86box.txt`.

```powershell
python -m pip install -r tools\requirements-86box.txt
```

Run `run_386sx16.bat` for the conservative childhood-PC baseline:

- Packard Bell PB300-class 386SX at 16 MHz
- 2 MB RAM
- onboard Oak VGA
- IDE disk using 86Box's generic 1992 3600-RPM timing model
- bootable FreeDOS 1.4 disk containing the current `DOSFER.EXE`

At the FreeDOS prompt:

- `RUN.BAT` starts a V40-M transfer of the 4 MB deterministic sample.
- `BENCH.BAT` runs the DOSFER benchmark and writes `BENCH.TXT`.
- `DOSFER.EXE /?` shows all sender options.

Rebuild DOSFER, then run `python tools\prepare_86box_disk.py` to refresh the
executable inside the disk image.
