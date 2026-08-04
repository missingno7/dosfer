"""Host reference implementation of DOSfer protocol v1."""
from __future__ import annotations
from dataclasses import dataclass
import binascii
import struct

FRAME_MAGIC = b"DQR1"
RECORD_MAGIC = b"DQRC"
VERSION = 1
FRAME_HEADER = 48
RECORD_HEADER = 24

DATA, END_WINDOW, CALIBRATION, CHAIN_XOR, BLOCK_XOR = 1, 2, 3, 4, 5
SESSION, DIRECTORY, FILE_BEGIN, FILE_DATA, FILE_END, TRANSFER_END = range(1, 7)
FLAG_REPEATED, FLAG_PAIR_WHITENED, FLAG_WHITENED = 0x0001, 0x0004, 0x0008

def crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF

def whiten_payload(data: bytes, session: int, global_index: int) -> bytes:
    state = (session ^ ((global_index * 0x9E3779B9) & 0xFFFFFFFF) ^ 0xD05FE123) & 0xFFFFFFFF
    if not state: state = 0xA5A5A5A5
    out = bytearray(data)
    pos = 0
    while pos < len(out):
        state ^= (state << 13) & 0xFFFFFFFF
        state ^= state >> 17
        state ^= (state << 5) & 0xFFFFFFFF
        state &= 0xFFFFFFFF
        key = state
        for _ in range(4):
            if pos == len(out): break
            out[pos] ^= key & 0xFF
            key >>= 8
            pos += 1
    return bytes(out)

def whiten_payload_pair(data: bytes, session: int, global_index: int) -> bytes:
    left = (session ^ ((global_index * 0x9E3779B9) & 0xFFFFFFFF) ^ 0xD05FE123) & 0xFFFFFFFF
    right = (session ^ (((global_index + 1) * 0x9E3779B9) & 0xFFFFFFFF) ^ 0xD05FE123) & 0xFFFFFFFF
    if not left: left = 0xA5A5A5A5
    if not right: right = 0xA5A5A5A5
    out = bytearray(data)
    pos = 0
    while pos < len(out):
        left ^= (left << 13) & 0xFFFFFFFF; left ^= left >> 17; left ^= (left << 5) & 0xFFFFFFFF; left &= 0xFFFFFFFF
        right ^= (right << 13) & 0xFFFFFFFF; right ^= right >> 17; right ^= (right << 5) & 0xFFFFFFFF; right &= 0xFFFFFFFF
        key = left ^ right
        for _ in range(4):
            if pos == len(out): break
            out[pos] ^= key & 0xFF; key >>= 8; pos += 1
    return bytes(out)

@dataclass(frozen=True)
class Frame:
    kind: int
    flags: int
    session: int
    window: int
    global_index: int
    window_index: int
    window_count: int
    stream_id: int
    stream_offset: int
    payload: bytes

    def encode(self) -> bytes:
        if not (0 < self.session <= 0xFFFFFFFF): raise ValueError("invalid session")
        if not (0 <= len(self.payload) <= 0xFFFF): raise ValueError("payload too large")
        wire_payload = (whiten_payload_pair(self.payload, self.session, self.global_index)
                        if self.flags & FLAG_PAIR_WHITENED else
                        whiten_payload(self.payload, self.session, self.global_index)
                        if self.flags & FLAG_WHITENED else self.payload)
        h = struct.pack(">4sBBHIIIHHIIHHIII", FRAME_MAGIC, VERSION, self.kind,
            self.flags, self.session, self.window, self.global_index,
            self.window_index, self.window_count, self.stream_id,
            self.stream_offset, len(self.payload), FRAME_HEADER,
            crc32(wire_payload), 0, 0)
        h = h[:40] + struct.pack(">I", crc32(h)) + h[44:]
        return h + wire_payload

    @staticmethod
    def decode(raw: bytes) -> "Frame":
        if len(raw) < FRAME_HEADER: raise ValueError("short frame")
        fields = struct.unpack(">4sBBHIIIHHIIHHIII", raw[:FRAME_HEADER])
        magic, ver, kind, flags, session, window, glob, wi, wc, sid, off, plen, hlen, pcrc, hcrc, reserved = fields
        if magic != FRAME_MAGIC or ver != VERSION or hlen != FRAME_HEADER or reserved: raise ValueError("unsupported header")
        if kind not in (DATA, END_WINDOW, CALIBRATION, CHAIN_XOR, BLOCK_XOR): raise ValueError("unknown kind")
        if flags & ~0x0F or session == 0 or len(raw) != FRAME_HEADER + plen: raise ValueError("invalid fields")
        hz = raw[:40] + b"\0\0\0\0" + raw[44:FRAME_HEADER]
        if crc32(hz) != hcrc: raise ValueError("header crc")
        payload = raw[FRAME_HEADER:]
        if crc32(payload) != pcrc: raise ValueError("payload crc")
        if flags & FLAG_PAIR_WHITENED: payload = whiten_payload_pair(payload, session, glob)
        elif flags & FLAG_WHITENED: payload = whiten_payload(payload, session, glob)
        return Frame(kind, flags, session, window, glob, wi, wc, sid, off, payload)

@dataclass(frozen=True)
class Record:
    type: int
    record_id: int
    file_id: int
    body: bytes
    flags: int = 0

    def encode(self) -> bytes:
        if self.type not in range(1, 7): raise ValueError("unknown record")
        return struct.pack(">4sBBHIIII", RECORD_MAGIC, VERSION, self.type,
            self.flags, self.record_id, self.file_id, len(self.body),
            crc32(self.body)) + self.body

    @staticmethod
    def decode(raw: bytes) -> "Record":
        if len(raw) < RECORD_HEADER: raise ValueError("short record")
        magic, ver, typ, flags, rid, fid, size, checksum = struct.unpack(">4sBBHIIII", raw[:RECORD_HEADER])
        if magic != RECORD_MAGIC or ver != VERSION or typ not in range(1, 7) or flags: raise ValueError("unsupported record")
        body = raw[RECORD_HEADER:]
        if len(body) != size or crc32(body) != checksum: raise ValueError("record crc/length")
        return Record(typ, rid, fid, body, flags)

def safe_path(path: str) -> str:
    if not path or len(path.encode()) > 1024 or "\\" in path or path.startswith("/"):
        raise ValueError("unsafe path")
    parts = path.split("/")
    if any(not p or p in (".", "..") or len(p.encode()) > 255 or
           any(ord(c) < 32 for c in p) for p in parts):
        raise ValueError("unsafe component")
    if len(parts[0]) >= 2 and parts[0][1] == ":": raise ValueError("drive path")
    return "/".join(parts)

def chain_frame(left: Frame, right: Frame) -> Frame:
    if (left.kind != DATA or right.kind != DATA or left.session != right.session or
            left.window != right.window or right.global_index != left.global_index + 1 or
            right.window_index != left.window_index + 1): raise ValueError("non-adjacent data")
    n=max(len(left.payload),len(right.payload));payload=bytes((left.payload[i] if i<len(left.payload) else 0) ^ (right.payload[i] if i<len(right.payload) else 0) for i in range(n))
    lengths=(len(left.payload)<<16)|len(right.payload)
    return Frame(CHAIN_XOR,FLAG_PAIR_WHITENED,left.session,left.window,left.global_index,left.window_index,left.window_count,lengths,0,payload)

def recover_chain(chain: Frame, known: bytes, known_is_left: bool) -> bytes:
    if chain.kind != CHAIN_XOR: raise ValueError("not chain")
    left,right=chain.stream_id>>16,chain.stream_id&0xffff
    if len(known)!=(left if known_is_left else right) or len(chain.payload)!=max(left,right): raise ValueError("chain lengths")
    size=right if known_is_left else left
    out=bytes(chain.payload[i] ^ (known[i] if i<len(known) else 0) for i in range(size));Record.decode(out);return out

def block_frame(frames: list[Frame]) -> Frame:
    if not frames or len(frames)>64: raise ValueError("block size")
    first=frames[0]
    for i,frame in enumerate(frames):
        if (frame.kind != DATA or frame.session != first.session or frame.window != first.window or
                frame.global_index != first.global_index+i or frame.window_index != first.window_index+i or
                frame.window_count != first.window_count): raise ValueError("non-contiguous block")
    size=max(len(frame.payload) for frame in frames);payload=bytearray(size)
    for frame in frames:
        for i,value in enumerate(frame.payload): payload[i]^=value
    return Frame(BLOCK_XOR,FLAG_WHITENED,first.session,first.window,first.global_index,
                 first.window_index,first.window_count,len(frames),0,bytes(payload))

def chain_blocks(frames: list[Frame], width: int) -> list[Frame]:
    if width < 2 or width > 64 or width & 1: raise ValueError("chain width")
    half=width//2
    return [block_frame(frames[start:min(start+width,len(frames))])
            for start in range(0,len(frames),half) if start+half<len(frames)]

def recover_block(parity: Frame, members: list[bytes | None], missing: int) -> bytes:
    if (parity.kind != BLOCK_XOR or parity.flags != FLAG_WHITENED or parity.stream_offset or parity.stream_id != len(members) or
            not 1<=len(members)<=64 or not 0<=missing<len(members) or members[missing] is not None):
        raise ValueError("block")
    out=bytearray(parity.payload)
    for index,known in enumerate(members):
        if index==missing: continue
        if known is None or len(known)>len(out): raise ValueError("block members")
        for i,value in enumerate(known):out[i]^=value
    if len(out)<RECORD_HEADER:raise ValueError("block payload")
    total=RECORD_HEADER+struct.unpack_from(">I",out,16)[0]
    if total>len(out) or any(out[total:]):raise ValueError("block length/padding")
    recovered=bytes(out[:total]);Record.decode(recovered);return recovered

def metadata(attrs: int, date: int, time: int, path: str, size: int | None = None) -> bytes:
    p = safe_path(path).encode("utf-8")
    base = struct.pack(">BBHH", attrs, 0, date, time)
    return base + (struct.pack(">IH", size, len(p)) if size is not None else struct.pack(">H", len(p))) + p
