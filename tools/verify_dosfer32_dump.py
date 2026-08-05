#!/usr/bin/env python3
"""Verify DOSFER32 /DUMP output: compose bitplanes, render QRs, decode DQR1 frames."""
from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

VGA_WIDTH = 320
VGA_HEIGHT = 200
VGA_RASTER_BYTES = 8000
QR_SIZE = 177
QR_QUIET = 4
QR_X0 = (VGA_WIDTH - (QR_SIZE + 2 * QR_QUIET)) // 2 + QR_QUIET
FRAME_MAGIC = b"DQR1"
FRAME_HEADER = 48


def load_raw(path: Path) -> bytes:
    data = path.read_bytes()
    if len(data) != VGA_RASTER_BYTES:
        raise ValueError(f"{path}: expected {VGA_RASTER_BYTES} bytes, got {len(data)}")
    return data


def compose_planes(planes: list[bytes], mask: int) -> bytes:
    out = bytearray(VGA_RASTER_BYTES)
    for i in range(VGA_RASTER_BYTES):
        value = 0
        for plane in range(4):
            if mask & (1 << plane):
                value ^= planes[plane][i]
        out[i] = value
    return bytes(out)


def raster_to_qr_image(raster: bytes):
  try:
    from PIL import Image
  except ImportError as exc:
    raise SystemExit("Pillow required: pip install pillow") from exc
  img = Image.new("L", (QR_SIZE, QR_SIZE), 255)
  px_data = []
  for y in range(QR_SIZE):
    row = []
    for x in range(QR_SIZE):
      px = QR_X0 + x
      py = QR_QUIET + y
      byte = raster[py * 40 + px // 8]
      bit = (byte >> (7 - (px % 8))) & 1
      row.append(0 if bit else 255)
    px_data.extend(row)
  img.putdata(px_data)
  return img


def decode_qr(img) -> bytes | None:
  try:
    from pyzbar.pyzbar import decode as zbar_decode
  except ImportError:
    return None
  results = zbar_decode(img)
  if not results:
    return None
  return results[0].data


def dewhiten_plane(payload: bytes, session: int, stream_id: int) -> bytes:
  state = (session ^ ((stream_id * 0x9E3779B9) & 0xFFFFFFFF) ^ 0xD05FE123) & 0xFFFFFFFF
  if not state:
    state = 0xA5A5A5A5
  out = bytearray(payload)
  pos = 0
  while pos < len(out):
    state ^= (state << 13) & 0xFFFFFFFF
    state ^= state >> 17
    state ^= (state << 5) & 0xFFFFFFFF
    state &= 0xFFFFFFFF
    key = state
    for _ in range(4):
      if pos == len(out):
        break
      out[pos] ^= key & 0xFF
      key >>= 8
      pos += 1
  return bytes(out)


def parse_dqr1(raw: bytes) -> dict:
  if len(raw) < FRAME_HEADER or raw[:4] != FRAME_MAGIC:
    raise ValueError("not a DQR1 frame")
  version, kind, flags = raw[4], raw[5], struct.unpack(">H", raw[6:8])[0]
  session, window, global_index = struct.unpack(">III", raw[8:20])
  window_index, window_count = struct.unpack(">HH", raw[20:24])
  stream_id, stream_offset, payload_len = struct.unpack(">IIH", raw[24:32])
  payload = raw[FRAME_HEADER : FRAME_HEADER + payload_len]
  if flags & 0x0010:
    payload = dewhiten_plane(payload, session, stream_id)
  return {
    "version": version,
    "kind": kind,
    "flags": flags,
    "session": session,
    "window": window,
    "global_index": global_index,
    "window_index": window_index,
    "window_count": window_count,
    "stream_id": stream_id,
    "stream_offset": stream_offset,
    "payload_len": payload_len,
    "payload": payload,
  }


def parse_meta(path: Path) -> list[dict]:
  entries = []
  if not path.is_file():
    return entries
  sym_re = re.compile(
    r"group=(?P<group>\d+)\s+sym=(?P<sym>\d+)\s+mask=(?P<mask>[0-9A-Fa-f]+)\s+"
    r"slot=(?P<slot>\d+)\s+correction=(?P<corr>\d+)\s+file=(?P<file>\S+)"
  )
  for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
    m = sym_re.search(line)
    if m:
      entries.append(m.groupdict())
  return entries


def verify_dump(dump_dir: Path, export_png: bool) -> int:
  meta_path = dump_dir / "D32META.TXT"
  entries = parse_meta(meta_path)
  if not entries:
    print(f"No symbol entries in {meta_path}; scanning G*_S*.RAW files")
    for path in sorted(dump_dir.glob("G*S*.RAW")):
      m = re.match(r"G(\d+)S(\d+)\.RAW", path.name)
      if m:
        entries.append({"group": m.group(1), "sym": m.group(2), "file": path.name})

  failures = 0
  print(f"Verifying dump in {dump_dir} ({len(entries)} composed symbols)")
  for entry in entries:
    raw_path = dump_dir / entry["file"]
    if not raw_path.is_file():
      print(f"FAIL missing {raw_path.name}")
      failures += 1
      continue
    raster = load_raw(raw_path)
    img = raster_to_qr_image(raster)
    if export_png:
      png_path = raw_path.with_suffix(".png")
      img.save(png_path)
      print(f"  wrote {png_path.name}")
    raw = decode_qr(img)
    if raw is None:
      print(f"FAIL {raw_path.name}: QR decode failed (install pyzbar + zbar DLL?)")
      failures += 1
      continue
    try:
      frame = parse_dqr1(raw)
    except ValueError as exc:
      print(f"FAIL {raw_path.name}: {exc}  prefix={raw[:16].hex()}")
      failures += 1
      continue
    print(
      f"OK {raw_path.name}: kind={frame['kind']} global={frame['global_index']} "
      f"wi={frame['window_index']} stream={frame['stream_id']} payload={frame['payload_len']}B"
    )
  return failures


def main() -> int:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("dump_dir", type=Path, nargs="?", default=Path("D32DUMP"))
  parser.add_argument("--png", action="store_true", help="export QR crop PNG per symbol")
  args = parser.parse_args()
  if not args.dump_dir.is_dir():
    print(f"dump directory not found: {args.dump_dir}", file=sys.stderr)
    return 2
  return 1 if verify_dump(args.dump_dir.resolve(), args.png) else 0


if __name__ == "__main__":
  raise SystemExit(main())
