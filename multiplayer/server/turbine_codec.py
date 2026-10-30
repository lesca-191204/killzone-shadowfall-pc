"""Codec for the plaintext Turbine envelope verified in this eboot.

Static verification is not proof of an in-game session. See analysis/ for evidence.
"""
from dataclasses import dataclass
import struct


def crc32c_table():
    entries = []
    for value in range(256):
        for _ in range(8):
            value = (value >> 1) ^ (0x82F63B78 if value & 1 else 0)
        entries.append(value)
    return tuple(entries)


CRC32C_TABLE = crc32c_table()


def crc32c(data):
    value = 0xFFFFFFFF
    for byte in data:
        value = CRC32C_TABLE[(value ^ byte) & 0xFF] ^ (value >> 8)
    return value ^ 0xFFFFFFFF


@dataclass(frozen=True)
class Frame:
    message_type: int
    game_id: int
    client_index: int
    body: bytes = b""


def encode_frame(frame):
    # The checksum covers the seven host-order bytes, not the network header.
    host_fields = struct.pack("<HIB", frame.message_type, frame.game_id,
                              frame.client_index)
    return struct.pack(">HIBI", frame.message_type, frame.game_id,
                       frame.client_index, crc32c(host_fields)) + frame.body


def decode_frame(data):
    if len(data) < 11:
        raise ValueError("truncated Turbine header")
    kind, game_id, client, checksum = struct.unpack_from(">HIBI", data)
    expected = crc32c(struct.pack("<HIB", kind, game_id, client))
    if checksum != expected:
        raise ValueError("Turbine header checksum mismatch")
    return Frame(kind, game_id, client, bytes(data[11:]))


def encode_blob(data):
    return struct.pack(">H", len(data)) + data


class Reader:
    def __init__(self, data):
        self.data = data
        self.offset = 0

    def read(self, count):
        if count < 0 or self.offset + count > len(self.data):
            raise ValueError("truncated Turbine body")
        value = self.data[self.offset:self.offset + count]
        self.offset += count
        return value

    def number(self, fmt):
        return struct.unpack(fmt, self.read(struct.calcsize(fmt)))[0]

    def blob(self):
        return self.read(self.number(">H"))

    def finish(self):
        if self.offset != len(self.data):
            raise ValueError("unexpected trailing body bytes")


def decode_body(frame):
    """Strict decoder for verified body shapes; unknown messages remain opaque.

    These names describe the wire type, which differs from the send API enum
    for ENC_VERIFICATION (API enum 3, wire type 2).
    """
    reader = Reader(frame.body)
    kind = frame.message_type
    if kind in (1, 2, 4, 6, 0x1F, 0x21, 0x23):
        result = {}
    elif kind in (7, 0x0F, 0x17, 0x1E):
        result = {"data": reader.blob()}
    elif kind in (8, 0x10):
        result = {"value": reader.number(">H")}
    elif kind in (0x12, 0x16, 0x20):
        result = {"value": reader.number(">I")}
    elif kind == 3:
        result = {"user_data": reader.blob(), "client_info": reader.blob()}
    elif kind == 0x11:
        result = {"mss": reader.number(">H"),
                  "bandwidth": reader.number(">I"),
                  "user_data": reader.blob()}
    elif kind == 0x15:
        result = {"master_index": reader.number("B")}
    elif kind >= 0x28:
        result = {"broadcast": reader.number("B"),
                  "recipients": reader.blob(), "payload": reader.blob()}
    else:
        return {"opaque": frame.body}
    reader.finish()
    return result


def encode_game_body(broadcast, recipients, payload):
    if broadcast not in (False, True):
        raise ValueError("broadcast must be boolean")
    return bytes([int(broadcast)]) + encode_blob(recipients) + encode_blob(payload)


def encode_join_body(user_data, client_info):
    return encode_blob(user_data) + encode_blob(client_info)

