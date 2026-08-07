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
MAX_WINDOW = 128

DATA, END_WINDOW, CALIBRATION, CHAIN_XOR, BLOCK_XOR = 1, 2, 3, 4, 5
SESSION, DIRECTORY, FILE_BEGIN, FILE_DATA, FILE_END, TRANSFER_END = range(1, 7)
FLAG_REPEATED = 0x0001
FLAG_PAIR_WHITENED = 0x0004
FLAG_WHITENED = 0x0008
FLAG_GROUP_XOR_WHITENED = 0x0020
KNOWN_FLAGS = (
    FLAG_REPEATED
    | FLAG_PAIR_WHITENED
    | FLAG_WHITENED
    | FLAG_GROUP_XOR_WHITENED
)
WHITENING_FLAGS = (
    FLAG_PAIR_WHITENED | FLAG_WHITENED | FLAG_GROUP_XOR_WHITENED
)


def crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def _xorshift32(state: int) -> int:
    state ^= (state << 13) & 0xFFFFFFFF
    state ^= state >> 17
    state ^= (state << 5) & 0xFFFFFFFF
    return state & 0xFFFFFFFF


def _whitening_seed(session: int, global_index: int) -> int:
    state = (
        session
        ^ ((global_index * 0x9E3779B9) & 0xFFFFFFFF)
        ^ 0xD05FE123
    ) & 0xFFFFFFFF
    return state or 0xA5A5A5A5


def _apply_whitening(data: bytes, states: list[int]) -> bytes:
    out = bytearray(data)
    pos = 0
    while pos < len(out):
        key = 0
        for index, state in enumerate(states):
            state = _xorshift32(state)
            states[index] = state
            key ^= state
        for _ in range(4):
            if pos == len(out):
                break
            out[pos] ^= key & 0xFF
            key >>= 8
            pos += 1
    return bytes(out)


def whiten_payload(data: bytes, session: int, global_index: int) -> bytes:
    return _apply_whitening(data, [_whitening_seed(session, global_index)])


def whiten_payload_pair(data: bytes, session: int, global_index: int) -> bytes:
    return _apply_whitening(
        data,
        [
            _whitening_seed(session, global_index),
            _whitening_seed(session, global_index + 1),
        ],
    )


def whiten_payload_xor_group(
    data: bytes,
    session: int,
    global_index: int,
    count: int,
    stride: int,
) -> bytes:
    """Apply the XOR of ``count`` DATA whitening streams.

    RGB3 parity uses count 1..3 and normally stride 3, so one equation protects
    the same colour channel across successive physical RGB images.
    """
    if not 1 <= count <= 3 or stride < 1:
        raise ValueError("group whitening")
    return _apply_whitening(
        data,
        [
            _whitening_seed(session, global_index + index * stride)
            for index in range(count)
        ],
    )


def _validate_frame_fields(
    kind: int,
    flags: int,
    session: int,
    window_index: int,
    window_count: int,
    stream_id: int,
    stream_offset: int,
    payload_length: int,
) -> None:
    if not (0 < session <= 0xFFFFFFFF):
        raise ValueError("invalid session")
    if not (0 <= payload_length <= 0xFFFF):
        raise ValueError("payload too large")
    if kind not in (DATA, END_WINDOW, CALIBRATION, CHAIN_XOR, BLOCK_XOR):
        raise ValueError("unknown kind")
    if flags & ~KNOWN_FLAGS:
        raise ValueError("unknown flags")
    if (flags & WHITENING_FLAGS).bit_count() > 1:
        raise ValueError("multiple whitening modes")
    if flags & FLAG_GROUP_XOR_WHITENED:
        stride = stream_offset or 1
        if (
            kind != BLOCK_XOR
            or not 1 <= stream_id <= 3
            or not 1 <= stride <= MAX_WINDOW
            or not 0 <= window_index < window_count <= MAX_WINDOW
            or window_index + (stream_id - 1) * stride >= window_count
        ):
            raise ValueError("invalid group XOR fields")


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
        _validate_frame_fields(
            self.kind,
            self.flags,
            self.session,
            self.window_index,
            self.window_count,
            self.stream_id,
            self.stream_offset,
            len(self.payload),
        )
        if self.flags & FLAG_GROUP_XOR_WHITENED:
            wire_payload = whiten_payload_xor_group(
                self.payload,
                self.session,
                self.global_index,
                self.stream_id,
                self.stream_offset or 1,
            )
        elif self.flags & FLAG_PAIR_WHITENED:
            wire_payload = whiten_payload_pair(
                self.payload, self.session, self.global_index
            )
        elif self.flags & FLAG_WHITENED:
            wire_payload = whiten_payload(
                self.payload, self.session, self.global_index
            )
        else:
            wire_payload = self.payload
        header = struct.pack(
            ">4sBBHIIIHHIIHHIII",
            FRAME_MAGIC,
            VERSION,
            self.kind,
            self.flags,
            self.session,
            self.window,
            self.global_index,
            self.window_index,
            self.window_count,
            self.stream_id,
            self.stream_offset,
            len(self.payload),
            FRAME_HEADER,
            crc32(wire_payload),
            0,
            0,
        )
        header = header[:40] + struct.pack(">I", crc32(header)) + header[44:]
        return header + wire_payload

    @staticmethod
    def decode(raw: bytes) -> "Frame":
        if len(raw) < FRAME_HEADER:
            raise ValueError("short frame")
        fields = struct.unpack(">4sBBHIIIHHIIHHIII", raw[:FRAME_HEADER])
        (
            magic,
            version,
            kind,
            flags,
            session,
            window,
            global_index,
            window_index,
            window_count,
            stream_id,
            stream_offset,
            payload_length,
            header_length,
            payload_crc,
            header_crc,
            reserved,
        ) = fields
        if (
            magic != FRAME_MAGIC
            or version != VERSION
            or header_length != FRAME_HEADER
            or reserved
            or len(raw) != FRAME_HEADER + payload_length
        ):
            raise ValueError("unsupported header")
        _validate_frame_fields(
            kind,
            flags,
            session,
            window_index,
            window_count,
            stream_id,
            stream_offset,
            payload_length,
        )
        zeroed_header = raw[:40] + b"\0\0\0\0" + raw[44:FRAME_HEADER]
        if crc32(zeroed_header) != header_crc:
            raise ValueError("header crc")
        payload = raw[FRAME_HEADER:]
        if crc32(payload) != payload_crc:
            raise ValueError("payload crc")
        if flags & FLAG_GROUP_XOR_WHITENED:
            payload = whiten_payload_xor_group(
                payload,
                session,
                global_index,
                stream_id,
                stream_offset or 1,
            )
        elif flags & FLAG_PAIR_WHITENED:
            payload = whiten_payload_pair(payload, session, global_index)
        elif flags & FLAG_WHITENED:
            payload = whiten_payload(payload, session, global_index)
        return Frame(
            kind,
            flags,
            session,
            window,
            global_index,
            window_index,
            window_count,
            stream_id,
            stream_offset,
            payload,
        )


@dataclass(frozen=True)
class Record:
    type: int
    record_id: int
    file_id: int
    body: bytes
    flags: int = 0

    def encode(self) -> bytes:
        if self.type not in range(1, 7):
            raise ValueError("unknown record")
        return (
            struct.pack(
                ">4sBBHIIII",
                RECORD_MAGIC,
                VERSION,
                self.type,
                self.flags,
                self.record_id,
                self.file_id,
                len(self.body),
                crc32(self.body),
            )
            + self.body
        )

    @staticmethod
    def decode(raw: bytes) -> "Record":
        if len(raw) < RECORD_HEADER:
            raise ValueError("short record")
        magic, version, record_type, flags, record_id, file_id, size, checksum = (
            struct.unpack(">4sBBHIIII", raw[:RECORD_HEADER])
        )
        if (
            magic != RECORD_MAGIC
            or version != VERSION
            or record_type not in range(1, 7)
            or flags
        ):
            raise ValueError("unsupported record")
        body = raw[RECORD_HEADER:]
        if len(body) != size or crc32(body) != checksum:
            raise ValueError("record crc/length")
        return Record(record_type, record_id, file_id, body, flags)


def safe_path(path: str) -> str:
    if not path or len(path.encode()) > 1024 or "\\" in path or path.startswith("/"):
        raise ValueError("unsafe path")
    parts = path.split("/")
    if any(
        not part
        or part in (".", "..")
        or len(part.encode()) > 255
        or any(ord(character) < 32 for character in part)
        for part in parts
    ):
        raise ValueError("unsafe component")
    if len(parts[0]) >= 2 and parts[0][1] == ":":
        raise ValueError("drive path")
    return "/".join(parts)


def chain_frame(left: Frame, right: Frame) -> Frame:
    if (
        left.kind != DATA
        or right.kind != DATA
        or left.session != right.session
        or left.window != right.window
        or right.global_index != left.global_index + 1
        or right.window_index != left.window_index + 1
    ):
        raise ValueError("non-adjacent data")
    size = max(len(left.payload), len(right.payload))
    payload = bytes(
        (left.payload[index] if index < len(left.payload) else 0)
        ^ (right.payload[index] if index < len(right.payload) else 0)
        for index in range(size)
    )
    lengths = (len(left.payload) << 16) | len(right.payload)
    return Frame(
        CHAIN_XOR,
        FLAG_PAIR_WHITENED,
        left.session,
        left.window,
        left.global_index,
        left.window_index,
        left.window_count,
        lengths,
        0,
        payload,
    )


def recover_chain(chain: Frame, known: bytes, known_is_left: bool) -> bytes:
    if chain.kind != CHAIN_XOR:
        raise ValueError("not chain")
    left_length, right_length = chain.stream_id >> 16, chain.stream_id & 0xFFFF
    if (
        len(known) != (left_length if known_is_left else right_length)
        or len(chain.payload) != max(left_length, right_length)
    ):
        raise ValueError("chain lengths")
    size = right_length if known_is_left else left_length
    recovered = bytes(
        chain.payload[index] ^ (known[index] if index < len(known) else 0)
        for index in range(size)
    )
    Record.decode(recovered)
    return recovered


def _xor_payloads(frames: list[Frame]) -> bytes:
    size = max(len(frame.payload) for frame in frames)
    payload = bytearray(size)
    for frame in frames:
        for index, value in enumerate(frame.payload):
            payload[index] ^= value
    return bytes(payload)


def block_frame(frames: list[Frame]) -> Frame:
    if not frames or len(frames) > 64:
        raise ValueError("block size")
    first = frames[0]
    for index, frame in enumerate(frames):
        if (
            frame.kind != DATA
            or frame.session != first.session
            or frame.window != first.window
            or frame.global_index != first.global_index + index
            or frame.window_index != first.window_index + index
            or frame.window_count != first.window_count
        ):
            raise ValueError("non-contiguous block")
    return Frame(
        BLOCK_XOR,
        FLAG_WHITENED,
        first.session,
        first.window,
        first.global_index,
        first.window_index,
        first.window_count,
        len(frames),
        0,
        _xor_payloads(frames),
    )


def group_block_frame(frames: list[Frame], stride: int) -> Frame:
    """Build the optimized one-to-three-member strided RGB3 parity equation."""
    if not frames or len(frames) > 3 or stride < 1:
        raise ValueError("group block size/stride")
    first = frames[0]
    for index, frame in enumerate(frames):
        if (
            frame.kind != DATA
            or frame.session != first.session
            or frame.window != first.window
            or frame.global_index != first.global_index + index * stride
            or frame.window_index != first.window_index + index * stride
            or frame.window_count != first.window_count
        ):
            raise ValueError("non-strided block")
    return Frame(
        BLOCK_XOR,
        FLAG_GROUP_XOR_WHITENED,
        first.session,
        first.window,
        first.global_index,
        first.window_index,
        first.window_count,
        len(frames),
        stride,
        _xor_payloads(frames),
    )


def chain_blocks(frames: list[Frame], width: int) -> list[Frame]:
    if width < 2 or width > 64 or width & 1:
        raise ValueError("chain width")
    half = width // 2
    return [
        block_frame(frames[start : min(start + width, len(frames))])
        for start in range(0, len(frames), half)
        if start + half < len(frames)
    ]


def recover_block(
    parity: Frame, members: list[bytes | None], missing: int
) -> bytes:
    group = parity.flags == FLAG_GROUP_XOR_WHITENED
    if (
        parity.kind != BLOCK_XOR
        or parity.flags not in (FLAG_WHITENED, FLAG_GROUP_XOR_WHITENED)
        or (not group and parity.stream_offset)
        or (group and (not 1 <= parity.stream_id <= 3 or parity.stream_offset < 1))
        or parity.stream_id != len(members)
        or not 1 <= len(members) <= MAX_WINDOW
        or not 0 <= missing < len(members)
        or members[missing] is not None
    ):
        raise ValueError("block")
    recovered_padded = bytearray(parity.payload)
    for index, known in enumerate(members):
        if index == missing:
            continue
        if known is None or len(known) > len(recovered_padded):
            raise ValueError("block members")
        for byte_index, value in enumerate(known):
            recovered_padded[byte_index] ^= value
    if len(recovered_padded) < RECORD_HEADER:
        raise ValueError("block payload")
    total = RECORD_HEADER + struct.unpack_from(">I", recovered_padded, 16)[0]
    if total > len(recovered_padded) or any(recovered_padded[total:]):
        raise ValueError("block length/padding")
    recovered = bytes(recovered_padded[:total])
    Record.decode(recovered)
    return recovered


def metadata(
    attrs: int,
    date: int,
    time: int,
    path: str,
    size: int | None = None,
) -> bytes:
    encoded_path = safe_path(path).encode("utf-8")
    base = struct.pack(">BBHH", attrs, 0, date, time)
    if size is not None:
        return base + struct.pack(">IH", size, len(encoded_path)) + encoded_path
    return base + struct.pack(">H", len(encoded_path)) + encoded_path
